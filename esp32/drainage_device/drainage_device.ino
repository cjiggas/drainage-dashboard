// Drainage Prototype - ESP32-S3 device for the web dashboard
// ESP32-S3 CAM | HC-SR04 (TRIG 21, ECHO 14) | LED 2 | Buzzer 1
//
// Reads the water level, drives the LED/buzzer, and sends the sensor readings +
// a camera photo to the dashboard server (/api/device) a few times per second.
// The server runs the AI check and replies with CLEAR / BLOCKED.
// It also serves a smooth live video stream on this WiFi: http://<board-ip>:81/stream

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include "esp_http_server.h"

// WiFi name/password, SERVER_URL and DEVICE_KEY live in secrets.h (not uploaded to GitHub).
// First time: copy secrets.example.h to secrets.h and fill it in.
#include "secrets.h"

const unsigned long UPLOAD_INTERVAL_MS = 500;   // photos sent to the server (try 1000 on a weak signal)

// ---------- Pins ----------
const int TRIG_PIN   = 21;
const int ECHO_PIN   = 14;
const int LED_PIN    = 2;
const int BUZZER_PIN = 1;

// ---------- Calibration ----------
const float SENSOR_TO_BOTTOM_CM = 50.0;   // distance sensor -> empty drain bottom
const float WARNING_LEVEL_CM    = 20.0;   // water height for WARNING
const float DANGER_LEVEL_CM     = 35.0;   // water height for DANGER

// ---------- Camera pins (ESP32S3_EYE layout, used by most ESP32-S3 CAM boards) ----------
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5
#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM    8
#define Y3_GPIO_NUM    9
#define Y2_GPIO_NUM    11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13

enum State    { NORMAL, WARNING, DANGER, SENSOR_ERROR };
enum AiResult { AI_UNKNOWN, AI_CLEAR, AI_BLOCKED };
const char* STATE_NAMES[] = { "NORMAL", "WARNING", "DANGER", "SENSOR_ERROR" };
const char* AI_NAMES[]    = { "UNKNOWN", "CLEAR", "BLOCKED" };

// Written by loop(), read by the network task (and the other way round for aiResult)
volatile State    state    = NORMAL;
volatile AiResult aiResult = AI_UNKNOWN;
volatile float    waterLevel = 0;
volatile float    distanceCm = -1;
volatile bool     serverOk = false;

const int SAMPLES = 5;
float samples[SAMPLES];
int sampleCount = 0, sampleIndex = 0, failCount = 0;
unsigned long lastRead = 0, lastPrint = 0;

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  setupCamera();
  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) startStreamServer();

  // Network work runs on core 0 so the sensor + alarm on core 1 never freeze
  xTaskCreatePinnedToCore(networkTask, "network", 20480, NULL, 1, NULL, 0);
}

void loop() {
  if (millis() - lastRead >= 100) {
    lastRead = millis();
    updateSensor();
  }
  updateState();
  updateOutputs();

  if (millis() - lastPrint >= 1000) {
    lastPrint = millis();
    Serial.printf("Water: %.1f cm | Distance: %.1f cm | AI: %s | State: %s | Server: %s\n",
                  waterLevel, distanceCm, AI_NAMES[aiResult], STATE_NAMES[state],
                  serverOk ? "OK" : "not connected");
  }
}

// ---------------- Camera ----------------
void setupCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_CIF;      // 400x296: small photos upload fast (VGA = 640x480 is sharper but slower)
  config.jpeg_quality = 15;                 // higher number = smaller file
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) Serial.printf("Camera init FAILED (0x%x). Check camera pins and PSRAM setting.\n", err);
  else               Serial.println("Camera OK.");
}

// ---------------- WiFi ----------------
void connectWiFi() {
  Serial.printf("Connecting to WiFi '%s'", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) Serial.printf("\nWiFi OK. IP: %s\n", WiFi.localIP().toString().c_str());
  else                               Serial.println("\nWiFi FAILED. Check name/password (2.4 GHz only).");
}

