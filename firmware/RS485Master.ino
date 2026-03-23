// RS485 Master with Ball Sensor State Machine + I2C FRAM Config Storage
// Board: generic Arduino-compatible MCU acting as RS485 master
//
// Behavior:
// - Four motor states: STATE_FORWARD, STATE_HOLD, STATE_REVERSE, STATE_FREE
// - In STATE_FREE, motor is commanded "free" and outputs stay stable
//   until ball sensor detects a ball (sensor goes from 1 -> 0).
// - Once ball is detected, wait 2 seconds, then transition to STATE_FORWARD.
//
// FRAM (FM24CL64B @ 0x50):
// - Stores device provisioning config (WiFi creds, MQTT creds, deviceId)
// - ESP32-S3 reads/writes config via Serial commands:
//     {"cmd":"configRead"}  -> RP reads FRAM, responds with config JSON
//     {"cmd":"configWrite",...} -> RP writes to FRAM, responds with ack

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SoftwareSerial.h>
#include <Wire.h>
#include <string.h>

// ===================== Pin Definitions =====================

// RS485 bus (to slaves)
#define RS485_TX_PIN 0
#define RS485_RX_PIN 1
#define RS485_DE_PIN 2
#define RS485_RE_PIN 3

#define BAUDRATE 9600

// Encoder revolutions for the DOWN leg of the rack cycle (sent to slaves; tune here only)
#define MOTOR_DOWN_REVS 8

// Ball sensor: 1 = no ball, 0 = ball present
#define BALL_SENSOR_PIN 13

// ESP32 link (to PlayArka master ESP32-S3)
// RP TX 20 -> ESP RX 44
// RP RX 21 -> ESP TX 43
#define ESP_RX_PIN 21
#define ESP_TX_PIN 20
#define ESP_BAUD   115200

// I2C FRAM (FM24CL64B-G)
// RP2040: SDA=GP4, SCL=GP5 | AVR Uno/Nano: SDA=A4, SCL=A5
// Adjust if your wiring differs.
#define FRAM_I2C_ADDR 0x50
#define I2C_SDA_PIN 16
#define I2C_SCL_PIN 17
#define FRAM_WP_PIN   18    // Write Protect pin — driven LOW to allow writes

// ===================== FRAM Config Layout =====================
// Total: ~379 bytes — well within FM24CL64B's 8 KB
#define FRAM_MAGIC_OFFSET   0x0000  // 2 bytes: 'P','A'
#define FRAM_CONFIG_OFFSET  0x0002  // 1 byte: configured flag (0/1)
#define FRAM_SSID_OFFSET    0x0003  // 1 byte len + 32 bytes
#define FRAM_SSID_MAX       32
#define FRAM_WPASS_OFFSET   0x0024  // 1 byte len + 64 bytes
#define FRAM_WPASS_MAX      64
#define FRAM_MQHOST_OFFSET  0x0065  // 1 byte len + 128 bytes
#define FRAM_MQHOST_MAX     128
#define FRAM_MQPORT_OFFSET  0x00E6  // 2 bytes big-endian uint16
#define FRAM_MQUSER_OFFSET  0x00E8  // 1 byte len + 64 bytes
#define FRAM_MQUSER_MAX     64
#define FRAM_MQPASS_OFFSET  0x0129  // 1 byte len + 64 bytes
#define FRAM_MQPASS_MAX     64
#define FRAM_DEVID_OFFSET   0x016A  // 1 byte len + 16 bytes
#define FRAM_DEVID_MAX      16

// ===================== Software Serials =====================

SoftwareSerial RS485(RS485_RX_PIN, RS485_TX_PIN);
SoftwareSerial espSerial(ESP_RX_PIN, ESP_TX_PIN);

// ===================== Pin Tracking =====================

const int TOTAL_PINS = 8;

int statusCount = 0;
bool slaveReplied[TOTAL_PINS] = {false};

static const size_t SERIAL_LINE_MAX = 512;
static char espSerialLine[SERIAL_LINE_MAX];
static size_t espSerialLineLen = 0;

bool pinStates[TOTAL_PINS];
int remainingPins[TOTAL_PINS];
int fallenPins[TOTAL_PINS];
int remainingCount = 0;
int fallenCount = 0;

int ballsSinceLastFullRack = 0;
bool ballJustDetected = false;
bool cancelBallWait = false;
bool waitingForStatus = false;
bool gameOver = false;
/// After gameStart: wait for one full status_req round-trip (all slaves) before first UP.
bool pendingGameStart = false;

