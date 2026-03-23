import "./env.js";
import express from "express";
import cors from "cors";
import mqtt from "mqtt";
import { StateManager } from "./state.js";
import { generateCommentary } from "./commentary.js";
import { generateSpeech, AUDIO_DIR } from "./ttsService.js";
import os from "os";
import path from "path";
import fs from "fs";
import { fileURLToPath } from "url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const app = express();
const PORT = Number(process.env.PORT) || 3001;

app.use(cors());
app.use(express.json());
app.use("/audio", express.static(AUDIO_DIR));

const DIST_DIR = path.join(__dirname, "..", "dist");
app.use(express.static(DIST_DIR));

// ---------------------------------------------------------------------------
// MQTT & provisioning (from environment — copy backend/.env.example to .env)
// ---------------------------------------------------------------------------
const MQTT_PORT_TLS = Number(process.env.MQTT_PORT_TLS) || 8883;
const MQTT_HOST = (process.env.MQTT_HOST || "").trim();
const MQTT_BROKER =
  (process.env.MQTT_BROKER || "").trim() ||
  (MQTT_HOST ? `mqtts://${MQTT_HOST}:${MQTT_PORT_TLS}` : "");
const MQTT_USERNAME = process.env.MQTT_USERNAME || "";
const MQTT_PASSWORD = process.env.MQTT_PASSWORD || "";

const PROVISION_TOKEN = process.env.PROVISION_TOKEN || "";

function requireEnv(label, value) {
  if (typeof value === "string" && value.trim()) return;
  console.error(
    `\nMissing required env: ${label}\nCopy backend/.env.example to backend/.env and set values.\n`,
  );
  process.exit(1);
}

requireEnv("MQTT_BROKER or MQTT_HOST", MQTT_BROKER);
requireEnv("MQTT_USERNAME", MQTT_USERNAME);
requireEnv("MQTT_PASSWORD", MQTT_PASSWORD);
requireEnv("PROVISION_TOKEN", PROVISION_TOKEN);

// Wildcard topic — subscribe to ALL devices
const STATE_TOPIC_WILDCARD = "playarka/device/+/state";

// ---------------------------------------------------------------------------
// Multi-device state & device registry (in-memory)
// ---------------------------------------------------------------------------
const stateManagers = new Map(); // deviceId -> StateManager
const deviceRegistry = new Map(); // mac -> { deviceId, deviceName, mac, provisionedAt }
const streakTrackers = new Map(); // deviceId -> { playerIndex: { strikes: number, spares: number } }
let nextDeviceId = 1;

function getStateManager(deviceId) {
  if (!stateManagers.has(deviceId)) {
    stateManagers.set(deviceId, new StateManager());
  }
  return stateManagers.get(deviceId);
}

function getStreakTracker(deviceId) {
  if (!streakTrackers.has(deviceId)) {
    streakTrackers.set(deviceId, {});
  }
  return streakTrackers.get(deviceId);
}

function updateStreak(deviceId, playerIndex, isStrike, isSpare) {
  const tracker = getStreakTracker(deviceId);
  if (!tracker[playerIndex]) tracker[playerIndex] = { strikes: 0, spares: 0 };

  if (isStrike) {
    tracker[playerIndex].strikes += 1;
    tracker[playerIndex].spares = 0;
  } else if (isSpare) {
    tracker[playerIndex].spares += 1;
    tracker[playerIndex].strikes = 0;
  } else {
    tracker[playerIndex].strikes = 0;
    tracker[playerIndex].spares = 0;
  }
  return tracker[playerIndex];
}

function buildAllPlayersContext(scores) {
  return (scores || []).map((s) => ({
    name: s.name,
    totalScore: s.totalScore || 0,
  }));
}

