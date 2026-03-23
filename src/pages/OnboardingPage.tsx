import { useState, useEffect } from "react";
import mqtt, { type MqttClient } from "mqtt";

const MQTT_BROKER = "wss://g11e070b.ala.asia-southeast1.emqxsl.com:8084/mqtt";
const MQTT_USERNAME = "Pushp";
const MQTT_PASSWORD = "Pushp9029@r";

function getDeviceIdFromUrl(): string {
  return new URLSearchParams(window.location.search).get("device") || "1";
}

export function OnboardingPage() {
  const [players, setPlayers] = useState<string[]>([]);
  const [playerName, setPlayerName] = useState("");
  const [ready, setReady] = useState(false);
  const [paymentQr, setPaymentQr] = useState<string | null>(null);
  const [paymentStatus, setPaymentStatus] = useState<string | null>(null);
  const [amount, setAmount] = useState<number | null>(null);
  const [mqttClient, setMqttClient] = useState<MqttClient | null>(null);
  const [isConnected, setIsConnected] = useState(false);

  const deviceId = getDeviceIdFromUrl();
  const stateTopic = `playarka/device/${deviceId}/state`;

  useEffect(() => {
    const options = {
      clientId: `playarka_mobile_${deviceId}_${Math.random().toString(16).substring(2, 8)}`,
      clean: true,
      reconnectPeriod: 5000,
      connectTimeout: 10000,
      username: MQTT_USERNAME,
      password: MQTT_PASSWORD,
    };

    const client = mqtt.connect(MQTT_BROKER, options);
    setMqttClient(client);

    client.on("connect", () => {
      console.log("✅ Mobile MQTT Connected");
      setIsConnected(true);

      client.publish(stateTopic, JSON.stringify({ action: "scan" }), {
        qos: 1,
      });
      client.subscribe(stateTopic, { qos: 1 });
    });

    client.on("message", (_topic, message) => {
      try {
        const data = JSON.parse(message.toString());
        if (data.action === "update") {
          setPlayers(data.players || []);
          setReady(data.ready || false);
          setPaymentQr(data.paymentQr || null);
          setPaymentStatus(data.paymentStatus || null);
          setAmount(data.amount || null);
        }
      } catch (error) {
        console.error("Error parsing message:", error);
      }
    });

    return () => {
      if (client) {
        client.end();
      }
    };
  }, [deviceId, stateTopic]);

  const handleAddPlayer = () => {
    if (!playerName.trim() || !mqttClient || !mqttClient.connected) return;

    mqttClient.publish(
      stateTopic,
      JSON.stringify({ action: "addPlayer", name: playerName.trim() }),
      { qos: 1 },
    );
    setPlayerName("");
  };

  const handleReady = () => {
    if (!mqttClient || !mqttClient.connected) return;
    mqttClient.publish(stateTopic, JSON.stringify({ action: "ready" }), {
      qos: 1,
    });
  };

  const handleRequestPayment = () => {
    if (!mqttClient || !mqttClient.connected) return;
    const calculatedAmount = players.length * 250; // ₹250 per player
    mqttClient.publish(
      stateTopic,
      JSON.stringify({ action: "requestPayment", amount: calculatedAmount }),
      { qos: 1 },
    );
  };

  if (paymentStatus === "success") {
    return (
      <div className="min-h-screen wood-background flex items-center justify-center p-6">
        <div className="wood-panel border-2 border-green-500/50 rounded-lg p-8 text-center relative overflow-hidden max-w-md">
          <div className="text-6xl mb-4">✅</div>
          <h2 className="text-2xl font-bold text-green-400 mb-2">
            Payment Successful!
          </h2>
          <p className="text-gray-400">Game will start on the main screen</p>
        </div>
      </div>
    );
  }

  if (paymentQr) {
    return (
      <div className="min-h-screen wood-background p-6">
        <div className="max-w-md mx-auto">
          <h2 className="text-2xl font-bold text-white mb-4 text-center">
            Payment Required
          </h2>
          {amount && (
            <div className="wood-panel border-2 border-gray-600/40 rounded-lg p-4 mb-4 text-center relative overflow-hidden">
              <p className="text-sm text-gray-400">Amount to Pay</p>
              <p className="text-3xl font-bold text-white mt-2">₹{amount}</p>
            </div>
          )}
          <div className="wood-panel border-2 border-gray-600/40 rounded-lg p-4 mb-4 relative overflow-hidden">
            <p className="text-center text-sm text-gray-400 mb-2">
              Click the QR code to simulate payment
            </p>
            <div
              className="bg-black/40 border-2 border-gray-600/40 rounded p-4 text-center cursor-pointer hover:bg-black/50 transition-colors"
              onClick={() => {
                if (mqttClient && mqttClient.connected) {
                  mqttClient.publish(
                    stateTopic,
                    JSON.stringify({
                      action: "paymentStatus",
                      status: "success",
                    }),
                    { qos: 1 },
                  );
                }
              }}
              title="Click to simulate payment"
            >
              <p className="text-xs text-gray-300 break-all">{paymentQr}</p>
            </div>
          </div>
          {paymentStatus === "pending" && (
            <div className="wood-panel border-2 border-yellow-500/50 rounded-lg p-4 text-center relative overflow-hidden">
              <p className="text-sm text-yellow-400">Waiting for payment...</p>
            </div>
          )}
        </div>
      </div>
    );
  }

  return (
    <div className="min-h-screen wood-background p-6">
      <div className="max-w-md mx-auto">
        <div className="flex items-center justify-between mb-6">
          <h1 className="text-2xl font-bold text-white">Start Game</h1>
          <div className="flex items-center gap-2">
            <div
              className={`w-2 h-2 rounded-full ${
                isConnected ? "bg-green-500" : "bg-red-500"
              }`}
            />
            <span className="text-xs text-gray-400">
              {isConnected ? "Connected" : "Connecting..."}
            </span>
          </div>
        </div>

        <div className="wood-panel border-2 border-gray-600/40 rounded-lg p-4 mb-4 relative overflow-hidden">
          <h2 className="text-lg font-semibold text-gray-300 mb-3">
            Add Players ({players.length}/6)
          </h2>

          <div className="flex gap-2 mb-3">
            <input
              type="text"
              value={playerName}
              onChange={(e) => setPlayerName(e.target.value)}
              onKeyPress={(e) => e.key === "Enter" && handleAddPlayer()}
              placeholder="Enter player name"
              className="flex-1 bg-black/30 border-2 border-gray-600/40 rounded px-3 py-2 text-gray-300 placeholder-gray-500 focus:outline-none focus:border-gray-500/60"
            />
            <button
              onClick={handleAddPlayer}
              disabled={!playerName.trim() || players.length >= 6}
              className="wood-panel border-2 border-gray-600/40 text-gray-300 px-4 py-2 rounded font-semibold hover:border-gray-500/60 hover:text-white transition-colors disabled:border-gray-600/30 disabled:text-gray-500 disabled:cursor-not-allowed relative overflow-hidden"
            >
              Add
            </button>
          </div>

          {players.length > 0 && (
            <div className="space-y-2">
              {players.map((player, index) => (
                <div
                  key={index}
                  className="bg-black/30 border border-gray-600/40 rounded px-3 py-2 flex items-center justify-between"
                >
                  <span className="text-gray-300">
                    {index + 1}. {player}
                  </span>
                </div>
              ))}
            </div>
          )}
        </div>

        {players.length >= 1 && !ready && (
          <button
            onClick={handleReady}
            className="w-full wood-panel border-2 border-gray-600/40 text-gray-300 py-3 rounded-lg font-semibold text-lg mb-4 hover:border-gray-500/60 hover:text-white transition-colors relative overflow-hidden"
          >
            Ready to Play
          </button>
        )}

        {ready && !paymentQr && (
          <button
            onClick={handleRequestPayment}
            className="w-full wood-panel border-2 border-gray-600/40 text-gray-300 py-3 rounded-lg font-semibold text-lg hover:border-gray-500/60 hover:text-white transition-colors relative overflow-hidden"
          >
            Pay Now (₹{players.length * 250})
          </button>
        )}
      </div>
    </div>
  );
}