enum MotorState {
  STATE_FORWARD,
  STATE_REVERSE,
  STATE_HOLD,
  STATE_FREE
};

MotorState currentState = STATE_FORWARD;

// ===================== FRAM I2C Functions =====================

bool framAvailable = false;

void framWriteBytes(uint16_t startAddr, const uint8_t *data, size_t len) {
  digitalWrite(FRAM_WP_PIN, LOW); // ensure writes are enabled
  size_t offset = 0;
  while (offset < len) {
    size_t chunk = len - offset;
    if (chunk > 28) chunk = 28; // Wire buffer safe limit (30 - 2 addr bytes)
    Wire.beginTransmission(FRAM_I2C_ADDR);
    Wire.write((uint8_t)((startAddr + offset) >> 8));
    Wire.write((uint8_t)((startAddr + offset) & 0xFF));
    Wire.write(data + offset, chunk);
    Wire.endTransmission();
    offset += chunk;
  }
}

void framReadBytes(uint16_t startAddr, uint8_t *data, size_t len) {
  size_t offset = 0;
  while (offset < len) {
    size_t chunk = len - offset;
    if (chunk > 32) chunk = 32;
    Wire.beginTransmission(FRAM_I2C_ADDR);
    Wire.write((uint8_t)((startAddr + offset) >> 8));
    Wire.write((uint8_t)((startAddr + offset) & 0xFF));
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)FRAM_I2C_ADDR, (uint8_t)chunk);
    for (size_t i = 0; i < chunk && Wire.available(); i++) {
      data[offset + i] = Wire.read();
    }
    offset += chunk;
  }
}

void framWriteString(uint16_t addr, const char *str, size_t maxLen) {
  size_t len = strlen(str);
  if (len > maxLen) len = maxLen;
  uint8_t lenByte = (uint8_t)len;
  framWriteBytes(addr, &lenByte, 1);
  if (len > 0) framWriteBytes(addr + 1, (const uint8_t *)str, len);
}

size_t framReadString(uint16_t addr, char *buf, size_t bufSize, size_t maxFieldLen) {
  uint8_t len = 0;
  framReadBytes(addr, &len, 1);
  if (len > maxFieldLen) len = (uint8_t)maxFieldLen;
  if (len >= bufSize) len = (uint8_t)(bufSize - 1);
  if (len > 0) framReadBytes(addr + 1, (uint8_t *)buf, len);
  buf[len] = '\0';
  return len;
}

