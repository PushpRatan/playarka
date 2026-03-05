# Backend Server

Simple Node.js backend for PlayArka Duckpin Bowling - runs on local WiFi network with in-memory storage.

## Setup

1. Install dependencies:
```bash
npm install
```

2. Start the server:
```bash
npm run backend
```

Or with auto-reload:
```bash
npm run dev:backend
```

## Configuration

**MQTT settings are hardcoded in `server.js`:**
- MQTT Broker: `g11e070b.ala.asia-southeast1.emqxsl.com:8883` (TLS/SSL)
- Username: `Pushp`
- Password: `Pushp9029@r`
- Device ID: `1`
- Server Port: `3001`

**To change MQTT credentials**, edit the constants at the top of `server.js`:
```javascript
const MQTT_BROKER = 'mqtts://your-broker:8883';
const MQTT_USERNAME = 'your-username';
const MQTT_PASSWORD = 'your-password';
const DEVICE_ID = '1';
```

### MQTT Topics

- `playarka/device/1/state` - All state management
- `playarka/device/1/pins` - Pin states (from ESP32)

## API Endpoints

- `GET /api/state` - Get current state
- `POST /api/players` - Add player `{ "name": "John Doe" }`
- `DELETE /api/players/:index` - Remove player
- `POST /api/payment/request` - Request payment `{ "amount": 500 }`
- `POST /api/payment/webhook` - Payment webhook (from payment gateway)
- `POST /api/reset` - Reset state (for testing)

## MQTT Actions Handled

- `scan` - QR code scanned
- `addPlayer` - Add a player
- `ready` - Players ready for payment
- `requestPayment` - Request payment QR

## State Management

State is stored in-memory (no database). State includes:
- Current status (waiting, onboarding, payment, active, completed)
- Players list
- Payment QR and status
- Game state (current player, frame, roll)

## Payment Integration

Currently uses mock payment QR generation. To integrate with actual payment gateway:

1. Replace `generatePaymentQr()` in `server.js` with your payment gateway API
2. Set up webhook endpoint to receive payment status
3. Update `handlePaymentStatus()` to verify payment

## Network Access

The server binds to `0.0.0.0` so it's accessible from other devices on your WiFi network.

When you start the server, it will display:
- **Local URL**: `http://localhost:3001` (for same machine)
- **Network URL**: `http://192.168.x.x:3001` (for mobile/other devices)

Use the Network URL on your mobile device (must be on same WiFi).

## API Endpoints

- `GET /api/info` - Get server info (IP addresses, MQTT config)
- `GET /api/state` - Get current state
- `POST /api/players` - Add player `{ "name": "John Doe" }`
- `DELETE /api/players/:index` - Remove player
- `POST /api/payment/request` - Request payment `{ "amount": 500 }`
- `POST /api/payment/webhook` - Payment webhook (from payment gateway)
- `POST /api/reset` - Reset state (for testing)

## Notes

- State is lost on server restart (in-memory storage)
- For production, consider adding file-based persistence or database
- MQTT messages are retained for state synchronization
- Server is accessible on local network (same WiFi) for mobile testing

