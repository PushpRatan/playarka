import express from "express";
import cors from "cors";
import mqtt from "mqtt";
import { StateManager } from "./state.js";
import os from "os";

const app = express();
const PORT = 3001;

// Middleware
app.use(cors());
app.use(express.json());

// MQTT Configuration - Hardcoded for EMQX
const MQTT_BROKER = "mqtts://g11e070b.ala.asia-southeast1.emqxsl.com:8883";
const MQTT_USERNAME = "Pushp"; // Update if needed
const MQTT_PASSWORD = "Pushp9029@r"; // Update if needed
const DEVICE_ID = "1";

const STATE_TOPIC = `playarka/device/${DEVICE_ID}/state`;
const PINS_TOPIC = `playarka/device/${DEVICE_ID}/pins`;

// Initialize state manager
const stateManager = new StateManager();

// MQTT Client Options with TLS
const mqttOptions = {
  clientId: `playarka_backend_${Date.now()}`,
  clean: true,
  reconnectPeriod: 5000,
  connectTimeout: 10000,
  username: "Pushp",
  password: "Pushp9029@r",
  // TLS/SSL options for secure connection
  rejectUnauthorized: false, // Set to true in production with proper certificates
};

const mqttClient = mqtt.connect(MQTT_BROKER, mqttOptions);

mqttClient.on("connect", () => {
  console.log("✅ MQTT Connected to broker");

  // Subscribe to state topic
  mqttClient.subscribe(STATE_TOPIC, { qos: 1 }, (err) => {
    if (err) {
      console.error("❌ Failed to subscribe to state topic:", err);
    } else {
      console.log(`✅ Subscribed to ${STATE_TOPIC}`);
    }
  });
});

