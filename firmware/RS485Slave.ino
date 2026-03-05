#include <Arduino.h>
#include <ArduinoJson.h>
#include <SoftwareSerial.h>
#include <Wire.h>

// ================= PIN DEFINITIONS =================
#define STEP_PIN 2
#define DIR_PIN  3
#define EN_PIN   6
#define SW1      1

#define ENC_A    17
#define ENC_B    7
#define LED 16

#define FANA 27
#define FANB 28

// ================= FRAM (FM24CL64B) =================
#define I2C_SDA_PIN 10
#define I2C_SCL_PIN 11
#define FRAM_ADDR        0x50
#define FRAM_DIR_ADDR    0x0000   // 1 byte only

// ===== RS485 Pins =====
#define RS485_TX_PIN 20   // DI
#define RS485_RX_PIN 21   // RO
#define RS485_DE_PIN 22   // DE
#define RS485_RE_PIN 23   // RE
#define BAUDRATE 9600

// ================= DIP SWITCH =================
#define DIP0  15    // LSB
#define DIP1  14
#define DIP2  13
#define DIP3  12   // MSB

// ================= SETTINGS =================
#define STEPS_PER_REV 200
#define MICROSTEPS   16

#define DIR_UP    HIGH
#define DIR_DOWN  LOW

#define HOMING_ENCODER_THRESHOLD 400
#define RECOVERY_REVS            8
// Operation Parameters

#define BOTTLE_KICK_THRESHOLD -200   // Negative encoder value to trigger check


// ================= STATES =================
enum State {
  STATE_UP,
  STATE_HOLD,
  STATE_DOWN,
  STATE_FREE,
  STATE_STUCK,
  STATE_MONITOR_KICK
};



// ================= GLOBALS =================

int alert = 0; // detect alert : 1 -> stuck, 2 -> timeout
int MY_ADDR = 0;

SoftwareSerial RS485(RS485_RX_PIN, RS485_TX_PIN); // RX, TX

State currentState = STATE_HOLD;

volatile long encoderCount = 0;

// ---- motion tracking ----
long stepCounter = 0;
int  revCounter  = 0;
bool moveActive  = false;

//volatile long counter = 0;
long tempCounter = 0;  

int addr = -1;

String motorDir = "stop";
bool waitingForStatus = false;  //when master send req then it become true.

bool pinState = true;  //detect bottle: true-> bottle stand. false-> bottle fall.

unsigned long motionStartTime = 0;
const unsigned long TIMEOUT_MS = 20000;       // <--- MODIFIED: 20 Seconds Limit


/* =====================================================
   >>> FRAM FUNCTIONS
   ===================================================== */
void framWriteByte(uint16_t addr, uint8_t value) {
  Wire1.beginTransmission(FRAM_ADDR);
  Wire1.write(addr >> 8);
  Wire1.write(addr & 0xFF);
  Wire1.write(value);
  Wire1.endTransmission(true);
}

uint8_t framReadByte(uint16_t addr) {
  Wire1.beginTransmission(FRAM_ADDR);
  Wire1.write(addr >> 8);
  Wire1.write(addr & 0xFF);
  Wire1.endTransmission(false);
  Wire1.requestFrom(FRAM_ADDR, (uint8_t)1);
  if (Wire1.available()) return Wire1.read();
  return 0xFF;
}

void framWriteDirection(int8_t value) {
  if (value != 1 && value != -1) return;
  framWriteByte(FRAM_DIR_ADDR, (uint8_t)value);
}

int8_t framReadDirection() {
  int8_t v = (int8_t)framReadByte(FRAM_DIR_ADDR);
  if (v != 1 && v != -1) {
    v = 1;                     // SAFE DEFAULT
    framWriteDirection(v);     // FIX FRAM
  }
  return v;
}

// ===== RS485 Functions =====
void rs485Transmit() {
  digitalWrite(RS485_DE_PIN, HIGH);
  digitalWrite(RS485_RE_PIN, HIGH);
}
void rs485Receive() {
  digitalWrite(RS485_DE_PIN, LOW);
  digitalWrite(RS485_RE_PIN, LOW);
}

// ================= DIP ADDRESS =================
int readDipAddress() {
  int a0 = digitalRead(DIP0) == LOW ? 1 : 0;
  int a1 = digitalRead(DIP1) == LOW ? 1 : 0;
  int a2 = digitalRead(DIP2) == LOW ? 1 : 0;
  int a3 = digitalRead(DIP3) == LOW ? 1 : 0;
  return (a3 << 3) | (a2 << 2) | (a1 << 1) | a0;
}