// ---------------- Sending data to the server ----------------
void networkTask(void* param) {
  int failures = 0;
  bool secure = String(SERVER_URL).startsWith("https");

  // Created ONCE and kept open: the slow secure handshake only happens on (re)connect,
  // not for every photo. This is the biggest speed-up for the online dashboard.
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();   // skips certificate check - fine for a prototype
  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(15000);

  while (true) {
    unsigned long started = millis();

    if (WiFi.status() != WL_CONNECTED) {
      serverOk = false;
      secureClient.stop();
      plainClient.stop();
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    char url[300];
    snprintf(url, sizeof(url),
             "%s/api/device?distance=%.1f&water=%.1f&depth=%.1f&warn=%.1f&danger=%.1f&state=%s&sensorOk=%d&ip=%s",
             SERVER_URL, distanceCm, waterLevel, SENSOR_TO_BOTTOM_CM, WARNING_LEVEL_CM,
             DANGER_LEVEL_CM, STATE_NAMES[state], state == SENSOR_ERROR ? 0 : 1,
             WiFi.localIP().toString().c_str());

    camera_fb_t* fb = esp_camera_fb_get();

    if (secure) http.begin(secureClient, url);
    else        http.begin(plainClient, url);
    http.addHeader("content-type", "image/jpeg");
    http.addHeader("x-device-key", DEVICE_KEY);

    int code = fb ? http.POST(fb->buf, fb->len) : http.POST((uint8_t*)"", 0);
    if (fb) esp_camera_fb_return(fb);

    if (code == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, http.getString())) {
        const char* r = doc["ai"] | "UNKNOWN";
        if      (strcmp(r, "BLOCKED") == 0) aiResult = AI_BLOCKED;
        else if (strcmp(r, "CLEAR")   == 0) aiResult = AI_CLEAR;
        else                                aiResult = AI_UNKNOWN;
      }
      serverOk = true;
      failures = 0;
    } else {
      Serial.printf("[NET] Upload failed: %d %s\n", code,
                    code == 401 ? "(DEVICE_KEY does not match the server)" : "");
      serverOk = false;
      if (++failures >= 3) aiResult = AI_UNKNOWN;   // don't trust an old AI result
      secureClient.stop();                          // drop a possibly dead connection, reconnect next time
      plainClient.stop();
    }
    http.end();

    unsigned long spent = millis() - started;
    if (spent < UPLOAD_INTERVAL_MS) vTaskDelay(pdMS_TO_TICKS(UPLOAD_INTERVAL_MS - spent));
  }
}

// ---------------- Live video stream (same WiFi only) ----------------
// Open http://<board-ip>:81/stream in a browser on the same WiFi. The dashboard shows a
// "Live view" button for it. This is smooth video; the online dashboard shows photos.
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t streamHandler(httpd_req_t* req) {
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  char part[64];
  while (true) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return ESP_FAIL;
    size_t hlen = snprintf(part, sizeof(part), STREAM_PART, (unsigned)fb->len);
    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;            // viewer closed the page
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return res;
}

void startStreamServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81;
  config.stack_size  = 8192;
  httpd_uri_t streamUri = { .uri = "/stream", .method = HTTP_GET, .handler = streamHandler, .user_ctx = NULL };
  httpd_handle_t server = NULL;
  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &streamUri);
    Serial.printf("Live video on this WiFi: http://%s:81/stream\n", WiFi.localIP().toString().c_str());
  }
}

// ---------------- HC-SR04 ----------------
float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long us = pulseIn(ECHO_PIN, HIGH, 30000);
  if (us == 0) return -1;
  return us * 0.0343 / 2.0;
}

float median() {
  float tmp[SAMPLES];
  for (int i = 0; i < sampleCount; i++) tmp[i] = samples[i];
  for (int i = 1; i < sampleCount; i++) {
    float k = tmp[i]; int j = i - 1;
    while (j >= 0 && tmp[j] > k) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = k;
  }
  return tmp[sampleCount / 2];
}

void updateSensor() {
  float d = readDistanceCm();
  if (d < 0) { failCount++; return; }
  failCount = 0;
  samples[sampleIndex] = d;
  sampleIndex = (sampleIndex + 1) % SAMPLES;
  if (sampleCount < SAMPLES) sampleCount++;
  float dist = median();
  distanceCm = dist;
  float level = SENSOR_TO_BOTTOM_CM - dist;
  waterLevel = level < 0 ? 0 : level;
}

// ---------------- Decision logic ----------------
void updateState() {
  if (failCount >= 10) { state = SENSOR_ERROR; return; }
  bool high    = waterLevel >= WARNING_LEVEL_CM;
  bool blocked = aiResult == AI_BLOCKED;

  if (waterLevel >= DANGER_LEVEL_CM || (high && blocked)) state = DANGER;
  else if (high || blocked)                               state = WARNING;
  else                                                    state = NORMAL;
}

void updateOutputs() {
  unsigned long t = millis();
  switch (state) {
    case NORMAL:       digitalWrite(LED_PIN, LOW);              digitalWrite(BUZZER_PIN, LOW); break;
    case WARNING:      digitalWrite(LED_PIN, (t % 1000) < 500); digitalWrite(BUZZER_PIN, LOW); break;
    case DANGER:       digitalWrite(LED_PIN, HIGH);             digitalWrite(BUZZER_PIN, (t % 400) < 200); break;
    case SENSOR_ERROR: digitalWrite(LED_PIN, (t % 200) < 100);  digitalWrite(BUZZER_PIN, LOW); break;
  }
}