void framWriteUint16(uint16_t addr, uint16_t val) {
  uint8_t buf[2] = { (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
  framWriteBytes(addr, buf, 2);
}

uint16_t framReadUint16(uint16_t addr) {
  uint8_t buf[2] = {0, 0};
  framReadBytes(addr, buf, 2);
  return ((uint16_t)buf[0] << 8) | buf[1];
}

bool framDetect() {
  Wire.beginTransmission(FRAM_I2C_ADDR);
  return Wire.endTransmission() == 0;
}

bool framHasMagic() {
  uint8_t magic[2] = {0, 0};
  framReadBytes(FRAM_MAGIC_OFFSET, magic, 2);
  return (magic[0] == 'P' && magic[1] == 'A');
}

void framWriteMagic() {
  uint8_t magic[2] = {'P', 'A'};
  framWriteBytes(FRAM_MAGIC_OFFSET, magic, 2);
}

// ===================== Config Commands (ESP <-> FRAM) =====================

void handleConfigRead() {
  JsonDocument doc;
  doc["type"] = "config";

  if (!framAvailable) {
    doc["configured"] = 0;
    doc["error"] = "FRAM not found";
  } else if (!framHasMagic()) {
    doc["configured"] = 0;
  } else {
    uint8_t cfgFlag = 0;
    framReadBytes(FRAM_CONFIG_OFFSET, &cfgFlag, 1);
    doc["configured"] = cfgFlag ? 1 : 0;

    char buf[129];
    framReadString(FRAM_SSID_OFFSET, buf, sizeof(buf), FRAM_SSID_MAX);
    doc["ssid"] = String(buf);

    framReadString(FRAM_WPASS_OFFSET, buf, sizeof(buf), FRAM_WPASS_MAX);
    doc["pass"] = String(buf);

    framReadString(FRAM_MQHOST_OFFSET, buf, sizeof(buf), FRAM_MQHOST_MAX);
    doc["mqttHost"] = String(buf);

    doc["mqttPort"] = framReadUint16(FRAM_MQPORT_OFFSET);

    framReadString(FRAM_MQUSER_OFFSET, buf, sizeof(buf), FRAM_MQUSER_MAX);
    doc["mqttUser"] = String(buf);

    framReadString(FRAM_MQPASS_OFFSET, buf, sizeof(buf), FRAM_MQPASS_MAX);
    doc["mqttPass"] = String(buf);

    framReadString(FRAM_DEVID_OFFSET, buf, sizeof(buf), FRAM_DEVID_MAX);
    doc["deviceId"] = String(buf);
  }

  serializeJson(doc, espSerial);
  espSerial.println();
  espSerial.flush();

  Serial.print("FRAM -> ESP config: ");
  serializeJson(doc, Serial);
  Serial.println();
}

void handleConfigWrite(JsonDocument &cmdDoc) {
  if (!framAvailable) {
    espSerial.println("{\"type\":\"configWriteErr\",\"error\":\"FRAM not found\"}");
    espSerial.flush();
    return;
  }

  framWriteMagic();

  uint8_t cfgFlag = cmdDoc["configured"] | 0;
  framWriteBytes(FRAM_CONFIG_OFFSET, &cfgFlag, 1);

  const char *ssid     = cmdDoc["ssid"]     | "";
  const char *pass     = cmdDoc["pass"]     | "";
  const char *mqttHost = cmdDoc["mqttHost"] | "";
  uint16_t    mqttPort = cmdDoc["mqttPort"] | 8883;
  const char *mqttUser = cmdDoc["mqttUser"] | "";
  const char *mqttPass = cmdDoc["mqttPass"] | "";
  const char *deviceId = cmdDoc["deviceId"] | "";

  framWriteString(FRAM_SSID_OFFSET,    ssid,     FRAM_SSID_MAX);
  framWriteString(FRAM_WPASS_OFFSET,   pass,     FRAM_WPASS_MAX);
  framWriteString(FRAM_MQHOST_OFFSET,  mqttHost, FRAM_MQHOST_MAX);
  framWriteUint16(FRAM_MQPORT_OFFSET,  mqttPort);
  framWriteString(FRAM_MQUSER_OFFSET,  mqttUser, FRAM_MQUSER_MAX);
  framWriteString(FRAM_MQPASS_OFFSET,  mqttPass, FRAM_MQPASS_MAX);
  framWriteString(FRAM_DEVID_OFFSET,   deviceId, FRAM_DEVID_MAX);

  Serial.print("FRAM write: configured=");
  Serial.print(cfgFlag);
  Serial.print(" ssid=");
  Serial.print(ssid);
  Serial.print(" deviceId=");
  Serial.println(deviceId);

  espSerial.println("{\"type\":\"configWriteOk\"}");
  espSerial.flush();
}

void handleConfigClear() {
  if (!framAvailable) {
    espSerial.println("{\"type\":\"configWriteErr\",\"error\":\"FRAM not found\"}");
    espSerial.flush();
    return;
  }

  // Clear magic bytes and configured flag
  uint8_t zeros[3] = {0, 0, 0};
  framWriteBytes(FRAM_MAGIC_OFFSET, zeros, 3);

  Serial.println("FRAM cleared (magic + configured)");
  espSerial.println("{\"type\":\"configClearOk\"}");
  espSerial.flush();
}

// ===================== RS485 Functions =====================

void rs485Transmit() {
  digitalWrite(RS485_DE_PIN, HIGH);
  digitalWrite(RS485_RE_PIN, HIGH);
}

void rs485Receive() {
  digitalWrite(RS485_DE_PIN, LOW);
  digitalWrite(RS485_RE_PIN, LOW);
}

void sendMotorCommand(const char *direction) {
  JsonDocument doc;
  doc["cmd"] = "motor";
  JsonArray addr = doc["addr"].to<JsonArray>();
  for (int i = 0; i < TOTAL_PINS; i++) addr.add(i);
  doc["dir"] = direction;
  if (strcmp(direction, "down") == 0) {
    doc["revs"] = MOTOR_DOWN_REVS;
  }

  rs485Transmit();
  serializeJson(doc, RS485);
  RS485.println();
  RS485.flush();
  rs485Receive();
}

void sendMotorCommandRemaining(const char *direction) {
  JsonDocument doc;
  doc["cmd"] = "motor";
  JsonArray addr = doc["addr"].to<JsonArray>();
  for (int i = 0; i < remainingCount; i++) addr.add(remainingPins[i]);
  doc["dir"] = direction;
  if (strcmp(direction, "down") == 0) {
    doc["revs"] = MOTOR_DOWN_REVS;
  }

  rs485Transmit();
  serializeJson(doc, RS485);
  RS485.println();
  RS485.flush();
  rs485Receive();
}

void sendStatusReq() {
  JsonDocument doc;
  doc["cmd"] = "status_req";
  rs485Transmit();
  serializeJson(doc, RS485);
  RS485.println();
  RS485.flush();
  rs485Receive();
}


// Send ball detection data to ESP
void sendBallDetectedToESP() {
  JsonDocument doc;
  doc["type"] = "ballDetected";
  
  JsonArray fallen = doc["fallenPins"].to<JsonArray>();
  for (int i = 0; i < fallenCount; i++) {
    fallen.add(fallenPins[i] + 1); // Convert 0-based to 1-based pin numbers
  }
  
  JsonArray remaining = doc["remainingPins"].to<JsonArray>();
  for (int i = 0; i < remainingCount; i++) {
    remaining.add(remainingPins[i] + 1);
  }
  
  bool isStrike = (fallenCount == TOTAL_PINS);
  bool isSpare = (fallenCount == TOTAL_PINS && remainingCount == 0);
  doc["isStrike"] = isStrike;
  doc["isSpare"] = isSpare;

  serializeJson(doc, espSerial);
  espSerial.println();
  espSerial.flush();

  Serial.print("RP -> ESP : ");
  serializeJson(doc, Serial);
  Serial.println();
  Serial.flush();

  Serial.print("📤 Sent ball detection to ESP: fallen=");
  Serial.print(fallenCount);
  Serial.print(", remaining=");
  Serial.println(remainingCount);
}

// ===================== ESP Command Handler =====================

void handleESPCommand(const char *line) {
  if (!line) return;
  while (*line == ' ' || *line == '\t') line++;
  if (*line == '\0') return;

  Serial.print("📥 Command from ESP: ");
  Serial.println(line);
  
  if (line[0] != '{') return;
  
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    Serial.print("⚠️ Failed to parse ESP command: ");
    Serial.println(err.c_str());
    return;
  }
  
  const char *cmd = doc["cmd"] | "";

  // ---- Config commands (FRAM) ----
  if (strcmp(cmd, "configRead") == 0) {
    handleConfigRead();
    return;
  }
  if (strcmp(cmd, "configWrite") == 0) {
    handleConfigWrite(doc);
    return;
  }
  if (strcmp(cmd, "configClear") == 0) {
    handleConfigClear();
    return;
  }

  // ---- Game commands ----
  if (strcmp(cmd, "gameStart") == 0) {
    Serial.println("🎮 Game start — probing all slaves (status_req); motion after all reply");
    for (int i = 0; i < TOTAL_PINS; i++) {
      pinStates[i] = true;
    }
    ballsSinceLastFullRack = 0;
    gameOver = false;
    ballJustDetected = false;
    cancelBallWait = true;
    pendingGameStart = true;
    currentState = STATE_FORWARD;
    waitingForStatus = false;
    statusCount = 0;
    for (int i = 0; i < TOTAL_PINS; i++) slaveReplied[i] = false;
    sendStatusReq();
    waitingForStatus = true;
  } else if (strcmp(cmd, "reset") == 0) {
    Serial.println("🔄 Reset command from ESP");
    pendingGameStart = false;
    for (int i = 0; i < TOTAL_PINS; i++) {
      pinStates[i] = true;
    }
    ballsSinceLastFullRack = 0;
    gameOver = false;
    ballJustDetected = false;
    cancelBallWait = true;
    currentState = STATE_FORWARD;
    waitingForStatus = false;
    statusCount = 0;
    for (int i = 0; i < TOTAL_PINS; i++) slaveReplied[i] = false;
  } else if (strcmp(cmd, "gameOver") == 0) {
    Serial.println("🏁 Game over command from ESP - dropping all pins and entering FREE state");
    pendingGameStart = false;
    gameOver = true;
    ballJustDetected = false;
    cancelBallWait = false;
    ballsSinceLastFullRack = 0;

    for (int i = 0; i < TOTAL_PINS; i++) {
      pinStates[i] = true;
      remainingPins[i] = i;
    }
    remainingCount = TOTAL_PINS;
    fallenCount = 0;

    Serial.println(">>> GAME OVER: Sending all pins DOWN");
    sendMotorCommandRemaining("down");
    delay(1000);
    Serial.println(">>> GAME OVER: Sending all pins FREE");
    sendMotorCommandRemaining("free");
    currentState = STATE_FREE;
    waitingForStatus = false;
    statusCount = 0;
    for (int i = 0; i < TOTAL_PINS; i++) slaveReplied[i] = false;
  }
}

// USB Serial (monitor): type `down` + Enter → send DOWN to all pin addresses.
static const size_t SERIAL_CMD_MAX = 64;
static char serialMonitorLine[SERIAL_CMD_MAX];
static size_t serialMonitorLineLen = 0;

void pollSerialMonitor() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serialMonitorLine[serialMonitorLineLen] = '\0';
      char *s = serialMonitorLine;
      while (*s == ' ' || *s == '\t') s++;
      char *end = s + strlen(s);
      while (end > s && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
      }
      if (strcmp(s, "down") == 0) {
        Serial.println(">>> Serial: DOWN to all pins");
        sendMotorCommand("down");
      }
      serialMonitorLineLen = 0;
      continue;
    }
    if (serialMonitorLineLen < SERIAL_CMD_MAX - 1) {
      serialMonitorLine[serialMonitorLineLen++] = c;
    } else {
      serialMonitorLineLen = 0;
    }
  }
}

