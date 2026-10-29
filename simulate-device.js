// Fake ESP32 for testing the dashboard without the hardware.
// Usage:  npm run simulate                 (sensor data only)
//         npm run simulate -- photo.jpg    (also sends a photo, so the AI check can run)

import fs from "node:fs";

const SERVER_URL = process.env.SERVER_URL || `http://localhost:${process.env.PORT || 3000}`;
const DEVICE_KEY = process.env.DEVICE_KEY || "change-me";
const photo = process.argv[2] ? fs.readFileSync(process.argv[2]) : Buffer.alloc(0);

const DEPTH = 50, WARN = 20, DANGER = 35;
let t = 0;

async function send() {
  t += 1;
  const water = Math.max(0, 22 + 18 * Math.sin(t / 15));   // slowly rises and falls
  const state = water >= DANGER ? "DANGER" : water >= WARN ? "WARNING" : "NORMAL";
  const params = new URLSearchParams({
    distance: (DEPTH - water).toFixed(1),
    water: water.toFixed(1),
    depth: DEPTH, warn: WARN, danger: DANGER,
    state, sensorOk: "1",
  });

  try {
    const res = await fetch(`${SERVER_URL}/api/device?${params}`, {
      method: "POST",
      headers: { "content-type": "image/jpeg", "x-device-key": DEVICE_KEY },
      body: photo,
    });
    const data = await res.json();
    console.log(`water ${water.toFixed(1)} cm | ${state} | server says AI: ${data.ai ?? data.error}`);
  } catch (err) {
    console.log("Cannot reach server:", err.message);
  }
}

console.log(`Simulating a device -> ${SERVER_URL}  (Ctrl+C to stop)`);
send();
setInterval(send, 1000);
