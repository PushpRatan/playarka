// RS485 Master with Ball Sensor State Machine
// Board: generic Arduino-compatible MCU acting as RS485 master
//
// Behavior:
// - Four motor states: STATE_FORWARD, STATE_HOLD, STATE_REVERSE, STATE_FREE
// - In STATE_FREE, motor is commanded "free" and outputs stay stable
//   until ball sensor detects a ball (sensor goes from 1 -> 0).
// - Once ball is detected, wait 2 seconds, then transition to STATE_FORWARD.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SoftwareSerial.h>

// RS485 bus (to slaves)
#define RS485_TX_PIN 0
#define RS485_RX_PIN 1
#define RS485_DE_PIN 2
#define RS485_RE_PIN 3

#define BAUDRATE 9600

// Ball sensor pin:
// Assumption: sensor outputs 1 when NO ball is present, 0 when ball IS present.
// Change this pin number and pinMode configuration in setup() if your wiring differs.
#define BALL_SENSOR_PIN 13

// ESP32 link (to PlayArka master ESP32-S3)
// Match the wiring used in your simple RP<->ESP test:
// RP TX 20 -> ESP RX 44
// RP RX 21 -> ESP TX 43
#define ESP_RX_PIN 21
#define ESP_TX_PIN 20
#define ESP_BAUD   115200

SoftwareSerial RS485(RS485_RX_PIN, RS485_TX_PIN);
SoftwareSerial espSerial(ESP_RX_PIN, ESP_TX_PIN);

// Variables for logic
int statusCount = 0;
bool slaveReplied[4] = {false}; // Track unique replies

// Serial buffer for ESP communication
static const size_t SERIAL_LINE_MAX = 512;
static char espSerialLine[SERIAL_LINE_MAX];
static size_t espSerialLineLen = 0;

// Pin tracking: 4 pins total; all 4 (0–3) come from real slaves
// For testing, a STRIKE means all 4 pins have fallen.
const int TOTAL_PINS = 4;
// true -> standing, false -> fallen; for real pins (0–3) this is latched:
// once a pin becomes false, it stays false.
bool pinStates[TOTAL_PINS];
int remainingPins[TOTAL_PINS];
int fallenPins[TOTAL_PINS];
int remainingCount = 0;
int fallenCount = 0;

// Flag to indicate that a ball was just detected and we should send pins to ESP
bool ballJustDetected = false;

// Flag to cancel any in-progress ball-wait when a new game / reset command arrives
bool cancelBallWait = false;

// TIMER VARIABLES
unsigned long waitStartTime = 0;
bool waitingToSendStatus = false; // Flag to track if we are in the "gap"
const long interval = 3000;       // 3 second delay

enum MotorState {
  STATE_FORWARD,
  STATE_REVERSE,
  STATE_HOLD,
  STATE_FREE
};

MotorState currentState = STATE_FORWARD;

// ================= RS485 Functions =================
void rs485Transmit() {
  digitalWrite(RS485_DE_PIN, HIGH);
  digitalWrite(RS485_RE_PIN, HIGH);
}

void rs485Receive() {
  digitalWrite(RS485_DE_PIN, LOW);
  digitalWrite(RS485_RE_PIN, LOW);
}

void sendMotorCommand(const char *direction, const char *speed) {
  JsonDocument doc;
  doc["cmd"] = "motor";
  JsonArray addr = doc["addr"].to<JsonArray>();
  // Send command to ALL pins (0..TOTAL_PINS-1)
  for (int i = 0; i < TOTAL_PINS; i++) addr.add(i);
  doc["dir"] = direction;
  //doc["spd"] = speed;

  rs485Transmit();
  serializeJson(doc, RS485);
  RS485.println();
  RS485.flush();
  rs485Receive();
}

void sendMotorCommandRemaining(const char *direction, const char *speed) {
  JsonDocument doc;
  doc["cmd"] = "motor";
  JsonArray addr = doc["addr"].to<JsonArray>();
  // Send command ONLY to remaining (non-fallen) pins
  for (int i = 0; i < remainingCount; i++) addr.add(remainingPins[i]);
  doc["dir"] = direction;
  //doc["spd"] = speed;

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
    fallen.add(fallenPins[i] + 1); // Convert 0-9 to 1-10 for pin numbers
  }
  
  JsonArray remaining = doc["remainingPins"].to<JsonArray>();
  for (int i = 0; i < remainingCount; i++) {
    remaining.add(remainingPins[i] + 1); // Convert 0-9 to 1-10 for pin numbers
  }
  
  // Calculate strike/spare
  bool isStrike = (fallenCount == TOTAL_PINS);
  bool isSpare = (fallenCount == TOTAL_PINS && remainingCount == 0);
  doc["isStrike"] = isStrike;
  doc["isSpare"] = isSpare;

  // Send JSON to ESP over dedicated serial link
  serializeJson(doc, espSerial);
  espSerial.println();
  espSerial.flush();

  // Also log the exact JSON to the PC Serial monitor
  Serial.print("RP -> ESP : ");
  serializeJson(doc, Serial);
  Serial.println();
  Serial.flush();

  Serial.print("📤 Sent ball detection to ESP: fallen=");
  Serial.print(fallenCount);
  Serial.print(", remaining=");
  Serial.println(remainingCount);
}