/** Running totals through each frame + standings — for AI commentary vs rivals */
function buildMatchContextString(scores) {
  if (!scores?.length) return "";
  const lines = [];
  for (const s of scores) {
    const parts = (s.frames || [])
      .map((f, i) => {
        if (f.cumulative == null) return null;
        return `F${i + 1}:${f.cumulative}`;
      })
      .filter(Boolean);
    lines.push(
      `${s.name}: after each finished frame — ${parts.join(", ")} | total now ${s.totalScore}`,
    );
  }
  if (scores.length > 1) {
    const sorted = [...scores].sort((a, b) => b.totalScore - a.totalScore);
    const last = sorted[sorted.length - 1];
    const spread = sorted[0].totalScore - last.totalScore;
    lines.push(
      `Standings: leader ${sorted[0].name} ${sorted[0].totalScore}, trailing ${last.name} ${last.totalScore} (${spread} pt spread)`,
    );
  }
  return lines.join("\n");
}

function deviceTopicFor(deviceId) {
  return `playarka/device/${deviceId}/state`;
}

/** Extract deviceId from a topic like "playarka/device/3/state" */
function deviceIdFromTopic(topic) {
  const m = topic.match(/^playarka\/device\/([^/]+)\/state$/);
  return m ? m[1] : null;
}

// ---------------------------------------------------------------------------
// MQTT Client
// ---------------------------------------------------------------------------
const mqttOptions = {
  clientId: `playarka_backend_${Date.now()}`,
  clean: true,
  reconnectPeriod: 5000,
  connectTimeout: 10000,
  username: MQTT_USERNAME,
  password: MQTT_PASSWORD,
  rejectUnauthorized: false,
};

const mqttClient = mqtt.connect(MQTT_BROKER, mqttOptions);

mqttClient.on("connect", () => {
  console.log("✅ MQTT Connected to broker");
  mqttClient.subscribe(STATE_TOPIC_WILDCARD, { qos: 1 }, (err) => {
    if (err) {
      console.error("❌ Failed to subscribe:", err);
    } else {
      console.log(`✅ Subscribed to ${STATE_TOPIC_WILDCARD}`);
    }
  });
});

mqttClient.on("error", (err) => {
  console.error("❌ MQTT Error:", err.message);
});

mqttClient.on("close", () => {
  console.log("⚠️  MQTT Connection closed");
});

mqttClient.on("reconnect", () => {
  console.log("🔄 Reconnecting to MQTT broker...");
});

mqttClient.on("message", (topic, message) => {
  try {
    const data = JSON.parse(message.toString());
    handleMqttMessage(topic, data);
  } catch (error) {
    console.error("❌ Error parsing MQTT message:", error);
  }
});

// ---------------------------------------------------------------------------
// MQTT message handler (multi-device aware)
// ---------------------------------------------------------------------------
function handleMqttMessage(topic, data) {
  const deviceId = deviceIdFromTopic(topic);
  if (!deviceId) return;

  if (data.type === "SCORE") {
    handleScoreFromEsp(deviceId, data);
    return;
  }
  if (data.type === "HEARTBEAT") {
    console.log(`💓 HEARTBEAT device=${deviceId}:`, JSON.stringify(data));
    return;
  }
  if (data.type === "PIN_ERROR") {
    handlePinError(deviceId, data);
    return;
  }
  if (data.action === "pinError") return; // skip our own re-published message
  if (data.action === "update") return;

  console.log(
    `📥 [device=${deviceId}] action="${data.action}"`,
    JSON.stringify(data),
  );

  switch (data.action) {
    case "scan":
      handleScan(deviceId);
      break;
    case "addPlayer":
      handleAddPlayer(deviceId, data.name);
      break;
    case "ready":
      handleReady(deviceId);
      break;
    case "requestPayment":
      handleRequestPayment(deviceId, data.amount);
      break;
    case "paymentStatus":
      if (data.status) {
        handlePaymentStatus(deviceId, data.status);
      }
      break;
    case "reset":
      handleReset(deviceId);
      break;
    default:
      console.log(`⚠️  Unknown action "${data.action}" on device=${deviceId}`);
  }
}

// ---------------------------------------------------------------------------
// Action handlers (all device-aware)
// ---------------------------------------------------------------------------
function handleScan(deviceId) {
  console.log(`📱 [device=${deviceId}] QR scanned`);
  const sm = getStateManager(deviceId);
  sm.setState({ status: "onboarding" });
  publishState(deviceId);
}

