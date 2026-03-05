# MQTT Topics - Minimal Setup

## Only 2 Essential Topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `playarka/device/1/state` | All ↔ All | All state (scan, players, payment, game) |
| `playarka/device/1/pins` | ESP32 → Web | Pin states from hardware |

## Topic Details

### 1. State Topic (Everything)
**Topic:** `playarka/device/1/state`  
**Direction:** Mobile ↔ Backend ↔ Web  
**Retained:** Yes (for current state)

**QR Scanned:**
```json
{
  "action": "scan"
}
```

**Add Player:**
```json
{
  "action": "addPlayer",
  "name": "John Doe"
}
```

**Current State (Published by Backend):**
```json
{
  "action": "update",
  "players": ["John Doe", "Jane Smith"],
  "ready": false,
  "paymentQr": null,
  "paymentStatus": null,
  "gameStarted": false
}
```

**Ready for Payment:**
```json
{
  "action": "ready"
}
```

**Request Payment:**
```json
{
  "action": "requestPayment",
  "amount": 500
}
```

**Payment QR:**
```json
{
  "action": "paymentQr",
  "qr": "upi://pay?pa=merchant@upi&pn=PlayArka&am=500&cu=INR"
}
```

**Payment Status:**
```json
{
  "action": "paymentStatus",
  "status": "success"
}
```

**Game Start:**
```json
{
  "action": "gameStart",
  "players": ["John Doe", "Jane Smith"]
}
```

**Game State:**
```json
{
  "action": "gameState",
  "currentPlayer": 0,
  "frame": 1,
  "roll": 1
}
```

### 2. Pins Topic
**Topic:** `playarka/device/1/pins`  
**Direction:** ESP32 → Web

```json
{
  "pins": [
    { "id": 1, "down": false },
    { "id": 2, "down": true }
  ]
}
```

## Subscription

**Web App:**
```javascript
mqtt.subscribe('playarka/device/1/state', { qos: 1 })
mqtt.subscribe('playarka/device/1/pins', { qos: 1 })
```

**Mobile App:**
```javascript
mqtt.subscribe('playarka/device/1/state', { qos: 1 })
```

**Backend:**
```javascript
mqtt.subscribe('playarka/device/1/state', { qos: 1 })
```

## That's It!

Just 2 topics. Use the `action` field to differentiate message types.