mqttClient.on("error", (err) => {
  console.error("❌ MQTT Error:", err.message);
  console.error("   Check your MQTT credentials and broker URL");
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

// Handle MQTT messages
function handleMqttMessage(topic, data) {
  if (topic === STATE_TOPIC) {
    // Messages from ESP (and others) may use a "type" field instead of "action"
    // Handle SCORE / HEARTBEAT first so they don't fall into the action switch.
    if (data.type === "SCORE") {
      handleScoreFromEsp(data);
      return;
    }
    if (data.type === "HEARTBEAT") {
      // For now we just log heartbeat; can be extended to track device health
      console.log("💓 HEARTBEAT from device:", JSON.stringify(data, null, 2));
      return;
    }

    // Ignore 'update' actions - these are state updates we publish ourselves
    if (data.action === "update") {
      return;
    }

    console.log(
      `📥 Received MQTT message on ${topic}:`,
      JSON.stringify(data, null, 2),
    );
    console.log(`   Action: "${data.action}"`);

    switch (data.action) {
      case "scan":
        handleScan();
        break;
      case "addPlayer":
        handleAddPlayer(data.name);
        break;
      case "ready":
        handleReady();
        break;
      case "requestPayment":
        handleRequestPayment(data.amount);
        break;
      case "paymentStatus":
        console.log(
          `💳 Processing paymentStatus action with status: ${data.status}`,
        );
        if (data.status) {
          handlePaymentStatus(data.status);
        } else {
          console.error("❌ paymentStatus action missing status field");
        }
        break;
      case "reset":
        handleReset();
        break;
      default:
        console.log(`⚠️  Unknown action: "${data.action}"`);
        console.log(`   Full data:`, JSON.stringify(data, null, 2));
    }
  }
}

// QR Scan Handler
function handleScan() {
  console.log("📱 QR Code scanned");
  stateManager.setState({ status: "onboarding" });
  publishState();
}

// Add Player Handler
function handleAddPlayer(name) {
  console.log(`➕ Adding player: ${name}`);
  const players = stateManager.getPlayers();
  if (players.length >= 6) {
    console.log("❌ Maximum 6 players allowed");
    return;
  }
  stateManager.addPlayer(name);
  publishState();
}

// Ready Handler
function handleReady() {
  console.log("✅ Players ready for payment");
  stateManager.setState({ ready: true, status: "payment" });
  publishState();
}

// Request Payment Handler
async function handleRequestPayment(amount) {
  console.log(`💳 Payment requested: ₹${amount}`);

  // Generate payment QR (mock - replace with actual payment gateway)
  const paymentQr = generatePaymentQr(amount);

  stateManager.setState({
    status: "payment",
    paymentQr: paymentQr,
    paymentStatus: "pending",
    amount: amount,
  });

  publishState();

  // Simulate payment webhook after 5 seconds (for testing)
  // In production, this would come from payment gateway webhook
  setTimeout(() => {
    // Uncomment to simulate successful payment
    // handlePaymentStatus('success');
  }, 5000);
}

// Generate Payment QR (Mock - replace with actual payment gateway)
function generatePaymentQr(amount) {
  // Mock UPI QR code
  return `upi://pay?pa=merchant@upi&pn=PlayArka&am=${amount}&cu=INR`;
}

// Payment Status Handler (called from webhook in production)
function handlePaymentStatus(status) {
  console.log(`💳 Payment status: ${status}`);
  stateManager.setState({
    paymentStatus: status,
    status: status === "success" ? "active" : "payment",
  });

  // Always publish state update first
  publishState();

  if (status === "success") {
    // Start game (which will also publish state and gameStart)
    console.log("🎮 Payment successful, starting game...");
    startGame();
  }
}

// Reset Handler
function handleReset() {
  console.log("🔄 Resetting game state");
  stateManager.reset();
  publishState();
}

// Handle SCORE messages coming from ESP on the shared state topic
function handleScoreFromEsp(data) {
  try {
    const state = stateManager.getState();
    const players = state.players || [];
    if (players.length === 0) {
      console.log("⚠️ SCORE received but there are no players – ignoring");
      return;
    }

    const fallenPins = Array.isArray(data.fallenPins) ? data.fallenPins : [];
    const remainingPins = Array.isArray(data.remainingPins)
      ? data.remainingPins
      : [];

    // 4‑pin test mode scoring:
    // - STRIKE: all 4 pins fallen on 1st ball → +20 bonus, so 24 total.
    // - SPARE: all pins fallen by 2nd ball → (pins in 2nd ball) + 10.
    // - Otherwise: score equals pins fallen in that ball.
    const TOTAL_PINS_PER_FRAME = 4;
    const totalFallen = fallenPins.length;

    let currentPlayer =
      typeof state.currentPlayer === "number" ? state.currentPlayer : 0;
    let frame = typeof state.frame === "number" ? state.frame : 1; // 1-based
    let roll = typeof state.roll === "number" ? state.roll : 1; // 1-based (1..3 for duckpin)

    if (frame < 1) frame = 1;
    if (frame > 10) frame = 10;
    if (roll < 1) roll = 1;
    if (roll > 3) roll = 3;
    if (currentPlayer < 0) currentPlayer = 0;
    if (currentPlayer >= players.length) currentPlayer = players.length - 1;

    const scores =
      state.scores && Array.isArray(state.scores) ? state.scores.slice() : [];

    // Ensure scores array is aligned with players
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
        maxScore: 300,
      });
    }

    const playerScore = scores[currentPlayer];
    const frameIndex = frame - 1;
    const frameScore = playerScore.frames[frameIndex];

    // Calculate pins knocked down in THIS ball using cumulative fallen pins.
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

    // Apply STRIKE / SPARE rules
    let scoreThisRoll = pinsThisRoll;
    let isStrike = false;
    let isSpare = false;

    if (totalFallen >= TOTAL_PINS_PER_FRAME) {
      if (roll === 1) {
        // Strike on first ball
        isStrike = true;
        scoreThisRoll = 20 + TOTAL_PINS_PER_FRAME; // e.g. 24
      } else if (roll === 2) {
        // Spare by second ball
        isSpare = true;
        scoreThisRoll = pinsThisRoll + 10;
      }
    }

    // Store numeric score gained in this ball (UI will show this number)
    if (roll === 1) {
      frameScore.ball1 = scoreThisRoll;
    } else if (roll === 2) {
      frameScore.ball2 = scoreThisRoll;
    } else {
      frameScore.ball3 = scoreThisRoll;
    }

    // Update totals (very basic: just sum all numeric pins across frames)
    let total = 0;
    playerScore.frames.forEach((f) => {
      const b1 = typeof f.ball1 === "number" ? f.ball1 : 0;
      const b2 = typeof f.ball2 === "number" ? f.ball2 : 0;
      const b3 = typeof f.ball3 === "number" ? f.ball3 : 0;
      total += b1 + b2 + b3;
      f.cumulative = total > 0 ? total : null;
    });
    playerScore.totalScore = total;

    // Advance roll / frame / player
    let nextPlayer = currentPlayer;
    let nextFrame = frame;
    let nextRoll = roll + 1;

    // On STRIKE or SPARE, frame ends immediately and player changes
    if (isStrike || isSpare) {
      nextRoll = 1;
      if (nextPlayer < players.length - 1) {
        nextPlayer += 1;
      } else {
        nextPlayer = 0;
        if (nextFrame < 10) {
          nextFrame += 1;
        } else {
          state.status = "completed";
        }
      }
    } else if (nextRoll > 3) {
      nextRoll = 1;
      if (nextPlayer < players.length - 1) {
        nextPlayer += 1;
      } else {
        nextPlayer = 0;
        if (nextFrame < 10) {
          nextFrame += 1;
        } else {
          // Last ball of last frame for last player – mark game completed
          state.status = "completed";
        }
      }
    }

    state.currentPlayer = nextPlayer;
    state.frame = nextFrame;
    state.roll = nextRoll;
    state.scores = scores;

    stateManager.setState(state);
    publishState();

    // Also publish a lightweight gameState message for ESP / hardware
    const gameStateMessage = {
      action: "gameState",
      currentPlayer: state.currentPlayer,
      frame: state.frame,
      roll: state.roll,
    };

    mqttClient.publish(STATE_TOPIC, JSON.stringify(gameStateMessage), {
      qos: 1,
      // Use retain: true temporarily so it's easy to see in EMQX
      // You can switch this back to false once you've verified it works.
      retain: true,
    });

    console.log("📤 gameState published:", gameStateMessage);

    console.log("🎳 SCORE processed from ESP:", {
      player: nextPlayer,
      frame: nextFrame,
      roll: nextRoll,
      pinsThisRoll,
      fallenPins,
      remainingPins,
    });
  } catch (err) {
    console.error("❌ Error handling SCORE from ESP:", err);
  }
}

