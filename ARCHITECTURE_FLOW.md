# Architecture Flow - What Happens When QR is Scanned

## Complete Flow Diagram

```
┌─────────────┐
│  Web App   │  Displays QR Code
│  (React)   │  URL: http://localhost:5173/onboarding?device=1
└─────┬───────┘
      │
      │ QR Code Contains URL
      │
      ▼
┌─────────────┐
│   Mobile    │  User scans QR code
│   Device    │  Opens URL in browser
└─────┬───────┘
      │
      │ Opens /onboarding page
      │
      ▼
┌─────────────────────────────────┐
│   Mobile Onboarding Page        │
│   (OnboardingPage.tsx)          │
│                                 │
│   1. Connects to MQTT          │
│   2. Publishes "scan" action   │
│   3. Shows "Start Game" button  │
└─────┬───────────────────────────┘
      │
      │ MQTT: { action: "scan" }
      │ Topic: playarka/device/1/state
      │
      ▼
┌─────────────┐
│   Backend   │  Receives "scan" message
│  (Node.js)  │  Updates state: status = "onboarding"
└─────┬───────┘
      │
      │ MQTT: { action: "update", status: "onboarding" }
      │ (Retained message)
      │
      ▼
┌─────────────┐
│  Web App    │  Receives state update
│  (React)    │  Changes from "waiting" → "onboarding"
│             │  Shows WaitingScreen with player list
└─────────────┘
```

## Step-by-Step Flow

### Step 1: Initial State (Web App)
- **Web App** displays QR code
- QR code contains URL: `http://localhost:5173/onboarding?device=1`
- State: `waiting`
- Shows QR code on screen

### Step 2: QR Code Scanned (Mobile)
- User scans QR code with mobile device
- Mobile browser opens: `http://localhost:5173/onboarding?device=1`
- **OnboardingPage** component loads

### Step 3: Mobile Page Connects
- **OnboardingPage** connects to MQTT broker (WebSocket)
- On connection, automatically publishes:
  ```json
  {
    "action": "scan"
  }
  ```
- Topic: `playarka/device/1/state`
- Shows "Start Game" button and player form

### Step 4: Backend Receives Scan
- **Backend** receives `{ action: "scan" }` message
- Updates state: `status = "onboarding"`
- Publishes updated state:
  ```json
  {
    "action": "update",
    "status": "onboarding",
    "players": [],
    "ready": false,
    ...
  }
  ```
- Message is **retained** (so web app gets it immediately)

### Step 5: Web App Updates
- **Web App** receives state update via MQTT
- State changes: `waiting` → `onboarding`
- UI switches from QR code to **WaitingScreen**
- Shows "Waiting for Players" with empty player list

### Step 6: User Adds Players (Mobile)
- User clicks "Start Game" button
- User enters player name and clicks "Add"
- **OnboardingPage** publishes:
  ```json
  {
    "action": "addPlayer",
    "name": "John Doe"
  }
  ```

### Step 7: Backend Updates Players
- **Backend** receives `addPlayer` message
- Adds player to state
- Publishes updated state with new player list:
  ```json
  {
    "action": "update",
    "status": "onboarding",
    "players": ["John Doe"],
    "ready": false,
    ...
  }
  ```

### Step 8: Both Apps Update
- **Web App** receives update → Shows player in list
- **Mobile App** receives update → Shows player in list
- Both stay in sync via MQTT

### Step 9: Ready for Payment
- User clicks "Ready to Play" button
- **OnboardingPage** publishes:
  ```json
  {
    "action": "ready"
  }
  ```
- **Backend** updates: `ready = true`, `status = "payment"`
- User clicks "Pay Now" button
- **OnboardingPage** publishes:
  ```json
  {
    "action": "requestPayment",
    "amount": 500
  }
  ```

### Step 10: Payment Flow
- **Backend** generates payment QR
- Publishes payment QR:
  ```json
  {
    "action": "paymentQr",
    "qr": "upi://pay?..."
  }
  ```
- **Web App** shows payment QR on screen
- **Mobile App** shows payment QR (user can scan to pay)

### Step 11: Payment Success
- Payment gateway sends webhook to backend
- **Backend** publishes:
  ```json
  {
    "action": "paymentStatus",
    "status": "success"
  }
  ```
- **Backend** starts game:
  ```json
  {
    "action": "gameStart",
    "players": ["John Doe", "Jane Smith"]
  }
  ```

### Step 12: Game Active
- **Web App** receives `gameStart` → Shows game screen
- **Mobile App** can close or stay connected
- Game state updates continue via MQTT

## Key Points

1. **MQTT is the Communication Layer**
   - All state changes go through MQTT
   - Retained messages ensure new subscribers get current state
   - Real-time synchronization between web and mobile

2. **Backend is the Source of Truth**
   - Backend maintains state in memory
   - All state changes must go through backend
   - Backend validates and publishes updates

3. **Web App is Display Only**
   - Web app subscribes to MQTT and displays state
   - Web app doesn't modify state directly
   - Shows different screens based on state

4. **Mobile App is Input Only**
   - Mobile app publishes actions (scan, addPlayer, ready, requestPayment)
   - Mobile app also subscribes to see updates
   - User interacts through mobile, web app shows results

5. **No Direct Communication**
   - Web app and mobile app don't talk directly
   - All communication goes through MQTT broker
   - Backend processes all actions

## MQTT Topics Used

- **`playarka/device/1/state`** - All state management
  - Actions: `scan`, `addPlayer`, `ready`, `requestPayment`, `update`, `paymentQr`, `paymentStatus`, `gameStart`, `gameState`

## State Flow

```
waiting → onboarding → payment → active → completed
  ↑         ↑            ↑         ↑
  QR      Players      Payment   Game
Display   Added        Done      Running
```

## Why This Architecture?

1. **Scalable** - Can add more devices easily
2. **Real-time** - Instant updates via MQTT
3. **Decoupled** - Web and mobile don't need to know about each other
4. **Reliable** - MQTT handles reconnection, retained messages
5. **Simple** - Single source of truth (backend state)

