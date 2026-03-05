# PlayArka Duckpin Bowling - Architecture & Flow

## System Architecture

```
┌─────────────────┐         ┌──────────────┐         ┌─────────────┐
│   Web App       │◄───────►│  EMQX MQTT   │◄───────►│  Node.js    │
│   (React)       │         │   Broker     │         │  Backend    │
│   - QR Display  │         │              │         │  - API      │
│   - Onboarding  │         │              │         │  - Database │
│   - Game UI     │         │              │         │  - Payment  │
└─────────────────┘         └──────────────┘         └─────────────┘
                                      ▲
                                      │
                                      │
                            ┌─────────┴─────────┐
                            │                   │
                      ┌─────▼─────┐      ┌─────▼─────┐
                      │  Mobile   │      │  Payment  │
                      │  App/Web  │      │  Gateway  │
                      │           │      │           │
                      └───────────┘      └───────────┘
```

## Recommended Flow (Improved)

### Step 1: Initial QR Code
- **Web App**: Displays QR code with URL: `https://playarka.com/device/1`
- **QR Data**: Contains device ID
- **Mobile Scan**: Opens onboarding page
- **Web App**: Shows "Waiting for players..." screen (onboarding state)
- **MQTT**: Publish to `playarka/device/1/state` with `action: "scan"` when QR is scanned

### Step 2: Player Onboarding
- **Mobile**: Shows "Start Game" button → Opens player form
- **Form**: Add player names (minimum 1, maximum 4-6 players)
- **Real-time**: As players are added, web app updates in real-time via MQTT
- **MQTT**: `playarka/device/1/state` topic with actions:
  - `action: "addPlayer"` - Add player
  - `action: "update"` - Current state (retained)
  - `action: "ready"` - All players added

### Step 3: Payment Flow
- **Mobile**: "Pay Now" button appears when players are ready
- **Backend**: Creates payment session, generates payment QR
- **Web App**: Displays payment QR code
- **Mobile**: User scans payment QR → redirects to payment gateway
- **Payment Gateway**: Processes payment → sends webhook to backend
- **MQTT**: `playarka/device/1/state` topic with actions:
  - `action: "requestPayment"` - Payment QR request
  - `action: "paymentQr"` - Payment QR data (retained)
  - `action: "paymentStatus"` - Payment status updates

### Step 4: Game Start
- **Backend**: Verifies payment → publishes game start command
- **Web App**: Transitions to game screen
- **Mobile**: Can close or stay connected for live updates
- **MQTT**: `playarka/device/1/state` topic with actions:
  - `action: "gameStart"` - Game start command
  - `action: "gameState"` - Game state updates (retained)

## MQTT Topic Structure (Minimal - Only 2 Topics)

### Essential Topics Only

1. **`playarka/device/1/state`** - All state management (Mobile ↔ Backend ↔ Web)
   - `action: "scan"` - QR scanned
   - `action: "addPlayer"` - Add player
   - `action: "update"` - Current state (retained)
   - `action: "ready"` - Ready for payment
   - `action: "requestPayment"` - Request payment
   - `action: "paymentQr"` - Payment QR (retained)
   - `action: "paymentStatus"` - Payment status
   - `action: "gameStart"` - Game started
   - `action: "gameState"` - Game state updates (retained)

2. **`playarka/device/1/pins`** - Pin states from ESP32 (ESP32 → Web)

## MQTT Message Format

### State Topic (`playarka/device/1/state`)

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

**Current State (Retained):**
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

**Payment QR (Retained):**
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

**Game State (Retained):**
```json
{
  "action": "gameState",
  "currentPlayer": 0,
  "frame": 1,
  "roll": 1
}
```

### Pins Topic (`playarka/device/1/pins`)

```json
{
  "pins": [
    { "id": 1, "down": false },
    { "id": 2, "down": true }
  ]
}
```

## Database Schema (Node.js Backend)

### Sessions Table
```sql
CREATE TABLE sessions (
  id VARCHAR(255) PRIMARY KEY,
  deviceId VARCHAR(50) NOT NULL,
  status ENUM('waiting', 'onboarding', 'payment', 'active', 'completed') DEFAULT 'waiting',
  createdAt TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  updatedAt TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);
```

