# ESP32-S3 ↔ RP Board Communication

## Overview

This document describes the communication protocol between the ESP32-S3 (ESP) and RP board (RP) for synchronized game control.

## Hardware Setup

- **ESP32-S3**: Uses Serial2 (RX pin 17, TX pin 18) at 115200 baud to communicate with RP
- **RP Board**: Uses Serial (native UART) at 115200 baud to communicate with ESP
- Both boards are on the same physical board, connected via UART

## Communication Protocol

### ESP → RP Commands

ESP sends JSON commands to RP when game events occur:

#### 1. Game Start

```json
{ "cmd": "gameStart" }
```

**Trigger**: When MQTT receives `{"action":"gameStart"}` from backend
**RP Action**:

- Resets all pins to standing state
- Sends forward command ("up") to all slaves via RS485
- Sets state machine to STATE_HOLD to continue normal cycle

#### 2. Player Change

```json
{ "cmd": "playerChange" }
```

**Trigger**: When MQTT receives `{"action":"update"}` with changed `currentPlayer`
**RP Action**:

- Resets all pins to standing state
- Sends forward command ("up") to all slaves via RS485
- Sets state machine to STATE_HOLD to continue normal cycle

#### 3. Reset

```json
{ "cmd": "reset" }
```

**Trigger**: When MQTT receives `{"action":"reset"}` from backend
**RP Action**:

- Resets all pins to standing state
- Resets state machine to STATE_FORWARD

### RP → ESP Data

RP sends ball detection data to ESP when a ball is detected:

#### Ball Detected

```json
{
  "type": "ballDetected",
  "fallenPins": [1, 4, 6],
  "remainingPins": [2, 3, 5, 7, 8, 9, 10],
  "isStrike": false,
  "isSpare": false
}
```

**Trigger**: When ball sensor detects a ball in STATE_FREE
**ESP Action**:

- Publishes SCORE message to MQTT with fallen/remaining pins
- Backend processes score and updates game state

**Note**: Pin numbers are 1-10 (not 0-9) when sent to ESP

## State Machine Flow

The RP board maintains its state machine as before:

1. **STATE_FORWARD**: Send "up" command to all pins
2. **STATE_HOLD**: Send "stop" command to all pins
3. **STATE_REVERSE**: Send "down" command to remaining pins
4. **STATE_FREE**: Send "free" command to remaining pins, wait for ball detection

When ball is detected in STATE_FREE:

- Wait 2 seconds
- Send ball detection data to ESP
- Transition to STATE_FORWARD

## Key Changes

### ESP32-S3 (PlayArkaMasterESP32S3.ino)

1. ✅ Added Serial2 initialization for RP communication
2. ✅ Added `sendCommandToRP()` function to send commands
3. ✅ Modified `onMqttMessage()` to detect `gameStart` and `playerChange` actions
4. ✅ Added `handleRPData()` to process ball detection data from RP
5. ✅ Added `pollSerial2()` to continuously listen for RP data
6. ✅ Removed random Serial monitor input (kept only for matchId and debugging)

### RP Board (RS485Master.ino)

1. ✅ Added `sendBallDetectedToESP()` to send ball detection data
2. ✅ Added `handleESPCommand()` to process commands from ESP
3. ✅ Added `pollESPSerial()` to continuously listen for ESP commands
4. ✅ Modified STATE_FREE to send data to ESP when ball detected
5. ✅ Added handling for `gameStart`, `playerChange`, and `reset` commands

## Pin Numbering

- **Internal (RP)**: Uses 0-9 for pin indices
- **External (ESP/MQTT)**: Converts to 1-10 for pin numbers
- **RS485 Slaves**: Addresses 0-3 correspond to physical pins 1-4
- **Virtual Pins**: Addresses 4-9 are randomly generated

## Testing

1. **Game Start Test**:
   - Start game from UI
   - Verify ESP sends `{"cmd":"gameStart"}` to RP
   - Verify RP sends forward command to all slaves
   - Verify pins stand up

2. **Player Change Test**:
   - Complete a roll (ball detected)
   - Verify player changes in backend
   - Verify ESP sends `{"cmd":"playerChange"}` to RP
   - Verify RP resets and sends forward command

3. **Ball Detection Test**:
   - Wait for STATE_FREE
   - Trigger ball sensor
   - Verify RP sends ball detection data to ESP
   - Verify ESP publishes SCORE to MQTT
   - Verify backend processes score correctly

## Troubleshooting

- **No communication**: Check Serial2 pins (17/18) on ESP and Serial pins on RP
- **Commands not received**: Verify baud rate is 115200 on both sides
- **Ball detection not sent**: Check ball sensor pin and logic (HIGH = no ball, LOW = ball)
- **State machine stuck**: Check RS485 communication with slaves
