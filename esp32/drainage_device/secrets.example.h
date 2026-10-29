// Copy this file to secrets.h (same folder) and fill in your values.
// secrets.h is ignored by git, so your WiFi password and device key stay private.
#pragma once

const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
// Local:    "http://192.168.1.10:3000"   (the "On your WiFi" address printed by the server)
// Deployed: "https://your-app.onrender.com"
const char* SERVER_URL    = "http://192.168.1.10:3000";
const char* DEVICE_KEY    = "change-me-to-something-random";   // same as DEVICE_KEY on the server
