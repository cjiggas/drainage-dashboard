# Drainage Monitor Dashboard

Web dashboard for the ESP32-S3 drainage prototype. It shows the live camera photo, the AI result (CLEAR / BLOCKED + objects found), the HC-SR04 readings (water level, distance, alarm state, LED/buzzer status), and a table of recent readings.

```
ESP32-S3  --(sensor readings + photo every 1 s)-->  this server  --(photo)-->  Claude API
          <--------(AI result: CLEAR / BLOCKED)----              <--(answer)--
Browser   --(every 1 s)--> this server: /api/status, /api/frame.jpg
```

The ESP32 always **sends** data to the server, so the same setup works on your laptop (localhost) and on the internet (deployed). The Claude API key lives only on the server, never on the ESP32.

## Files

| File | What it is |
|---|---|
| `server.js` | Web server + AI check |
| `public/index.html` | The dashboard page |
| `simulate-device.js` | Fake ESP32, for testing without hardware |
| `esp32/drainage_device/drainage_device.ino` | Arduino sketch for the ESP32-S3 |
| `.env.example` | Settings template (copy to `.env`) |

## 1. Run it locally (same WiFi as the ESP32)

1. Install **Node.js LTS** from https://nodejs.org (version 20.6 or newer).
2. Open a terminal in this folder and run:
   ```
   npm install
   ```
3. Copy `.env.example` to `.env` and fill in `ANTHROPIC_API_KEY` and `DEVICE_KEY`.
4. Start the server:
   ```
   npm run dev
   ```
   It prints two addresses:
   ```
   On this computer:  http://localhost:3000
   On your WiFi:      http://192.168.1.21:3000   <- use this as SERVER_URL in the ESP32 sketch
   ```
5. If Windows asks whether to allow Node.js through the firewall, allow it on **Private networks**. Otherwise the ESP32 can't reach your laptop.
6. Open http://localhost:3000 in your browser.

### Test without the ESP32
In a second terminal:
```
npm run simulate                 # fake sensor data (water rises and falls)
npm run simulate -- photo.jpg    # also sends a photo, so "Check drain now" calls the AI
```

## 2. Set up the ESP32

1. In `esp32/drainage_device/`, copy `secrets.example.h` to `secrets.h`, then open `drainage_device.ino` in the Arduino IDE (`secrets.h` shows up as a second tab).
2. In `secrets.h`, fill in `WIFI_SSID`, `WIFI_PASSWORD`, `SERVER_URL` (the "On your WiFi" address) and `DEVICE_KEY` (same as in `.env`). `secrets.h` is ignored by git, so it never ends up on GitHub.
3. Tools: Board **ESP32S3 Dev Module**, PSRAM **OPI PSRAM**, Partition Scheme **16M Flash (3MB APP/9.9MB FATFS)**. Library **ArduinoJson** installed.
4. Upload. The Serial Monitor (115200) should show `Server: OK`, and the dashboard turns **Device online**.

## 3. Deploy it online (Render, free)

1. Put this folder in a GitHub repository. `.env` and `node_modules` are already ignored, so your key is not uploaded.
2. On https://render.com: **New → Blueprint** → connect GitHub → pick the repository. Render reads `render.yaml` and sets up the free web service for you.
3. Render asks for `ANTHROPIC_API_KEY` and `DEVICE_KEY`. Enter them, then click **Apply**.
4. Wait for the deploy to finish. You get a URL like `https://drainage-dashboard.onrender.com`.
5. In `secrets.h`, set `SERVER_URL` to that URL; in the sketch, set `UPLOAD_INTERVAL_MS` to `2000`. Upload again.

Notes about the free plan: the server sleeps after about 15 minutes without visitors, and the first visit takes about a minute to wake it up. Data is kept in memory, so it resets when the server restarts. Open the site a few minutes before your presentation.

## Settings (`.env` or Render environment)

| Name | Meaning |
|---|---|
| `ANTHROPIC_API_KEY` | Claude API key from https://platform.claude.com |
| `DEVICE_KEY` | Shared password; must match the ESP32 sketch |
| `AI_INTERVAL_SECONDS` | Automatic AI check interval while the device is online (default 30, `0` = button only) |
| `PORT` | Local port (default 3000; Render sets this automatically) |

Each AI check costs a fraction of a cent, but automatic checks add up if the device runs for hours. Set a monthly spend limit in the Claude Console, and use `AI_INTERVAL_SECONDS=0` when you only need the button.

## Troubleshooting

| Problem | Fix |
|---|---|
| Dashboard says **Device offline** | Check `SERVER_URL` in the sketch, that the laptop and ESP32 are on the same WiFi, and the Windows firewall (step 1.5) |
| Serial Monitor: `Upload failed: 401` | `DEVICE_KEY` in the sketch doesn't match the server |
| Serial Monitor: `Upload failed: -1` | Server not reachable: wrong IP/port, server not running, or firewall |
| "AI is disabled" on the page | `ANTHROPIC_API_KEY` is missing on the server |
| "Invalid API key" | Wrong key, or no credit in the Claude Console |
| Camera shows nothing | `Camera init FAILED` in the Serial Monitor means the camera pins don't match your board |
