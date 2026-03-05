/*
  PlayArka Master - ESP32-S3

  What it does
  - Connects to WiFi
  - Connects to EMQX Cloud over MQTT (TLS on 8883; cert verification disabled via setInsecure())
  - Subscribes to a command topic
  - Publishes:
      1) HEARTBEAT JSON periodically
      2) SCORE JSON when serial input provides fallen/remaining pins

  Topics (default)
  - Subscribe (commands):  playarka/device/1/master/cmd
  - Publish heartbeat:     playarka/device/1/heartbeat
  - Publish score:         playarka/device/1/score

  Serial input formats supported (one line per event)
  1) JSON line (recommended):
     {"matchId":"uuid","fallenPins":[1,4,6],"remainingPins":[2,3,5,7,8,9,10],"isStrike":false,"isSpare":false}

  2) Key/Value line:
     fallen=1,4,6 remaining=2,3,5,7,8,9,10 strike=0 spare=0 matchId=uuid

  3) Pipe-separated CSV:
     1,4,6|2,3,5,7,8,9,10

  Notes
  - This sketch uses NTP to fill `timestamp` (epoch seconds). If NTP isn't ready, it falls back to millis()/1000.
  - For production TLS, you should pin the EMQX root CA instead of setInsecure().
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

// -----------------------
// User configuration
// -----------------------

// WiFi
static const char *WIFI_SSID = "Fundook ";
static const char *WIFI_PASSWORD = "Cereble$1000Bn";

// MQTT (EMQX Cloud)
static const char *MQTT_HOST = "g11e070b.ala.asia-southeast1.emqxsl.com";
static const uint16_t MQTT_PORT = 8883; // TLS
static const char *MQTT_USERNAME = "Pushp";
static const char *MQTT_PASSWORD = "Pushp9029@r";

// Identity
static const char *DEVICE_ID_STR = "lane-12"; // appears in HEARTBEAT/SCORE JSON
static const char *TOPIC_DEVICE_ID = "1";     // used in playarka/device/<id>/...

// Topics
// Single topic used for BOTH publishing (HEARTBEAT/SCORE) and receiving commands
// Subscribe in your MQTT client to this topic to see all messages.
static const char *MASTER_TOPIC = "playarka/device/1/state";

// Heartbeat interval
static uint32_t HEARTBEAT_INTERVAL_MS = 10UL * 1000UL;

// Serial
static const uint32_t SERIAL_BAUD = 115200;
static const size_t SERIAL_LINE_MAX = 512;

// Serial2 for communication with RP board
static const uint32_t SERIAL2_BAUD = 115200;
static const int SERIAL2_RX_PIN = 44; // Adjust based on your ESP32-S3 pinout
static const int SERIAL2_TX_PIN = 43; // Adjust based on your ESP32-S3 pinout

// -----------------------
// Globals
// -----------------------

WiFiClientSecure wifiSecureClient;
PubSubClient mqttClient(wifiSecureClient);

static uint32_t lastHeartbeatMs = 0;
static char serialLine[SERIAL_LINE_MAX];
static size_t serialLineLen = 0;

// Serial2 buffer for RP communication
static char serial2Line[SERIAL_LINE_MAX];
static size_t serial2LineLen = 0;

// If you don't send matchId in serial, the sketch will use this one.
static char currentMatchId[80] = "uuid";

// -----------------------
// Time helpers
// -----------------------

static uint32_t epochNow() {
  time_t now = time(nullptr);
  if (now > 1700000000) { // sanity check: after ~2023
    return (uint32_t)now;
  }
  return (uint32_t)(millis() / 1000UL);
}

static void setupTimeNtp() {
  // UTC; adjust if you want local time
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

// -----------------------
// WiFi / MQTT
// -----------------------

static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("📶 Connecting WiFi");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\n❌ WiFi connect timeout. Retrying...");
      start = millis();
    }
  }

  Serial.print("\n✅ WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
}

// Send command to RP board via Serial2
static void sendCommandToRP(const char *cmd) {
  StaticJsonDocument<128> doc;
  doc["cmd"] = cmd;
  
  char out[128];
  size_t n = serializeJson(doc, out, sizeof(out));
  if (n > 0) {
    Serial2.println(out);
    Serial2.flush();
    Serial.print("📤 Sent to RP: ");
    Serial.println(out);
  }
}

static void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  // Copy payload into a null-terminated buffer
  static char msg[512];
  const unsigned int n = (length < sizeof(msg) - 1) ? length : (sizeof(msg) - 1);
  memcpy(msg, payload, n);
  msg[n] = '\0';

  Serial.print("📥 MQTT message on ");
  Serial.print(topic);
  Serial.print(": ");
  Serial.println(msg);

  // Commands are optional. Supported:
  // - {"action":"setMatchId","matchId":"..."}
  // - {"action":"setHeartbeatIntervalMs","ms":10000}
  // - {"action":"requestHeartbeat"}
  // - {"action":"gameStart"} -> send to RP
  // - {"action":"gameState"} with player/frame/roll -> send to RP on player change
  // Also supports "type" instead of "action".

  // Skip large state "update" messages meant only for the frontend UI
  if (strncmp(msg, "{\"action\":\"update\"", 18) == 0) {
    Serial.println("ℹ️ Skipping large update message (frontend state only)");
    return;
  }

  StaticJsonDocument<384> doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) {
    Serial.println("⚠️ Command not JSON; ignoring.");
    return;
  }

  const char *action = doc["action"] | doc["type"] | "";
  if (strcmp(action, "setMatchId") == 0) {
    const char *mid = doc["matchId"] | "";
    if (mid[0]) {
      strlcpy(currentMatchId, mid, sizeof(currentMatchId));
      Serial.print("✅ matchId set to: ");
      Serial.println(currentMatchId);
    }
  } else if (strcmp(action, "setHeartbeatIntervalMs") == 0) {
    uint32_t ms = doc["ms"] | HEARTBEAT_INTERVAL_MS;
    if (ms < 1000) ms = 1000;
    HEARTBEAT_INTERVAL_MS = ms;
    Serial.print("✅ heartbeat interval set to: ");
    Serial.println(HEARTBEAT_INTERVAL_MS);
  } else if (strcmp(action, "requestHeartbeat") == 0) {
    lastHeartbeatMs = 0; // force publish next loop
  } else if (strcmp(action, "gameStart") == 0) {
    // Game started - tell RP to send forward command to slaves
    Serial.println("🎮 Game start detected - sending command to RP");
    sendCommandToRP("gameStart");
  } else if (strcmp(action, "gameState") == 0) {
    // Lightweight game state update (currentPlayer/frame/roll) from backend
    // Check if this is a player change (currentPlayer changed)
    // We'll track the last known player to detect changes
    static int lastPlayer = -1;
    int currentPlayer = doc["currentPlayer"] | -1;
    
    if (currentPlayer < 0) {
      return;
    }

    // First gameState just initializes lastPlayer; do NOT send playerChange
    if (lastPlayer == -1) {
      lastPlayer = currentPlayer;
      Serial.print("ℹ️ Initial gameState received, currentPlayer=");
      Serial.println(currentPlayer);
      return;
    }

    // Subsequent changes in currentPlayer indicate an actual player change
    if (currentPlayer != lastPlayer) {
      Serial.print("👤 Player change detected: ");
      Serial.print(lastPlayer);
      Serial.print(" -> ");
      Serial.println(currentPlayer);
      sendCommandToRP("playerChange");
      lastPlayer = currentPlayer;
    }
  } else {
    Serial.println("⚠️ Unknown command action/type; ignoring.");
  }
}

static void connectMqtt() {
  if (mqttClient.connected()) return;

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);

  // For EMQX Cloud TLS quickly (no root CA pinning)
  wifiSecureClient.setInsecure();

  Serial.print("🔌 Connecting MQTT");
  while (!mqttClient.connected()) {
    // Unique-ish client id
    char clientId[64];
    snprintf(clientId, sizeof(clientId), "esp32s3_master_%s_%lu", TOPIC_DEVICE_ID, (unsigned long)esp_random());

    bool ok = mqttClient.connect(clientId, MQTT_USERNAME, MQTT_PASSWORD);
    if (ok) {
      Serial.println("\n✅ MQTT connected");

      // Subscribe to the single master topic (also used for publishing)
      if (mqttClient.subscribe(MASTER_TOPIC, 1)) {
        Serial.print("✅ Subscribed: ");
        Serial.println(MASTER_TOPIC);
      } else {
        Serial.print("❌ Subscribe failed: ");
        Serial.println(MASTER_TOPIC);
      }
    } else {
      Serial.print(".");
      delay(800);
    }
  }
}

// -----------------------
// Publish helpers
// -----------------------

static void publishHeartbeat() {
  StaticJsonDocument<256> doc;
  doc["type"] = "HEARTBEAT";
  doc["deviceId"] = DEVICE_ID_STR;
  doc["uptime"] = (uint32_t)millis();
  doc["timestamp"] = epochNow();

  char out[256];
  size_t n = serializeJson(doc, out, sizeof(out));
  if (n == 0) {
    Serial.println("❌ Failed to serialize heartbeat JSON");
    return;
  }

  // Publish on the single master topic
  bool ok = mqttClient.publish(MASTER_TOPIC, out, false);
  Serial.print(ok ? "📤 HEARTBEAT -> " : "❌ HEARTBEAT publish failed -> ");
  Serial.println(out);
}

static void publishScore(
  const char *matchId,
  const int *fallenPins, size_t fallenCount,
  const int *remainingPins, size_t remainingCount,
  bool isStrike, bool isSpare
) {
  StaticJsonDocument<512> doc;
  doc["type"] = "SCORE";
  doc["matchId"] = matchId;

  JsonArray fallen = doc.createNestedArray("fallenPins");
  for (size_t i = 0; i < fallenCount; i++) fallen.add(fallenPins[i]);

  doc["isStrike"] = isStrike;
  doc["isSpare"] = isSpare;

  JsonArray remaining = doc.createNestedArray("remainingPins");
  for (size_t i = 0; i < remainingCount; i++) remaining.add(remainingPins[i]);

  doc["timestamp"] = epochNow();

  char out[512];
  size_t n = serializeJson(doc, out, sizeof(out));
  if (n == 0) {
    Serial.println("❌ Failed to serialize score JSON");
    return;
  }

  // Publish on the single master topic
  bool ok = mqttClient.publish(MASTER_TOPIC, out, false);
  Serial.print(ok ? "📤 SCORE -> " : "❌ SCORE publish failed -> ");
  Serial.println(out);
}

// -----------------------
// Serial parsing
// -----------------------

static bool parseCsvPins(const char *csv, int *outPins, size_t outMax, size_t *outCount) {
  *outCount = 0;
  if (!csv) return false;

  // Accept empty list
  while (*csv == ' ' || *csv == '\t') csv++;
  if (*csv == '\0') return true;

  char buf[256];
  strlcpy(buf, csv, sizeof(buf));

  char *saveptr = nullptr;
  char *tok = strtok_r(buf, ",", &saveptr);
  while (tok) {
    while (*tok == ' ' || *tok == '\t') tok++;
    int v = atoi(tok);
    if (v > 0) {
      if (*outCount < outMax) outPins[(*outCount)++] = v;
    }
    tok = strtok_r(nullptr, ",", &saveptr);
  }
  return true;
}

static bool parseSerialJsonLine(const char *line) {
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) return false;

  // If the user sends full SCORE payload already, we accept it.
  // Required: fallenPins + remainingPins (arrays). Optional: matchId, isStrike, isSpare.
  JsonArray fallen = doc["fallenPins"].as<JsonArray>();
  JsonArray remaining = doc["remainingPins"].as<JsonArray>();
  if (fallen.isNull() || remaining.isNull()) return false;

  int fallenPins[10];
  int remainingPins[10];
  size_t fallenCount = 0, remainingCount = 0;

  for (JsonVariant v : fallen) {
    if (fallenCount < 10) fallenPins[fallenCount++] = v.as<int>();
  }
  for (JsonVariant v : remaining) {
    if (remainingCount < 10) remainingPins[remainingCount++] = v.as<int>();
  }

  const char *mid = doc["matchId"] | currentMatchId;
  bool strike = doc["isStrike"] | false;
  bool spare = doc["isSpare"] | false;

  publishScore(mid, fallenPins, fallenCount, remainingPins, remainingCount, strike, spare);
  return true;
}

static const char *findValue(const char *line, const char *key) {
  // Finds "key=VALUE" in a whitespace-separated string. Returns pointer to VALUE in original line.
  const size_t klen = strlen(key);
  const char *p = line;
  while (*p) {
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
      return p + klen + 1;
    }
    while (*p && *p != ' ' && *p != '\t') p++;
  }
  return nullptr;
}

static void copyTokenValue(const char *start, char *dst, size_t dstSize) {
  // Copies until whitespace/end.
  if (!start || dstSize == 0) return;
  size_t i = 0;
  while (start[i] && start[i] != ' ' && start[i] != '\t' && i < dstSize - 1) {
    dst[i] = start[i];
    i++;
  }
  dst[i] = '\0';
}

static bool parseSerialKvLine(const char *line) {
  // fallen=... remaining=... strike=0 spare=0 matchId=...
  const char *fallenV = findValue(line, "fallen");
  const char *remainingV = findValue(line, "remaining");
  if (!fallenV || !remainingV) return false;

  char fallenBuf[256], remainingBuf[256], matchBuf[80], strikeBuf[8], spareBuf[8];
  copyTokenValue(fallenV, fallenBuf, sizeof(fallenBuf));
  copyTokenValue(remainingV, remainingBuf, sizeof(remainingBuf));

  const char *matchV = findValue(line, "matchId");
  if (matchV) copyTokenValue(matchV, matchBuf, sizeof(matchBuf));
  else strlcpy(matchBuf, currentMatchId, sizeof(matchBuf));

  const char *strikeV = findValue(line, "strike");
  const char *spareV = findValue(line, "spare");
  if (strikeV) copyTokenValue(strikeV, strikeBuf, sizeof(strikeBuf)); else strlcpy(strikeBuf, "0", sizeof(strikeBuf));
  if (spareV) copyTokenValue(spareV, spareBuf, sizeof(spareBuf)); else strlcpy(spareBuf, "0", sizeof(spareBuf));

  int fallenPins[10], remainingPins[10];
  size_t fallenCount = 0, remainingCount = 0;
  parseCsvPins(fallenBuf, fallenPins, 10, &fallenCount);
  parseCsvPins(remainingBuf, remainingPins, 10, &remainingCount);

  bool strike = (atoi(strikeBuf) != 0);
  bool spare = (atoi(spareBuf) != 0);
  publishScore(matchBuf, fallenPins, fallenCount, remainingPins, remainingCount, strike, spare);
  return true;
}

static bool parseSerialPipeLine(const char *line) {
  // "1,4,6|2,3,5,7,8,9,10"
  const char *pipe = strchr(line, '|');
  if (!pipe) return false;

  char fallenBuf[256], remainingBuf[256];
  size_t leftLen = (size_t)(pipe - line);
  if (leftLen >= sizeof(fallenBuf)) leftLen = sizeof(fallenBuf) - 1;
  memcpy(fallenBuf, line, leftLen);
  fallenBuf[leftLen] = '\0';

  strlcpy(remainingBuf, pipe + 1, sizeof(remainingBuf));

  int fallenPins[10], remainingPins[10];
  size_t fallenCount = 0, remainingCount = 0;
  parseCsvPins(fallenBuf, fallenPins, 10, &fallenCount);
  parseCsvPins(remainingBuf, remainingPins, 10, &remainingCount);

  publishScore(currentMatchId, fallenPins, fallenCount, remainingPins, remainingCount, false, false);
  return true;
}

static void handleSerialLine(const char *line) {
  if (!line) return;
  while (*line == ' ' || *line == '\t') line++;
  if (*line == '\0') return;

  // Update matchId quickly:
  //   matchId=xxxx
  // or in JSON:
  //   {"action":"setMatchId","matchId":"xxxx"}
  if (strncmp(line, "matchId=", 8) == 0) {
    strlcpy(currentMatchId, line + 8, sizeof(currentMatchId));
    Serial.print("✅ matchId set to: ");
    Serial.println(currentMatchId);
    return;
  }

  if (line[0] == '{') {
    if (parseSerialJsonLine(line)) return;
  }
  if (parseSerialKvLine(line)) return;
  if (parseSerialPipeLine(line)) return;

  Serial.print("⚠️ Unrecognized serial format: ");
  Serial.println(line);
}

// Handle data from RP board (Serial2)
static void handleRPData(const char *line) {
  if (!line) return;
  while (*line == ' ' || *line == '\t') line++;
  if (*line == '\0') return;

  Serial.print("📥 Data from RP: ");
  Serial.println(line);

  // RP sends ball detection data in JSON format:
  // {"type":"ballDetected","fallenPins":[1,4,6],"remainingPins":[2,3,5,7,8,9,10]}
  if (line[0] == '{') {
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, line);
    if (err) {
      Serial.print("⚠️ Failed to parse RP JSON: ");
      Serial.println(err.c_str());
      return;
    }

    const char *type = doc["type"] | "";
    if (strcmp(type, "ballDetected") == 0) {
      JsonArray fallen = doc["fallenPins"].as<JsonArray>();
      JsonArray remaining = doc["remainingPins"].as<JsonArray>();
      
      if (!fallen.isNull() && !remaining.isNull()) {
        int fallenPins[10];
        int remainingPins[10];
        size_t fallenCount = 0, remainingCount = 0;

        for (JsonVariant v : fallen) {
          if (fallenCount < 10) fallenPins[fallenCount++] = v.as<int>();
        }
        for (JsonVariant v : remaining) {
          if (remainingCount < 10) remainingPins[remainingCount++] = v.as<int>();
        }

        bool strike = doc["isStrike"] | false;
        bool spare = doc["isSpare"] | false;

        Serial.println("🎳 Ball detected - publishing score");
        publishScore(currentMatchId, fallenPins, fallenCount, remainingPins, remainingCount, strike, spare);
      }
    }
  }
}

static void pollSerial2() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serial2Line[serial2LineLen] = '\0';
      handleRPData(serial2Line);
      serial2LineLen = 0;
      continue;
    }
    if (serial2LineLen < SERIAL_LINE_MAX - 1) {
      serial2Line[serial2LineLen++] = c;
    } else {
      // Line too long; reset
      serial2LineLen = 0;
    }
  }
}

// Keep Serial monitor input for debugging/fallback (optional)
static void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serialLine[serialLineLen] = '\0';
      // Only handle matchId updates from Serial monitor now
      // Ball detection data should come from RP
      if (strncmp(serialLine, "matchId=", 8) == 0) {
        strlcpy(currentMatchId, serialLine + 8, sizeof(currentMatchId));
        Serial.print("✅ matchId set to: ");
        Serial.println(currentMatchId);
      } else if (serialLine[0] == '{') {
        // Still allow JSON input for manual testing
        handleSerialLine(serialLine);
      }
      serialLineLen = 0;
      continue;
    }
    if (serialLineLen < SERIAL_LINE_MAX - 1) {
      serialLine[serialLineLen++] = c;
    } else {
      // Line too long; reset
      serialLineLen = 0;
    }
  }
}

// -----------------------
// Arduino lifecycle
// -----------------------

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);

  // Initialize Serial2 for RP board communication
  Serial2.begin(SERIAL2_BAUD, SERIAL_8N1, SERIAL2_RX_PIN, SERIAL2_TX_PIN);
  delay(200);

  Serial.println("\n=== PlayArka Master (ESP32-S3) ===");
  Serial.print("Topic device id: ");
  Serial.println(TOPIC_DEVICE_ID);
  Serial.print("Payload deviceId: ");
  Serial.println(DEVICE_ID_STR);
  Serial.println("📡 Serial2 initialized for RP communication");

  connectWiFi();
  setupTimeNtp();

  connectMqtt();
  publishHeartbeat(); // immediate heartbeat on boot
  lastHeartbeatMs = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (!mqttClient.connected()) {
    connectMqtt();
  }
  mqttClient.loop();

  // Poll Serial2 for data from RP board (ball detection)
  pollSerial2();

  // Poll Serial for manual input (matchId updates, debugging)
  pollSerial();

  uint32_t nowMs = millis();
  if ((uint32_t)(nowMs - lastHeartbeatMs) >= HEARTBEAT_INTERVAL_MS) {
    publishHeartbeat();
    lastHeartbeatMs = nowMs;
  }
}


