import { useState, useEffect, useRef } from "react";
import mqtt, { type MqttClient } from "mqtt";

const MQTT_BROKER = "wss://g11e070b.ala.asia-southeast1.emqxsl.com:8084/mqtt";
const MQTT_USERNAME = "Pushp";
const MQTT_PASSWORD = "Pushp9029@r";

/** Device/lane id from URL (?device=1) or default "1" for single-lane. */
function getDeviceIdFromUrl(): string {
  return new URLSearchParams(window.location.search).get("device") || "1";
}

type Player = string | { id: string; name: string; index: number };

interface Announcement {
  audioUrl: string;
  text: string;
  mood: string;
  timestamp: number;
}

interface GameState {
  status: "waiting" | "onboarding" | "payment" | "active" | "completed";
  players: Player[];
  ready: boolean;
  paymentQr: string | null;
  paymentStatus: string | null;
  amount: number | null;
  gameStarted: boolean;
  currentPlayer: number;
  frame: number;
  roll: number;
  scores?: any;
  announce: Announcement | null;
  pinError: { pin: number; message: string } | null;
}

export function useMqttState() {
  const deviceId = getDeviceIdFromUrl();
  const stateTopic = `playarka/device/${deviceId}/state`;

  const [state, setState] = useState<GameState>({
    status: "waiting",
    players: [],
    ready: false,
    paymentQr: null,
    paymentStatus: null,
    amount: null,
    gameStarted: false,
    currentPlayer: 0,
    frame: 1,
    roll: 1,
    scores: [],
    announce: null,
    pinError: null,
  });
  const [isConnected, setIsConnected] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const mqttClientRef = useRef<MqttClient | null>(null);

  useEffect(() => {
    const options = {
      clientId: `playarka_web_${deviceId}_${Math.random().toString(16).substring(2, 8)}`,
      clean: true,
      reconnectPeriod: 5000,
      connectTimeout: 10000,
      username: MQTT_USERNAME,
      password: MQTT_PASSWORD,
    };

    const client = mqtt.connect(MQTT_BROKER, options);
    mqttClientRef.current = client;

    client.on("connect", () => {
      console.log("✅ MQTT Connected");
      setIsConnected(true);
      setError(null);

      // Subscribe to state topic for this device/lane
      client.subscribe(stateTopic, { qos: 1 }, (err) => {
        if (err) {
          console.error("❌ Subscribe error:", err);
          setError("Failed to subscribe");
        } else {
          console.log(`✅ Subscribed to ${stateTopic}`);
        }
      });
    });

    client.on("error", (err) => {
      console.error("❌ MQTT Error:", err);
      setError(err.message);
      setIsConnected(false);
    });

    client.on("close", () => {
      console.log("⚠️ MQTT Connection closed");
      setIsConnected(false);
    });

    client.on("reconnect", () => {
      console.log("🔄 Reconnecting...");
    });

    client.on("message", (topic, message) => {
      try {
        const data = JSON.parse(message.toString());
        console.log(`📥 Frontend received MQTT message:`, data);

        if (topic === stateTopic) {
          if (data.action === "update") {
            console.log(`🔄 Processing update action, status: ${data.status}`);
            // Ensure players are always strings (backend should send strings, but handle objects just in case)
            const players = (data.players || []).map((p: any) => {
              return typeof p === "string" ? p : p?.name || String(p);
            });

            setState((prev) => {
              const status = data.status || "waiting";
              const newState: GameState = {
                ...prev,
                status,
                players: players,
                ready: data.ready || false,
                paymentQr: data.paymentQr || null,
                paymentStatus: data.paymentStatus || null,
                amount: data.amount || null,
                gameStarted: data.gameStarted || false,
                currentPlayer: data.currentPlayer ?? prev.currentPlayer,
                frame: data.frame ?? prev.frame,
                roll: data.roll ?? prev.roll,
                scores: data.scores ?? prev.scores,
                pinError: (status === "waiting" || status === "active") ? null : prev.pinError,
              };

              console.log(`✅ Updating state to:`, newState);
              return newState;
            });
          } else if (data.action === "gameStart") {
            console.log(`🎮 Processing gameStart action`);
            // Normalize players to strings
            const players = (data.players || []).map((p: any) => {
              return typeof p === "string" ? p : p?.name || String(p);
            });

            setState((prev: any) => {
              const newState = {
                ...prev,
                status: "active",
                gameStarted: true,
                players: players.length > 0 ? players : prev.players,
              };
              console.log(`✅ Updating state to active:`, newState);
              return newState;
            });
          } else if (data.action === "gameState") {
            setState((prev) => ({
              ...prev,
              currentPlayer: data.currentPlayer || prev.currentPlayer,
              frame: data.frame || prev.frame,
              roll: data.roll || prev.roll,
            }));
          } else if (data.action === "announce") {
            setState((prev) => ({
              ...prev,
              announce: {
                audioUrl: data.audioUrl,
                text: data.text || "",
                mood: data.mood || "medium",
                timestamp: Date.now(),
              },
            }));
          } else if (data.action === "pinError") {
            setState((prev) => ({
              ...prev,
              pinError: {
                pin: data.pin,
                message: data.message || `Pin ${data.pin} is stuck`,
              },
            }));
          } else {
            console.log(`⚠️  Unhandled action: ${data.action}`);
          }
        }
      } catch (error) {
        console.error("❌ Error parsing message:", error);
      }
    });

    return () => {
      if (mqttClientRef.current) {
        mqttClientRef.current.end();
        mqttClientRef.current = null;
      }
    };
  }, [deviceId, stateTopic]);

  const publish = (action: string, data: any = {}) => {
    if (mqttClientRef.current && mqttClientRef.current.connected) {
      const message = JSON.stringify({ action, ...data });
      mqttClientRef.current.publish(stateTopic, message, { qos: 1 }, (err) => {
        if (err) {
          console.error("❌ Publish error:", err);
        }
      });
    } else {
      console.warn("⚠️ MQTT not connected");
    }
  };

  return {
    deviceId,
    state,
    isConnected,
    error,
    publish,
  };
}