// Poll ESP serial link (JSON lines)

void pollESPSerial() {
  while (espSerial.available()) {
    char c = (char)espSerial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      espSerialLine[espSerialLineLen] = '\0';
      handleESPCommand(espSerialLine);
      espSerialLineLen = 0;
      continue;
    }
    if (espSerialLineLen < SERIAL_LINE_MAX - 1) {
      espSerialLine[espSerialLineLen++] = c;
    } else {
      espSerialLineLen = 0;
    }
  }
}

// ===================== Setup =====================

void setup() {
  pinMode(RS485_DE_PIN, OUTPUT);
  pinMode(RS485_RE_PIN, OUTPUT);
  rs485Receive();

  pinMode(BALL_SENSOR_PIN, INPUT_PULLUP);

  RS485.begin(BAUDRATE);
  Serial.begin(115200);
  espSerial.begin(ESP_BAUD);

  // Drive FRAM Write Protect pin LOW to allow writes
  pinMode(FRAM_WP_PIN, OUTPUT);
  digitalWrite(FRAM_WP_PIN, LOW);

  // Initialize I2C for FRAM
  Wire.setSDA(I2C_SDA_PIN);
  Wire.setSCL(I2C_SCL_PIN);
  Wire.begin();
  delay(10); // let I2C bus settle
  framAvailable = framDetect();
  if (framAvailable) {
    Serial.println("✅ FRAM detected at 0x50");
  } else {
    Serial.println("⚠️ FRAM not detected at 0x50 — config storage unavailable");
    Serial.println("   Check: SDA/SCL wiring, 4.7K pull-ups, A0/A1/A2 tied to GND, VDD=3.3V");
  }

  for (int i = 0; i < TOTAL_PINS; i++) {
    pinStates[i] = true;
  }

  delay(2000);
  Serial.println("=== Host Started: Non-Blocking Mode ===");
  Serial.println("Serial: type \"down\" + Enter to lower all pins");
  Serial.print("ESP link: RX=");
  Serial.print(ESP_RX_PIN);
  Serial.print(" TX=");
  Serial.print(ESP_TX_PIN);
  Serial.print(" @ ");
  Serial.print(ESP_BAUD);
  Serial.println(" baud");
  Serial.print("FRAM: ");
  Serial.println(framAvailable ? "OK" : "NOT FOUND");

  Serial.println(">>> Initializing: ...");
  sendStatusReq();
  waitingForStatus = true;
}