// ===== Send Status JSON =====
void sendStatus() {
  JsonDocument doc;
  doc["cmd"]  = "status_res";
  doc["addr"] = MY_ADDR;
  doc["sw1"] = (digitalRead(SW1) == HIGH) ? 1 : 0;   // Send actual switch state
  doc["pin"] = pinState; // Send actual switch state

  rs485Transmit();
  serializeJson(doc, RS485);   
  RS485.println();
  RS485.flush();
  rs485Receive();
}

//-----Send alert ----------------
void sendAlert() {
  JsonDocument doc;
  doc["cmd"] = "alert";
  doc["addr"] = MY_ADDR;
  doc["type"] = alert;

  rs485Transmit();
  serializeJson(doc, RS485);   
  RS485.println();
  RS485.flush();
  rs485Receive();
  //Serial.println("------OCP-----");
}


// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  pinMode(RS485_DE_PIN, OUTPUT);
  pinMode(RS485_RE_PIN, OUTPUT);
  RS485.begin(BAUDRATE);

  // DIP switches
  pinMode(DIP0, INPUT_PULLUP);
  pinMode(DIP1, INPUT_PULLUP);
  pinMode(DIP2, INPUT_PULLUP);
  pinMode(DIP3, INPUT_PULLUP);

  pinMode(LED, OUTPUT);

  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(EN_PIN, OUTPUT);

  pinMode(FANA,OUTPUT);
  pinMode(FANB,OUTPUT);

  digitalWrite(FANB,HIGH);
  digitalWrite(FANA, LOW);

  pinMode(SW1, INPUT_PULLUP);
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(ENC_A), encoderA, RISING);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encoderB, RISING);

  delay(100);
  digitalWrite(LED, LOW);

  MY_ADDR = readDipAddress();

  digitalWrite(EN_PIN, LOW); // enable motor
  resetEncoder();

  Serial.println("System Started");
  Serial.print("Slave addr= ");
  Serial.println(MY_ADDR);
  if (MY_ADDR == 15) {
    digitalWrite(LED, HIGH);
  }
}

// ================= MAIN LOOP =================
void loop() {
  //---------------------------------------------------------------------------------
  if (RS485.available()) {
    String raw = RS485.readStringUntil('\n');
    int start = raw.indexOf('{');
    if (start == -1) return; 
    String json = raw.substring(start);

    Serial.print("Rx: ");
    Serial.println(json);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
      Serial.print("JSON Error: ");
      Serial.println(err.c_str());
      return;
    }

    // ----- Motor Command -----
    if (doc["cmd"] == "motor") {
      JsonArray addrArr = doc["addr"];
      const char* dir  = doc["dir"];

      for (int i = 0; i < addrArr.size(); i++) {
        if (addrArr[i] == MY_ADDR) {
          motorDir  = dir;

          if (motorDir == "down") {
            digitalWrite(EN_PIN, LOW);
            currentState = STATE_DOWN;
            resetEncoder();
          } 
          else if (motorDir == "up") {
            digitalWrite(EN_PIN, LOW);
            currentState = STATE_UP;
            resetEncoder();
          } 
          else if (motorDir == "stop") {
            digitalWrite(EN_PIN, LOW);
            currentState = STATE_HOLD;
            resetEncoder();

          }
          else if(motorDir == "free"){
            currentState = STATE_FREE;
            digitalWrite(EN_PIN, HIGH); // Disable Motor
            delay(100);
            resetEncoder();

          }
          
          Serial.print("Cmd Executed: ");
          Serial.println(motorDir);
        }
      }
    }

    // ----- Status Request -----
    else if (doc["cmd"] == "status_req") {
       waitingForStatus = true; 
    }

    // ----- Relay Status Chain -----
    else if (doc["cmd"] == "status_res") {
       addr = doc["addr"];
    }

    //-----Alert Command-----
    else if (doc["cmd"] == "alert") {
      //motorStop();
      currentState = STATE_HOLD;
      digitalWrite(EN_PIN, LOW);
      Serial.println("----ALERT RX----");
    }
  }

  // Handle Status Response Logic
  if (waitingForStatus == true && moveActive == false) {
    if (MY_ADDR == 0) {
      delay(60);
      sendStatus(); 
      waitingForStatus = false;  
    } else if (addr == (MY_ADDR - 1)) {
      delay(100);
      sendStatus();
      waitingForStatus = false;
      addr = -1;
    }
  }

  // === Timer Calculation ===
  // Only calculate elapsed time if motionStartTime is NOT 0
  unsigned long motionElapsed = (motionStartTime > 0) ? millis() - motionStartTime : 0;

  if (motionStartTime > 0 && motionElapsed > TIMEOUT_MS) {
    Serial.println("⏱ 15s Timeout Reached → STOP");
    alert = 2;
    currentState = STATE_HOLD;
    //motorStop(); // This stops motor AND resets motionStartTime to 0
  }

  //---------------------------------------------------------------------------------
  switch (currentState) {

    case STATE_UP:                    // Moving UP
      movingUP();
      break;

    case STATE_DOWN:                  // Moving Down
      movingDOWN();
      break;

    case STATE_FREE:                  // Motor become free
      digitalWrite(EN_PIN, HIGH);
      moveActive = false;
      motionStartTime = 0;

      if (alert)
        break;

    case STATE_MONITOR_KICK:         //called after bottle kept down
          // A. Check for Kick (Negative Threshold)
      if (encoderCount < BOTTLE_KICK_THRESHOLD) {
        Serial.print("[STATE 3] Kick Detected at: ");
        Serial.println(encoderCount);
        tempCounter = encoderCount; 
        pinState = false;
        resetEncoder();
        break; // Exit switch immediately
      }

    case STATE_HOLD:
      digitalWrite(EN_PIN, LOW);
      motionStartTime = 0;
      moveActive = false;
      break;
  }
}