// Handle commands from ESP
void handleESPCommand(const char *line) {
  if (!line) return;
  while (*line == ' ' || *line == '\t') line++;
  if (*line == '\0') return;

  // Log raw command from ESP (this will also be visible on ESP side)
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
  if (strcmp(cmd, "gameStart") == 0) {
    Serial.println("🎮 Game start command from ESP - sending forward to all slaves");
    // Reset all pins to standing
    for (int i = 0; i < TOTAL_PINS; i++) {
      pinStates[i] = true;
    }
    // Cancel any in-progress ball detection wait from a previous game
    ballJustDetected = false;
    cancelBallWait = true;
    // Send forward command to all slaves
    currentState = STATE_FORWARD;
    waitingToSendStatus = false;
    statusCount = 0;
    for (int i = 0; i < 4; i++) slaveReplied[i] = false;
    // Trigger immediate forward command
    sendMotorCommand("up", "slow");
    currentState = STATE_HOLD;
    waitingToSendStatus = true;
    waitStartTime = millis();
  } else if (strcmp(cmd, "playerChange") == 0) {
    Serial.println("👤 Player change command from ESP - sending forward to all slaves");
    // Reset all pins to standing for new player
    for (int i = 0; i < TOTAL_PINS; i++) {
      pinStates[i] = true;
    }
    // Send forward command to all slaves
    currentState = STATE_FORWARD;
    waitingToSendStatus = false;
    statusCount = 0;
    for (int i = 0; i < 4; i++) slaveReplied[i] = false;
    // Trigger immediate forward command
    sendMotorCommand("up", "slow");
    currentState = STATE_HOLD;
    waitingToSendStatus = true;
    waitStartTime = millis();
  } else if (strcmp(cmd, "reset") == 0) {
    Serial.println("🔄 Reset command from ESP");
    // Reset everything
    for (int i = 0; i < TOTAL_PINS; i++) {
      pinStates[i] = true;
    }
    // Cancel any in-progress ball detection wait
    ballJustDetected = false;
    cancelBallWait = true;
    currentState = STATE_FORWARD;
    waitingToSendStatus = false;
    statusCount = 0;
    for (int i = 0; i < 4; i++) slaveReplied[i] = false;
  }
}

// Poll ESP serial link for commands from ESP
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
      // Line too long; reset
      espSerialLineLen = 0;
    }
  }
}

// ================= Setup =================
void setup() {
  pinMode(RS485_DE_PIN, OUTPUT);
  pinMode(RS485_RE_PIN, OUTPUT);
  rs485Receive();

  // Configure ball sensor input.
  // If your sensor is open-collector to ground with a pull-up resistor, use INPUT.
  // If you wired it directly between pin and ground, and want to use internal pull-up,
  // change this to INPUT_PULLUP and invert the logic in loop() if needed.
  pinMode(BALL_SENSOR_PIN, INPUT_PULLUP);

  RS485.begin(BAUDRATE);
  Serial.begin(115200);
  espSerial.begin(ESP_BAUD);

  // Initialize all pins as standing at start
  for (int i = 0; i < TOTAL_PINS; i++) {
    pinStates[i] = true;
  }

  delay(2000);
  Serial.println("=== Host Started: Non-Blocking Mode ===");
  Serial.print("ESP link: RX=");
  Serial.print(ESP_RX_PIN);
  Serial.print(" TX=");
  Serial.print(ESP_TX_PIN);
  Serial.print(" @ ");
  Serial.print(ESP_BAUD);
  Serial.println(" baud");

  // Start Sequence
  Serial.println(">>> Initializing: ...");

  // Set timer to send Status Request after initial interval
  waitingToSendStatus = true;
  waitStartTime = millis();
}