function handleAddPlayer(deviceId, name) {
  console.log(`➕ [device=${deviceId}] Adding player: ${name}`);
  const sm = getStateManager(deviceId);
  if (sm.getPlayers().length >= 6) return;
  sm.addPlayer(name);
  publishState(deviceId);
}

function handleReady(deviceId) {
  console.log(`✅ [device=${deviceId}] Players ready`);
  const sm = getStateManager(deviceId);
  sm.setState({ ready: true, status: "payment" });
  publishState(deviceId);
}

async function handleRequestPayment(deviceId, amount) {
  console.log(`💳 [device=${deviceId}] Payment requested: ₹${amount}`);
  const paymentQr = `upi://pay?pa=merchant@upi&pn=PlayArka&am=${amount}&cu=INR`;
  const sm = getStateManager(deviceId);
  sm.setState({
    status: "payment",
    paymentQr,
    paymentStatus: "pending",
    amount,
  });
  publishState(deviceId);
}

function handlePaymentStatus(deviceId, status) {
  console.log(`💳 [device=${deviceId}] Payment status: ${status}`);
  const sm = getStateManager(deviceId);
  sm.setState({
    paymentStatus: status,
    status: status === "success" ? "active" : "payment",
  });
  publishState(deviceId);
  if (status === "success") {
    startGame(deviceId);
  }
}

function handleReset(deviceId) {
  console.log(`🔄 [device=${deviceId}] Reset`);
  const sm = getStateManager(deviceId);
  sm.reset();
  streakTrackers.delete(deviceId);
  publishState(deviceId);
}

function handlePinError(deviceId, data) {
  const pin = data.pin || 0;
  console.log(
    `🚨 [device=${deviceId}] PIN ERROR: Pin ${pin} stuck after 3 recovery attempts`,
  );

  const topic = deviceTopicFor(deviceId);
  mqttClient.publish(
    topic,
    JSON.stringify({
      action: "pinError",
      pin,
      message: `Pin ${pin} is stuck after 3 recovery attempts. Please check the lane.`,
    }),
    { qos: 1 },
  );
}

// ---------------------------------------------------------------------------
// TTS Announcement (fire-and-forget, never blocks game flow)
// ---------------------------------------------------------------------------
async function announceEvent(deviceId, ctx) {
  try {
    const { text, mood } = await generateCommentary(ctx);
    if (!text) return;
    console.log(`🔊 [device=${deviceId}] TTS (${mood}): "${text}"`);

    const filename = await generateSpeech(text, mood);
    const baseUrl = `http://${getLocalIP()}:${PORT}`;
    const audioUrl = `${baseUrl}/audio/${filename}`;

    const topic = deviceTopicFor(deviceId);
    mqttClient.publish(
      topic,
      JSON.stringify({ action: "announce", audioUrl, text, mood }),
      { qos: 1 },
    );
    console.log(`📢 [device=${deviceId}] Audio published: ${audioUrl}`);
  } catch (err) {
    console.error(
      `⚠️  [device=${deviceId}] TTS error (non-fatal):`,
      err.message,
    );
  }
}