// =================================================
// ================= HOMING LOGIC ===================
// =================================================

void movingUP() {

  // ---- If limit switch hit → homing success ----
  if (digitalRead(SW1) == HIGH) {
    Serial.println("mOVing Successful");
    alert = 0;
    pinState = true;
    moveActive = false;
    currentState = STATE_HOLD;
    return;
  }

  // ---- Start homing movement ----
  if (!moveActive) {
    startMove(DIR_UP);
    Serial.println("MOVING UP...");
  }
  // ---- Step motor ----
  stepMotor(150);
  // ---- Check encoder every revolution ----
  if (stepCounter >= (long)STEPS_PER_REV * MICROSTEPS) {
    stepCounter = 0;
    revCounter++;

    Serial.print("Homing Rev ");
    Serial.print(revCounter);
    Serial.print(" | Encoder Δ = ");
    Serial.println(encoderCount);

    // ---- STUCK DETECTED ----
    if (encoderCount < HOMING_ENCODER_THRESHOLD) {
      Serial.println("Homing Stuck → Recovery Down");
      moveActive = false;
      currentState = STATE_HOLD;
      sendAlert();
    }
    resetEncoder();
  }
}

// =================================================
// ================= Moving DOWN ==================
// =================================================

void movingDOWN() {
  static bool started = false;
  //Serial.print("State 1: ");
  //Serial.println(started);
  if (!started) {
    Serial.println("Recovery: Moving DOWN fast (5 revs)");
    //Serial.println("Entered");
    startMove(DIR_DOWN);
    started = true;
  }

  // FAST DOWN (low delay = fast)
  if (moveStep(RECOVERY_REVS, 120)) {
    Serial.println("Recovery Done → Retry Homing");
    started = false;
    resetEncoder();
    currentState = STATE_HOLD;
    //currentState = STATE_UP;
  }
}

// =================================================
// ================= MOTOR FUNCTIONS =================
// =================================================

void startMove(bool dir) {
  motionStartTime = millis();        // START TIMER
  digitalWrite(DIR_PIN, dir);
  stepCounter = 0;
  revCounter  = 0;
  moveActive  = true;
}

bool moveStep(int totalRevs, int stepDelayUs) {
  if (!moveActive) return true;

  stepMotor(stepDelayUs);

  if (stepCounter >= (long)STEPS_PER_REV * MICROSTEPS) {
    stepCounter = 0;
    revCounter++;
  }

  if (revCounter >= totalRevs) {
    moveActive = false;
    return true;
  }

  return false;
}

void stepMotor(int us) {
  digitalWrite(STEP_PIN, HIGH);
  delayMicroseconds(us);
  digitalWrite(STEP_PIN, LOW);
  delayMicroseconds(us);
  stepCounter++;
}

// =================================================
// ================= ENCODER =======================
// =================================================

void resetEncoder() {
  noInterrupts();
  encoderCount = 0;
  interrupts();
}

void encoderA() {
  encoderCount += (digitalRead(ENC_B) == LOW) ? 1 : -1;
}

void encoderB() {
  encoderCount += (digitalRead(ENC_A) == LOW) ? -1 : 1;
}
