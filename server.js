// Drainage Monitor - web server
// The ESP32 sends its sensor readings + a camera photo to POST /api/device.
// This server keeps the latest data, asks Claude whether the drain is CLEAR or BLOCKED,
// sends the AI result back to the ESP32, and serves the dashboard in /public.

import express from "express";
import Anthropic from "@anthropic-ai/sdk";
import crypto from "node:crypto";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));

const PORT = Number(process.env.PORT) || 3000;
const DEVICE_KEY = process.env.DEVICE_KEY || "change-me";
const AI_INTERVAL_MS = Number(process.env.AI_INTERVAL_SECONDS ?? 30) * 1000;
const OFFLINE_AFTER_MS = 10000;
const MANUAL_CHECK_COOLDOWN_MS = 5000;

const MODEL = "claude-opus-5-5";
const PROMPT =
  "This photo shows a small DIY drainage channel. Look for any objects or trash in it " +
  "(leaves, plastic, bottles, paper, rocks, etc.). Reply on ONE line in exactly this format: " +
  "STATUS | objects. STATUS is CLEAR if water can flow freely, or BLOCKED if objects are " +
  "blocking or clogging the drain. objects is a short comma-separated list of what you see " +
  "in the drain, or none. Example: BLOCKED | plastic bottle, leaves";

const STATES = ["NORMAL", "WARNING", "DANGER", "SENSOR_ERROR"];

let client = null;
if (process.env.ANTHROPIC_API_KEY) {
  client = new Anthropic();
} else {
  console.warn("ANTHROPIC_API_KEY is not set - AI checks are disabled.");
}

// ---------------- Latest data (kept in memory) ----------------
const device = {
  lastSeen: 0,
  distance: null,
  water: null,
  depth: null,
  warn: null,
  danger: null,
  state: "UNKNOWN",
  sensorOk: false,
};
let frame = null;     // latest camera photo (JPEG buffer)
let frameAt = 0;
let aiFrame = null;   // the photo the AI last analyzed
const ai = {
  result: "UNKNOWN",
  objects: "",
  checking: false,
  checkedAt: 0,
  ms: 0,
  error: null,
  model: null,
};
const history = [];   // recent readings for the table
let lastManualCheck = 0;

// ---------------- AI check ----------------
async function runAiCheck() {
  if (!client || !frame || ai.checking) return;
  ai.checking = true;
  aiFrame = frame;
  const started = Date.now();

  try {
    const response = await client.beta.messages.create({
      model: MODEL,
      max_tokens: 1024,
      betas: ["server-side-fallback-2026-07-01"],
      fallbacks: "default",
      output_config: { effort: "low" },
      messages: [
        {
          role: "user",
          content: [
            {
              type: "image",
              source: { type: "base64", media_type: "image/jpeg", data: aiFrame.toString("base64") },
            },
            { type: "text", text: PROMPT },
          ],
        },
      ],
    });

    ai.model = response.model;
    if (response.stop_reason === "refusal") {
      ai.result = "UNKNOWN";
      ai.objects = "";
      ai.error = "The AI declined this request";
    } else {
      const text = response.content
        .filter((block) => block.type === "text")
        .map((block) => block.text)
        .join("")
        .trim();
      const [status = "", objects = ""] = text.split("|");
      const upper = status.toUpperCase();
      ai.result = upper.includes("BLOCKED") ? "BLOCKED" : upper.includes("CLEAR") ? "CLEAR" : "UNKNOWN";
      ai.objects = objects.trim().slice(0, 120);
      ai.error = ai.result === "UNKNOWN" ? `Unexpected reply: ${text.slice(0, 80)}` : null;
      console.log(`[AI] ${text}`);
    }
  } catch (err) {
    ai.result = "UNKNOWN";
    ai.objects = "";
    if (err instanceof Anthropic.AuthenticationError) ai.error = "Invalid API key";
    else if (err instanceof Anthropic.RateLimitError) ai.error = "Rate limited - try again later";
    else if (err instanceof Anthropic.APIError) ai.error = `AI service error (${err.status}): ${err.message}`;
    else ai.error = "Could not reach the AI service";
    console.error("[AI] Error:", err.message);
  } finally {
    ai.checking = false;
    ai.checkedAt = Date.now();
    ai.ms = ai.checkedAt - started;
  }
}