// ---------------------------------------------------------------------------
// Score handling from ESP
// ---------------------------------------------------------------------------
function handleScoreFromEsp(deviceId, data) {
  try {
    const sm = getStateManager(deviceId);
    const state = sm.getState();
    const players = state.players || [];
    if (players.length === 0) return;

    const fallenPins = Array.isArray(data.fallenPins) ? data.fallenPins : [];
    const remainingPins = Array.isArray(data.remainingPins)
      ? data.remainingPins
      : [];
    const TOTAL_PINS_PER_FRAME = 8;
    const totalFallen = fallenPins.length;

    let currentPlayer =
      typeof state.currentPlayer === "number" ? state.currentPlayer : 0;
    let frame = typeof state.frame === "number" ? state.frame : 1;
    let roll = typeof state.roll === "number" ? state.roll : 1;

    if (frame < 1) frame = 1;
    if (frame > 10) frame = 10;
    if (roll < 1) roll = 1;
    if (roll > 3) roll = 3;
    if (currentPlayer < 0) currentPlayer = 0;
    if (currentPlayer >= players.length) currentPlayer = players.length - 1;

    const scores =
      state.scores && Array.isArray(state.scores) ? state.scores.slice() : [];

    while (scores.length < players.length) {
      const p = players[scores.length];
      const name = typeof p === "string" ? p : p.name;
      scores.push({
        name,
        frames: Array.from({ length: 10 }, () => ({
          ball1: null,
          ball2: null,
          ball3: null,
          cumulative: null,
        })),
        totalScore: 0,
        maxScore: 280,
      });
    }

    const playerScore = scores[currentPlayer];
    const frameIndex = frame - 1;
    const frameScore = playerScore.frames[frameIndex];

    let pinsThisRoll = 0;
    if (roll === 1) {
      pinsThisRoll = totalFallen;
    } else if (roll === 2) {
      const prev = typeof frameScore.ball1 === "number" ? frameScore.ball1 : 0;
      pinsThisRoll = Math.max(0, totalFallen - prev);
    } else {
      const prev1 = typeof frameScore.ball1 === "number" ? frameScore.ball1 : 0;
      const prev2 = typeof frameScore.ball2 === "number" ? frameScore.ball2 : 0;
      pinsThisRoll = Math.max(0, totalFallen - prev1 - prev2);
    }

    let scoreThisRoll = pinsThisRoll;
    let isStrike = false;
    let isSpare = false;

    if (totalFallen >= TOTAL_PINS_PER_FRAME) {
      if (roll === 1) {
        isStrike = true;
        scoreThisRoll = 20 + TOTAL_PINS_PER_FRAME;
      } else if (roll === 2) {
        isSpare = true;
        scoreThisRoll = pinsThisRoll + 10;
      }
    }

    if (roll === 1) frameScore.ball1 = scoreThisRoll;
    else if (roll === 2) frameScore.ball2 = scoreThisRoll;
    else frameScore.ball3 = scoreThisRoll;

    let total = 0;
    playerScore.frames.forEach((f) => {
      total +=
        (typeof f.ball1 === "number" ? f.ball1 : 0) +
        (typeof f.ball2 === "number" ? f.ball2 : 0) +
        (typeof f.ball3 === "number" ? f.ball3 : 0);
      f.cumulative = total > 0 ? total : null;
    });
    playerScore.totalScore = total;

    let nextPlayer = currentPlayer;
    let nextFrame = frame;
    let nextRoll = roll + 1;

    if (isStrike || isSpare) {
      nextRoll = 1;
      if (nextPlayer < players.length - 1) {
        nextPlayer += 1;
      } else {
        nextPlayer = 0;
        if (nextFrame < 10) nextFrame += 1;
        else state.status = "completed";
      }
    } else if (nextRoll > 3) {
      nextRoll = 1;
      if (nextPlayer < players.length - 1) {
        nextPlayer += 1;
      } else {
        nextPlayer = 0;
        if (nextFrame < 10) nextFrame += 1;
        else state.status = "completed";
      }
    }

    state.currentPlayer = nextPlayer;
    state.frame = nextFrame;
    state.roll = nextRoll;
    state.scores = scores;

    const prevPlayer = currentPlayer;
    state.currentPlayer = nextPlayer;
    state.frame = nextFrame;
    state.roll = nextRoll;
    state.scores = scores;

    sm.setState(state);
    publishState(deviceId);

    // --- TTS announcements (fire-and-forget, never blocks) ---
    const playerName =
      typeof players[prevPlayer] === "string"
        ? players[prevPlayer]
        : players[prevPlayer]?.name || "Bowler";

    const streakInfo = updateStreak(deviceId, prevPlayer, isStrike, isSpare);
    const allPlayers = buildAllPlayersContext(scores);
    const matchContext = buildMatchContextString(scores);

    announceEvent(deviceId, {
      event: "score",
      player: playerName,
      pins: pinsThisRoll,
      isStrike,
      isSpare,
      frame,
      roll,
      streak: streakInfo.strikes,
      spareStreak: streakInfo.spares,
      allPlayers,
      matchContext,
    }).then(() => {
      if (nextPlayer !== prevPlayer && state.status !== "completed") {
        const nextName =
          typeof players[nextPlayer] === "string"
            ? players[nextPlayer]
            : players[nextPlayer]?.name || "Bowler";
        return announceEvent(deviceId, {
          event: "nextPlayer",
          player: nextName,
          frame: nextFrame,
          allPlayers,
          matchContext,
        });
      }
    });

    if (state.status === "completed") {
      const topic = deviceTopicFor(deviceId);
      mqttClient.publish(topic, JSON.stringify({ action: "gameOver" }), {
        qos: 1,
        retain: true,
      });
      console.log(`📤 [device=${deviceId}] gameOver published`);

      const winner = scores.reduce(
        (best, s) => (s.totalScore > best.totalScore ? s : best),
        scores[0],
      );
      announceEvent(deviceId, {
        event: "gameOver",
        player: winner.name,
        score: winner.totalScore,
        allPlayers: buildAllPlayersContext(scores),
        matchContext: buildMatchContextString(scores),
      });
      streakTrackers.delete(deviceId);
    }

    console.log(`🎳 [device=${deviceId}] SCORE processed`, {
      player: nextPlayer,
      frame: nextFrame,
      roll: nextRoll,
      pinsThisRoll,
      fallenPins,
      remainingPins,
    });
  } catch (err) {
    console.error(`❌ [device=${deviceId}] Error handling SCORE:`, err);
  }
}