### Players Table
```sql
CREATE TABLE players (
  id VARCHAR(255) PRIMARY KEY,
  sessionId VARCHAR(255) NOT NULL,
  name VARCHAR(100) NOT NULL,
  playerIndex INT NOT NULL,
  createdAt TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (sessionId) REFERENCES sessions(id) ON DELETE CASCADE
);
```

### Payments Table
```sql
CREATE TABLE payments (
  id VARCHAR(255) PRIMARY KEY,
  sessionId VARCHAR(255) NOT NULL,
  amount DECIMAL(10,2) NOT NULL,
  currency VARCHAR(3) DEFAULT 'INR',
  status ENUM('pending', 'processing', 'success', 'failed') DEFAULT 'pending',
  paymentGateway VARCHAR(50),
  gatewayTransactionId VARCHAR(255),
  qrData TEXT,
  expiresAt TIMESTAMP,
  completedAt TIMESTAMP,
  createdAt TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (sessionId) REFERENCES sessions(id) ON DELETE CASCADE
);
```

### Games Table
```sql
CREATE TABLE games (
  id VARCHAR(255) PRIMARY KEY,
  sessionId VARCHAR(255) NOT NULL,
  status ENUM('active', 'completed') DEFAULT 'active',
  startedAt TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  completedAt TIMESTAMP,
  FOREIGN KEY (sessionId) REFERENCES sessions(id) ON DELETE CASCADE
);
```

## Technical Considerations

### 1. Session Management
- Generate unique session ID when QR is displayed
- Session expires after 15-30 minutes of inactivity
- Store session state in database + Redis for fast access

### 2. Real-time Synchronization
- Use MQTT retained messages for current state
- Web app subscribes to all device topics on load
- Mobile app publishes events, web app reacts

### 3. Payment Integration
- Use payment gateway (Razorpay, Stripe, Paytm)
- Generate payment QR with unique transaction ID
- Webhook endpoint to receive payment status
- Publish payment status via MQTT to web app

### 4. Error Handling
- Handle MQTT disconnections gracefully
- Retry failed operations
- Show user-friendly error messages
- Log all events for debugging

### 5. Security
- Authenticate MQTT connections (username/password)
- Use TLS/SSL for MQTT connections
- Validate session IDs
- Sanitize user inputs
- Rate limiting on API endpoints

### 6. Scalability
- Support multiple devices (device/1, device/2, etc.)
- Use device-specific MQTT topics
- Database indexes on sessionId, deviceId
- Consider Redis for session caching

## Implementation Recommendations

### Phase 1: Core Setup
1. Set up Node.js backend with Express
2. Configure EMQX MQTT broker
3. Create database schema
4. Implement MQTT client in React app

### Phase 2: QR & Onboarding
1. Generate QR code with session ID
2. Create onboarding page (mobile)
3. Implement player add/remove functionality
4. Real-time player list sync via MQTT

### Phase 3: Payment Integration
1. Integrate payment gateway
2. Generate payment QR
3. Webhook handler for payment status
4. MQTT payment status updates

### Phase 4: Game Integration
1. Connect game logic to session
2. Implement game state management
3. Real-time score updates
4. Game completion handling

## Alternative Flow Improvements

### Option A: Simplified Flow
- Combine player addition and payment in one step
- Show payment option immediately after first player
- Reduce number of steps

### Option B: Pre-payment Flow
- Allow players to be added before payment
- Payment only when ready to start
- Better UX for group coordination

### Option C: Post-payment Player Addition
- Pay first, then add players
- Ensures payment before game starts
- Simpler state management

## Recommended: Current Flow (Best UX)
Your current flow is good because:
1. ✅ Players can coordinate before payment
2. ✅ Payment happens when everyone is ready
3. ✅ Clear separation of concerns
4. ✅ Better for group bookings

## Next Steps

1. **Set up Node.js backend** with Express + MQTT client
2. **Create database** with the schema above
3. **Implement QR code generation** in React app
4. **Build onboarding page** (mobile-responsive)
5. **Integrate payment gateway**
6. **Connect MQTT** for real-time updates

Would you like me to start implementing any specific part of this architecture?