function addHistory() {
  const last = history[history.length - 1];
  const now = Date.now();
  const changed = last && (last.state !== device.state || last.ai !== ai.result);
  if (last && !changed && now - last.t < 5000) return;
  history.push({ t: now, distance: device.distance, water: device.water, state: device.state, ai: ai.result });
  if (history.length > 30) history.shift();
}

function deviceKeyOk(req) {
  const given = Buffer.from(String(req.get("x-device-key") || ""));
  const expected = Buffer.from(DEVICE_KEY);
  return given.length === expected.length && crypto.timingSafeEqual(given, expected);
}

function num(value) {
  const n = Number.parseFloat(value);
  return Number.isFinite(n) ? n : null;
}

// ---------------- Routes ----------------
const app = express();
app.use(express.static(path.join(here, "public")));

// ESP32 -> server: sensor readings in the query string, camera photo (JPEG) as the body
app.post("/api/device", express.raw({ type: "image/jpeg", limit: "3mb" }), (req, res) => {
  if (!deviceKeyOk(req)) return res.status(401).json({ error: "wrong device key" });

  device.lastSeen = Date.now();
  device.distance = num(req.query.distance);
  device.water = num(req.query.water);
  device.depth = num(req.query.depth);
  device.warn = num(req.query.warn);
  device.danger = num(req.query.danger);
  device.state = STATES.includes(req.query.state) ? req.query.state : "UNKNOWN";
  device.sensorOk = req.query.sensorOk === "1";

  if (Buffer.isBuffer(req.body) && req.body.length > 0) {
    frame = req.body;
    frameAt = Date.now();
  }
  addHistory();

  if (AI_INTERVAL_MS > 0 && Date.now() - ai.checkedAt >= AI_INTERVAL_MS) runAiCheck();

  res.json({ ai: ai.result, objects: ai.objects, checking: ai.checking });
});

// Dashboard -> server: everything the page shows
app.get("/api/status", (req, res) => {
  const now = Date.now();
  res.set("Cache-Control", "no-store");
  res.json({
    device: {
      ...device,
      online: now - device.lastSeen < OFFLINE_AFTER_MS,
      lastSeenAgo: device.lastSeen ? Math.round((now - device.lastSeen) / 1000) : null,
    },
    ai: {
      ...ai,
      enabled: Boolean(client),
      checkedAgo: ai.checkedAt ? Math.round((now - ai.checkedAt) / 1000) : null,
      hasPhoto: Boolean(aiFrame),
    },
    frameAt,
    history: [...history].reverse(),
  });
});

app.get("/api/frame.jpg", (req, res) => {
  if (!frame) return res.status(404).end();
  res.set("Cache-Control", "no-store").type("image/jpeg").send(frame);
});

app.get("/api/ai-frame.jpg", (req, res) => {
  if (!aiFrame) return res.status(404).end();
  res.set("Cache-Control", "no-store").type("image/jpeg").send(aiFrame);
});

// "Check drain now" button
app.post("/api/check", (req, res) => {
  if (!client) return res.status(503).json({ error: "AI is disabled (no API key on the server)" });
  if (!frame) return res.status(400).json({ error: "No camera photo yet" });
  if (ai.checking) return res.status(409).json({ error: "Already checking" });
  if (Date.now() - lastManualCheck < MANUAL_CHECK_COOLDOWN_MS) {
    return res.status(429).json({ error: "Wait a few seconds" });
  }
  lastManualCheck = Date.now();
  runAiCheck();
  res.json({ started: true });
});

app.listen(PORT, "0.0.0.0", () => {
  console.log(`Drainage dashboard running.`);
  console.log(`  On this computer:  http://localhost:${PORT}`);
  for (const addrs of Object.values(os.networkInterfaces())) {
    for (const a of addrs ?? []) {
      if (a.family === "IPv4" && !a.internal) {
        console.log(`  On your WiFi:      http://${a.address}:${PORT}   <- use this as SERVER_URL in the ESP32 sketch`);
      }
    }
  }
});