// ===================== Main Loop =====================

void loop() {
  // 0. USB serial (manual commands)
  pollSerialMonitor();
  // 1. ESP serial (JSON)
  pollESPSerial();

  // 2. RS485 replies (non-blocking)
  if (RS485.available()) {
    String raw = RS485.readStringUntil('\n');
    int start = raw.indexOf('{');

    if (start != -1) {
      String json = raw.substring(start);
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, json);

      if (!err) {
        if (doc["cmd"] == "status_res") {
          int addr = doc["addr"];

          if (addr >= 0 && addr < TOTAL_PINS && !slaveReplied[addr]) {
            slaveReplied[addr] = true;
            statusCount++;

            bool pin = doc["pin"] | true;
            if (addr >= 0 && addr < TOTAL_PINS) {
              pinStates[addr] = pinStates[addr] && pin;
            }

            Serial.print("Slave ");
            Serial.print(addr);
            Serial.print(" replied. Total: ");
            Serial.print(statusCount);
            Serial.print(" pinState=");
            Serial.println(pinStates[addr]);
          }
        }
        
        delay(100);
      }
    }
  }

  // 3. CHECK IF ALL SLAVES REPLIED
  if (waitingForStatus && statusCount >= TOTAL_PINS) {
    if (pendingGameStart) {
      Serial.println("✅ Game start: all slaves online — sending UP");
      pendingGameStart = false;
      sendMotorCommand("up");
      currentState = STATE_HOLD;
      statusCount = 0;
      for (int i = 0; i < TOTAL_PINS; i++) slaveReplied[i] = false;
      sendStatusReq();
      waitingForStatus = true;
      return;
    }

    Serial.println(">>> All slaves replied. Switching State...");

    remainingCount = 0;
    fallenCount = 0;
    for (int i = 0; i < TOTAL_PINS; i++) {
      if (pinStates[i]) {
        remainingPins[remainingCount++] = i;
      } else {
        fallenPins[fallenCount++] = i;
      }
    }

    if (ballJustDetected) {
      Serial.println("🎳 Ball cycle complete - sending pins to ESP");
      sendBallDetectedToESP();
      ballJustDetected = false;

      bool allPinsDown = (remainingCount == 0);
      ballsSinceLastFullRack++;

      if (allPinsDown || ballsSinceLastFullRack >= 3) {
        Serial.println("🎯 Scheduling full rack on next cycle");
        for (int i = 0; i < TOTAL_PINS; i++) {
          pinStates[i] = true;
        }
        ballsSinceLastFullRack = 0;
      }
    }

    statusCount = 0;
    for (int i = 0; i < TOTAL_PINS; i++) slaveReplied[i] = false;

    // Switch State Logic
    if (currentState == STATE_FORWARD) {
      delay(500);
      Serial.println(">>> Sending: UP");
      sendMotorCommand("up");
      currentState = STATE_HOLD;
    } else if (currentState == STATE_HOLD) {
      delay(50);
      Serial.println(">>> Sending: HOLD");
      sendMotorCommand("stop");
      currentState = STATE_REVERSE;
    } else if (currentState == STATE_REVERSE) {
      delay(1000);

      Serial.println("----- PIN STATUS BEFORE REVERSE -----");
      Serial.print("Remaining Pins (Standing): ");
      for (int i = 0; i < remainingCount; i++) {
        Serial.print(remainingPins[i]);
        Serial.print(" ");
      }
      Serial.println();
      Serial.print("Fallen Pins: ");
      for (int i = 0; i < fallenCount; i++) {
        Serial.print(fallenPins[i]);
        Serial.print(" ");
      }
      Serial.println();
      Serial.println("--------------------------------------");

      // No standing pins (e.g. strike): remaining list is empty, but every pin
      // still ran UP and must receive DOWN/FREE to complete the rack cycle.
      Serial.println(">>> Sending: REVERSE");
      if (remainingCount == 0) {
        Serial.println("   (all pins down — full-rack down to all addresses)");
        sendMotorCommand("down");
      } else {
        sendMotorCommandRemaining("down");
      }

      currentState = STATE_FREE;
    } else if (currentState == STATE_FREE) {
      if (gameOver) {
        Serial.println("🏁 Game over - lane is FREE and idle");
      } else {
        Serial.println(">>> Sending: FREE");
        if (remainingCount == 0) {
          sendMotorCommand("free");
        } else {
          sendMotorCommandRemaining("free");
        }

        Serial.println("Waiting for ball...");
        bool ballDetected = false;
        
        while (!ballDetected) {
          pollSerialMonitor();
          pollESPSerial();

          if (cancelBallWait) {
            Serial.println("🔄 Game/reset received while waiting for ball - cancelling wait");
            cancelBallWait = false;
            break;
          }
          
          if (digitalRead(BALL_SENSOR_PIN) == LOW) {
            ballDetected = true;
            break;
          }
        }

        if (ballDetected) {
          Serial.println("Ball detected, waiting 2 seconds...");
          delay(2000);
          ballJustDetected = true;
          currentState = STATE_FORWARD;
        } else {
          Serial.println("🔄 Ball wait cancelled due to game/reset - resetting to forward");
          ballJustDetected = false;
          currentState = STATE_FORWARD;
        }
      }
    }

    sendStatusReq();
    waitingForStatus = true;
  }
}