// ---------------------------------------------------------------------------
// Start Game
// ---------------------------------------------------------------------------
function startGame(deviceId) {
  console.log(`🎮 [device=${deviceId}] Starting game`);
  const sm = getStateManager(deviceId);
  const players = sm.getPlayers();
  const scores = players.map((p) => {
    const name = typeof p === "string" ? p : p.name;
    return {
      name,
      frames: Array.from({ length: 10 }, () => ({
        ball1: null,
        ball2: null,
        ball3: null,
        cumulative: null,
      })),
      totalScore: 0,
      maxScore: 280,
    };
  });

  sm.setState({
    status: "active",
    gameStarted: true,
    currentPlayer: 0,
    frame: 1,
    roll: 1,
    scores,
  });

  publishState(deviceId);

  const topic = deviceTopicFor(deviceId);
  mqttClient.publish(topic, JSON.stringify({ action: "gameStart" }), {
    qos: 1,
    retain: true,
  });

  streakTrackers.delete(deviceId);

  const firstPlayer =
    typeof players[0] === "string" ? players[0] : players[0]?.name || "Bowler";
  const allPlayers = players.map((p) => ({
    name: typeof p === "string" ? p : p.name,
    totalScore: 0,
  }));
  announceEvent(deviceId, {
    event: "gameStart",
    player: firstPlayer,
    allPlayers,
  });
}