// Start Game
function startGame() {
  console.log("🎮 Starting game");
  const players = stateManager.getPlayers();
  // Initialize simple score structures for each player
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
      maxScore: 300,
    };
  });

  stateManager.setState({
    status: "active",
    gameStarted: true,
    currentPlayer: 0,
    frame: 1,
    roll: 1,
    scores,
  });

  publishState();

  // Publish game start
  // Lightweight gameStart message for ESP / hardware (frontend already
  // knows players from the published state)
  const gameStartMessage = {
    action: "gameStart",
  };

  mqttClient.publish(STATE_TOPIC, JSON.stringify(gameStartMessage), {
    qos: 1,
    retain: true,
  });
}

// Publish current state
function publishState() {
  const state = stateManager.getState();
  // Transform players from objects to strings for frontend compatibility
  const players = state.players.map((p) =>
    typeof p === "string" ? p : p.name,
  );

  const message = {
    action: "update",
    ...state,
    players: players,
  };

  mqttClient.publish(STATE_TOPIC, JSON.stringify(message), {
    qos: 1,
    retain: true,
  });

  console.log("📤 State published:", message);
}

// Get local IP address for network access
function getLocalIP() {
  const interfaces = os.networkInterfaces();
  for (const name of Object.keys(interfaces)) {
    for (const iface of interfaces[name]) {
      // Skip internal (loopback) and non-IPv4 addresses
      if (iface.family === "IPv4" && !iface.internal) {
        return iface.address;
      }
    }
  }
  return "localhost";
}

// API Routes

// Get server info (IP address for mobile connection)
app.get("/api/info", (req, res) => {
  const localIP = getLocalIP();
  res.json({
    local: `http://localhost:${PORT}`,
    network: `http://${localIP}:${PORT}`,
    mqttBroker: MQTT_BROKER,
    deviceId: DEVICE_ID,
  });
});

// Get current state
app.get("/api/state", (req, res) => {
  res.json(stateManager.getState());
});

// Add player (HTTP endpoint - alternative to MQTT)
app.post("/api/players", (req, res) => {
  const { name } = req.body;
  if (!name) {
    return res.status(400).json({ error: "Name is required" });
  }

  handleAddPlayer(name);
  res.json({ success: true, players: stateManager.getPlayers() });
});

// Remove player
app.delete("/api/players/:index", (req, res) => {
  const index = parseInt(req.params.index);
  stateManager.removePlayer(index);
  publishState();
  res.json({ success: true, players: stateManager.getPlayers() });
});

// Request payment (HTTP endpoint)
app.post("/api/payment/request", (req, res) => {
  const { amount } = req.body;
  if (!amount) {
    return res.status(400).json({ error: "Amount is required" });
  }

  handleRequestPayment(amount);
  res.json({ success: true });
});

// Payment webhook (for payment gateway to call)
app.post("/api/payment/webhook", (req, res) => {
  const { status, paymentId } = req.body;

  // Verify payment webhook (add signature verification in production)
  console.log(`📥 Payment webhook received: ${status} for ${paymentId}`);

  handlePaymentStatus(status);
  res.json({ success: true });
});

// Reset state (for testing)
app.post("/api/reset", (req, res) => {
  stateManager.reset();
  publishState();
  res.json({ success: true, message: "State reset" });
});

// Start server - bind to 0.0.0.0 to allow network access
const localIP = getLocalIP();
app.listen(PORT, "0.0.0.0", () => {
  console.log("\n🚀 Backend server running!");
  console.log(`📍 Local:   http://localhost:${PORT}`);
  console.log(`🌐 Network: http://${localIP}:${PORT}`);
  console.log(`\n📡 MQTT Broker: ${MQTT_BROKER}`);
  console.log(`🎯 Device ID: ${DEVICE_ID}`);
  console.log(`\n💡 Use the Network URL on your mobile device (same WiFi)\n`);
});