// ================= Main Loop =================
void loop() {
  // 0. ALWAYS CHECK FOR COMMANDS FROM ESP (Non-blocking)
  pollESPSerial();
  
  // 1. ALWAYS LISTEN (Non-blocking)
  if (RS485.available()) {
    String raw = RS485.readStringUntil('\n');
    int start = raw.indexOf('{');

    if (start != -1) {
      String json = raw.substring(start);
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, json);

      if (!err) {
        // Handle Status Response
        if (doc["cmd"] == "status_res") {
          int addr = doc["addr"];

          // Only count if this slave hasn't been counted yet this cycle
          if (addr >= 0 && addr < TOTAL_PINS && !slaveReplied[addr]) {
            slaveReplied[addr] = true;
            statusCount++;

            // Track & latch pin state for this slave (0–3).
            // Once a pin becomes false (fallen), it stays false.
            bool pin = doc["pin"] | true;  // default true if missing
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
        // Handle Alerts IMMEDIATELY (Example)
        else if (doc["cmd"] == "alert") {
          Serial.println("!!! ALERT RECEIVED !!!");
          delay(1500);
          currentState = STATE_FORWARD;
          waitingToSendStatus = false;
          statusCount = TOTAL_PINS;

          // You can handle emergency stop here immediately
        }
        delay(100);
      }
    }
  }

  // 2. CHECK IF WAITING PERIOD IS OVER
  if (waitingToSendStatus == true) {
    if (millis() - waitStartTime >= interval) {
      // Time is up! Send the Status Request now
      Serial.println(">>> Timer Done. Requesting Status...");
      sendStatusReq();

      waitingToSendStatus = false; // Stop waiting
    }
  }

  // 3. CHECK IF ALL SLAVES REPLIED
  // Only check this if we are NOT currently waiting on the timer
  if (!waitingToSendStatus && statusCount >= TOTAL_PINS) {
    Serial.println(">>> All slaves replied. Switching State...");

    // Build remainingPins and fallenPins arrays from latest pinStates
    remainingCount = 0;
    fallenCount = 0;
    for (int i = 0; i < TOTAL_PINS; i++) {
      if (pinStates[i]) {
        remainingPins[remainingCount++] = i;
      } else {
        fallenPins[fallenCount++] = i;
      }
    }

    // If a ball was just detected, send the current pins to ESP now
    if (ballJustDetected) {
      Serial.println("🎳 Ball cycle complete - sending pins to ESP");
      sendBallDetectedToESP();
      ballJustDetected = false;
    }

    // Reset counters for next round
    statusCount = 0;
    for (int i = 0; i < TOTAL_PINS; i++) slaveReplied[i] = false;

    // Switch State Logic
    if (currentState == STATE_FORWARD) {
      delay(500);
      Serial.println(">>> Sending: UP");
      sendMotorCommand("up", "slow");
      currentState = STATE_HOLD;
    } else if (currentState == STATE_HOLD) {
      delay(50);
      Serial.println(">>> Sending: HOLD");
      sendMotorCommand("stop", "slow");
      currentState = STATE_REVERSE;
    } else if (currentState == STATE_REVERSE) {
  delay(1000);

  // ===== PRINT REMAINING & FALLEN PINS BEFORE MOTOR COMMAND =====
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

  // ===== SEND MOTOR COMMAND =====
  Serial.println(">>> Sending: REVERSE");
  sendMotorCommandRemaining("down", "slow");

  currentState = STATE_FREE;
    } else if (currentState == STATE_FREE) {
      Serial.println(">>> Sending: FREE");
      // Only remaining (non-fallen) pins get FREE command
      sendMotorCommandRemaining("free", "slow");

      // Keep pins/motor in FREE until the ball is detected.
      Serial.println("Waiting for ball...");
      // Assumed logic: sensor 1 = no ball, 0 = ball detected.
      // Non-blocking wait for ball detection
      bool ballDetected = false;
      unsigned long ballWaitStart = millis();
      const unsigned long BALL_TIMEOUT = 60000; // 60 second timeout
      
      while (!ballDetected && (millis() - ballWaitStart < BALL_TIMEOUT)) {
        // Check for ESP commands while waiting (e.g. new game/reset)
        pollESPSerial();

        // If a new game / reset was requested, cancel this ball wait immediately
        if (cancelBallWait) {
          Serial.println("🔄 Game/reset received while waiting for ball - cancelling wait");
          cancelBallWait = false;
          ballDetected = false;
          break;
        }
        
        // Check ball sensor
        if (digitalRead(BALL_SENSOR_PIN) == LOW) {
          ballDetected = true;
          break;
        }
        delay(20); // small delay to avoid tight busy-wait
      }

      if (ballDetected) {
        // Ball detected, now wait 2 seconds before switching state.
        Serial.println("Ball detected, waiting 2 seconds...");
        delay(2000);

        // Mark that we need to send pins to ESP after next status cycle
        ballJustDetected = true;
        currentState = STATE_FORWARD;
      } else {
        Serial.println("⚠️ Ball not detected (timeout/cancel) - resetting to forward");
        ballJustDetected = false;
        currentState = STATE_FORWARD;
      }
    }

    // Start the Non-Blocking Timer again
    // We just sent a motor command, so we wait before asking for status
    waitingToSendStatus = true;
    waitStartTime = millis();
  }
}