// ---------------------------------------------------------------------------
// Publish state for a specific device
// ---------------------------------------------------------------------------
function publishState(deviceId) {
  const sm = getStateManager(deviceId);
  const state = sm.getState();
  const players = state.players.map((p) =>
    typeof p === "string" ? p : p.name,
  );

  const message = { action: "update", ...state, players };
  const topic = deviceTopicFor(deviceId);

  mqttClient.publish(topic, JSON.stringify(message), { qos: 1, retain: true });
  console.log(`📤 [device=${deviceId}] State published`);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
function getLocalIP() {
  const interfaces = os.networkInterfaces();
  for (const name of Object.keys(interfaces)) {
    for (const iface of interfaces[name]) {
      if (iface.family === "IPv4" && !iface.internal) return iface.address;
    }
  }
  return "localhost";
}

// ---------------------------------------------------------------------------
// REST API — Device provisioning
// ---------------------------------------------------------------------------
app.post("/api/device/provision", (req, res) => {
  const { token, deviceName, mac } = req.body;

  if (!token || !deviceName || !mac) {
    return res
      .status(400)
      .json({ error: "token, deviceName, and mac are required" });
  }

  if (token !== PROVISION_TOKEN) {
    console.log(`🚫 Provision rejected — invalid token from ${mac}`);
    return res.status(401).json({ error: "Invalid provisioning token" });
  }

  // Idempotent: same MAC always gets same deviceId
  if (deviceRegistry.has(mac)) {
    const existing = deviceRegistry.get(mac);
    console.log(`✅ Re-provision for MAC ${mac} → device=${existing.deviceId}`);
    return res.json({
      status: "ok",
      deviceId: existing.deviceId,
      mqttHost: MQTT_HOST,
      mqttPort: MQTT_PORT_TLS,
      mqttUsername: MQTT_USERNAME,
      mqttPassword: MQTT_PASSWORD,
    });
  }

  const deviceId = String(nextDeviceId++);
  deviceRegistry.set(mac, {
    deviceId,
    deviceName,
    mac,
    provisionedAt: new Date().toISOString(),
  });

  // Pre-create a state manager so it's ready when the device connects
  getStateManager(deviceId);

  console.log(`🆕 Provisioned ${deviceName} (${mac}) → device=${deviceId}`);
  return res.json({
    status: "ok",
    deviceId,
    mqttHost: MQTT_HOST,
    mqttPort: MQTT_PORT_TLS,
    mqttUsername: MQTT_USERNAME,
    mqttPassword: MQTT_PASSWORD,
  });
});

// List all provisioned devices
app.get("/api/devices", (_req, res) => {
  const devices = [];
  for (const [mac, info] of deviceRegistry) {
    devices.push({ ...info, mac });
  }
  res.json(devices);
});

// ---------------------------------------------------------------------------
// REST API — Existing endpoints (device-aware via ?device= query param)
// ---------------------------------------------------------------------------
app.get("/api/info", (req, res) => {
  const localIP = getLocalIP();
  res.json({
    local: `http://localhost:${PORT}`,
    network: `http://${localIP}:${PORT}`,
    mqttBroker: MQTT_BROKER,
  });
});

app.get("/api/state", (req, res) => {
  const deviceId = req.query.device || "1";
  res.json(getStateManager(deviceId).getState());
});

app.post("/api/players", (req, res) => {
  const { name } = req.body;
  const deviceId = req.query.device || req.body.device || "1";
  if (!name) return res.status(400).json({ error: "Name is required" });
  handleAddPlayer(deviceId, name);
  res.json({ success: true, players: getStateManager(deviceId).getPlayers() });
});

app.delete("/api/players/:index", (req, res) => {
  const deviceId = req.query.device || "1";
  const index = parseInt(req.params.index);
  const sm = getStateManager(deviceId);
  sm.removePlayer(index);
  publishState(deviceId);
  res.json({ success: true, players: sm.getPlayers() });
});

app.post("/api/payment/request", (req, res) => {
  const { amount } = req.body;
  const deviceId = req.query.device || req.body.device || "1";
  if (!amount) return res.status(400).json({ error: "Amount is required" });
  handleRequestPayment(deviceId, amount);
  res.json({ success: true });
});

app.post("/api/payment/webhook", (req, res) => {
  const { status, paymentId } = req.body;
  const deviceId = req.query.device || req.body.device || "1";
  console.log(`📥 Payment webhook: ${status} for ${paymentId}`);
  handlePaymentStatus(deviceId, status);
  res.json({ success: true });
});

app.post("/api/reset", (req, res) => {
  const deviceId = req.query.device || req.body.device || "1";
  const sm = getStateManager(deviceId);
  sm.reset();
  publishState(deviceId);
  res.json({ success: true, message: "State reset" });
});

// SPA fallback — serve index.html for any non-API route (React Router)
app.get("*", (_req, res) => {
  const indexPath = path.join(DIST_DIR, "index.html");
  if (fs.existsSync(indexPath)) {
    res.sendFile(indexPath);
  } else {
    res.status(404).send("Frontend not built. Run: npm run build");
  }
});

// ---------------------------------------------------------------------------
// Start server
// ---------------------------------------------------------------------------
const localIP = getLocalIP();
app.listen(PORT, "0.0.0.0", () => {
  console.log("\n🚀 Backend server running!");
  console.log(`📍 Local:   http://localhost:${PORT}`);
  console.log(`🌐 Network: http://${localIP}:${PORT}`);
  console.log(`📡 MQTT:    ${MQTT_BROKER} (wildcard: ${STATE_TOPIC_WILDCARD})`);
  console.log(`🔑 Provision token: ${PROVISION_TOKEN}`);
  console.log(`\n💡 Devices provision via POST /api/device/provision\n`);
});
