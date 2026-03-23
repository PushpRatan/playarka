# Device–Lane Setup Plan (Production) — Token, Backend, MQTTS, Screens

This document describes:
1. What is **pre-stored** on the device (token, backend URL, device name, CA cert) vs what is **stored at setup** (WiFi + MQTTS credentials from backend).
2. How the **backend verifies the token** and returns MQTTS credentials.
3. How **score screens** know which lane/device to show (multiple lanes, multiple screens).

---

## 1. What is stored where

### 1.1 Pre-stored from the start (factory / initial provisioning)

Stored in **I2C FRAM** (or NVS) on the **ESP32-S3** so the device can talk to your backend and prove identity. No WiFi needed yet.

| Field | Size (guide) | Purpose |
|-------|--------------|---------|
| **Magic** | 2 B | e.g. `0x50 0x41` ("PA") – valid config block. |
| **Token** | e.g. 32–64 B | Opaque token; device sends this to backend; backend verifies and returns MQTTS credentials. Can be a UUID or signed JWT. |
| **Backend URL** | 1 B len + up to 128 B | Base URL for provisioning API, e.g. `https://api.playarka.com` or `https://backend.venue.com`. |
| **Device name** | 1 B len + up to 32 B | Human name, e.g. `"Lane-Controller-12"`; used in logs and in API. |
| **Root CA certificate** | ~1–2 KB (PEM or DER) | Used to verify TLS: both HTTPS to backend and MQTTS to broker. Single CA that signs backend + MQTT broker, or separate certs if you prefer. |

So from the start the device has: **identity (token + device name)**, **where to call (backend URL)**, and **how to trust TLS (CA cert)**.

### 1.2 Stored during setup (installer provides / backend returns)

At setup time the installer only gives **WiFi**. The rest comes from the backend after token verification.

| Field | When / who | Purpose |
|-------|------------|---------|
| **WiFi SSID** | Installer in setup form | Venue WiFi. |
| **WiFi password** | Installer in setup form | Venue WiFi. |
| **MQTTS host** | Backend (after token verify) | Broker hostname. |
| **MQTTS port** | Backend | e.g. 8883. |
| **MQTT username** | Backend | Broker auth. |
| **MQTT password** | Backend | Broker auth. |
| **Device/Lane ID** | Backend (optional) | If backend assigns lane id (e.g. by MAC or token), device stores it and uses in topics `playarka/device/<id>/...`. |

All of these can be written to the same FRAM config block (or NVS) during setup so the device survives reboot without calling the backend again (until you want credential rotation).

---

## 2. Setup flow (token → backend → MQTTS)

1. **Device is unconfigured** (e.g. no WiFi or “configured” flag not set).
2. **Installation:** Installer powers the device; it starts **WiFi AP** (e.g. `PlayArka-Setup-XXXX`).
3. **Installer** connects phone/laptop to that AP and opens the **setup page** (captive portal or `http://192.168.4.1`).
4. **Setup form** asks only for:
   - **WiFi SSID** (or scan and select)
   - **WiFi password**
5. Device **saves WiFi** to FRAM/NVS, connects to venue WiFi.
6. **First HTTPS call to backend** (using pre-stored backend URL and CA cert):
   - `POST /api/device/provision` (or similar)
   - Body: `{ "token": "<stored token>", "deviceName": "<stored name>", "mac": "<ESP MAC address>" }`
   - Headers: normal TLS (device verifies server with CA cert).
7. **Backend:**
   - Verifies **token** (lookup in DB or verify signature).
   - Optionally maps **MAC** to a known device/lane in your system.
   - Returns MQTTS credentials and, if you use it, **lane/device id**:
     - `{ "mqttHost": "...", "mqttPort": 8883, "mqttUsername": "...", "mqttPassword": "...", "deviceId": "1" }`
   - Optionally stores in DB: MAC / token → lane id, for admin and for “which device is which”.
8. **Device** stores MQTTS credentials (and optional deviceId) in FRAM/NVS, sets “configured”, then connects to MQTTS and uses `playarka/device/<deviceId>/...` for the rest of its life.

So: **token + backend URL + device name + CA cert** are from the start; **WiFi** from installer; **MQTTS credentials (and lane id)** from backend after token verification. MAC is for backend to recognise the physical device and assign lanes if needed.

---

## 3. Root CA certificate role

- **HTTPS to backend:** Device uses the stored **root CA certificate** to validate the backend’s TLS certificate. No `setInsecure()`.
- **MQTTS to broker:** Same CA (if your MQTT broker is signed by the same CA) or a second cert; device uses it to verify the broker.

Store the CA in FRAM (or NVS) as PEM or DER. At runtime, feed it into your TLS stack (e.g. `WiFiClientSecure.setCACert()` or equivalent on ESP32). One CA for both backend and broker keeps provisioning simple.

---

## 4. How the system can use MAC address

- Device sends **MAC** in the provision request. Backend can:
  - **Register** the device: “MAC aa:bb:cc:dd:ee:ff = Lane 3”.
  - **Idempotent provisioning:** Same MAC calling again gets same credentials (or refreshed).
  - **Admin UI:** List “devices seen” by MAC and assign them to lanes.

So the **device** is recognised by token (and optionally MAC); the **lane** it represents can be decided by the backend (and returned as `deviceId` in the provision response).

---

## 5. Multiple lanes and multiple score screens – how each screen knows its device/lane

**No lane selector:** Installer opens the **same link** manually with the right `?device=X` per screen (e.g. Lane 1: `?device=1`, Lane 2: `?device=2`). No separate “select lane” screen.

You have **multiple lanes**, each with:
- one **ESP32** (lane device),
- one **score screen** (tablet/TV showing the score UI).

The screens are just **browsers** showing your web app. They don’t have a hardware identity like the ESP’s MAC; they need a **logical** binding to a lane.

### 5.1 Recommended: URL (and optional stored setting) per lane

- **Each lane has a fixed URL** that includes the device/lane id:
  - Lane 1: `https://your-app.com/?device=1` (or `/score?device=1` if you have a dedicated score route)
  - Lane 2: `https://your-app.com/?device=2`
  - etc.
- **Score screen for Lane 1:** Installer opens that URL on the tablet/TV for lane 1, bookmarks it or sets it as the only page (kiosk). That screen then only ever shows **device 1** (state, scores, MQTT for device 1).
- **Score screen for Lane 2:** Same app, but URL with `device=2`; it shows only device 2.

So **“which screen shows which device”** = **which URL is loaded on that screen**. No need for the screen to “discover” the ESP or use MAC.

Implementation details:
- Your app already has a `device` (or `deviceId`) from the URL (e.g. `?device=1`). Use it everywhere: MQTT topic subscription (`playarka/device/1/state`), API calls, and UI title (“Lane 1”).
- Optionally: **persist device in localStorage** so that if someone opens `https://your-app.com/` without `?device=1`, you can show “Select lane” once and then save `deviceId = 1` and always load that lane on that browser. Good for kiosks that always open the same URL.

### 5.2 Optional: Lane selector / display registration

- **Lane selector page:** One URL for all screens, e.g. `https://your-app.com/select-lane`. Show buttons: “This screen is Lane 1”, “Lane 2”, … On tap, save `deviceId` in **localStorage** and redirect to the main app with `?device=1` (or equivalent). Next time that tablet opens the app (same URL), it reads `deviceId` from localStorage and shows that lane. So the “binding” is per-browser (per screen), not per URL.
- **Backend “display” registry:** If you want the backend to know “this display = Lane 2”, you can have the app call e.g. `POST /api/display/register` with a generated **displayId** (UUID in localStorage) and the chosen **deviceId**. Backend stores `displayId → deviceId`. Later you can change lane assignment in the admin without touching the tablet. The app still needs an initial way to pick the lane (URL or lane selector) or to receive `deviceId` from the backend based on displayId.

### 5.3 Summary: screen–device recognition

| Question | Answer |
|----------|--------|
| How does the **backend** know which device (ESP) is which? | Token + MAC in provision; backend assigns/returns `deviceId` (lane id) and can store MAC → lane. |
| How does a **score screen** know which lane/device to show? | **URL** with `device=1` (or 2, 3, …) for that lane’s screen; optionally **localStorage** or a one-time “This screen is Lane X” so the same URL can be used on all screens. |
| Do screens need to know the ESP’s MAC? | No. They only need the **logical** device/lane id (`device=1`, etc.), which is the same id the backend and ESP use in MQTT. |

So: **device (ESP)** is recognised by token (+ MAC) and gets MQTTS + lane id from the backend; **screens** are bound to a lane by **which URL they open** (and optionally by a stored lane choice or display registration).

---

## 6. FRAM layout sketch (ESP32-S3)

One possible layout for “from the start” + “after setup” in a single block:

```
Offset    Size      Content
------    -----     -------
0x0000    2         Magic "PA"
0x0002    1         Configured (0 = need setup, 1 = done)
0x0003    1         Token length (or fixed 32)
0x0004    64        Token
0x0044    1         Backend URL length
0x0045    128       Backend URL
0x00C5    1         Device name length
0x00C6    32        Device name
0x00E6    2         CA cert length (or 0 if not used)
0x00E8    ~1200     CA cert (PEM or DER)
------    -----     (rest: reserved or for WiFi + MQTT after setup)
0x0580    1         SSID length
0x0581    32        SSID
0x05A1    1         Pass length
0x05A2    64        WiFi password
0x05E2    1         MQTT host length
0x05E3    64        MQTT host
...
```

Exact offsets and sizes can be tuned; keep token, backend URL, device name, and CA in the “from the start” region, and WiFi + MQTT in the “after setup” region.

---

## 7. End-to-end summary

| Item | Detail |
|------|--------|
| **From the start (pre-stored)** | Token, backend URL, device name, root CA certificate (in FRAM/NVS). |
| **At setup (installer)** | WiFi SSID and password only. |
| **From backend (after token verify)** | MQTTS host/port/username/password; optionally device/lane id. Stored on device after first successful provision. |
| **TLS** | Device uses stored CA cert to verify backend (HTTPS) and broker (MQTTS). |
| **Device recognition** | Backend uses token + MAC to verify and to assign/return lane id. |
| **Score screens** | Each screen shows one lane by opening the app with **that lane’s URL** (`?device=1`, `?device=2`, …) or by a one-time “This screen is Lane X” and storing in localStorage. No MAC or hardware id needed on the screen. |

This gives you: secure provisioning (token + CA), backend-controlled MQTTS and lane assignment, and a simple, production-ready way for multiple score screens to each show the correct lane.

---

## 8. Further improvements (no lane selector; same link opened manually)

With **no lane selector** — installer just opens the same link with the right `?device=X` per screen — the following improvements make the system production-ready.

### 8.1 Frontend (score / main app)

| Improvement | Why |
|-------------|-----|
| **Use `device` from URL everywhere** | Today only OnboardingPage reads `?device=`; the main score screen and MQTT hook use hardcoded `"1"`. Derive `deviceId` from `window.location.search` (or a small hook/context) and use it for MQTT topic, API calls, and QR link. Default to `"1"` when param is missing. |
| **Show lane label on screen** | Small label in a corner or header: e.g. "Lane 1" / "Lane 2". Confirms which device this screen is showing; helps installers and operators. |
| **QR URL must include device** | The main screen’s QR (for mobile onboarding) should point to `.../onboarding?device=<same device id>`. So the QR is per-lane: each score screen shows the QR for that lane only. |
| **Kiosk-friendly (optional)** | Prevent sleep (Wake Lock API), optional fullscreen, hide cursor after idle, disable right-click/back if needed so the tablet stays on the score view. |

### 8.2 Backend

| Improvement | Why |
|-------------|-----|
| **Multi-device state** | One backend should serve many lanes. Keep state **per device id** (e.g. `stateByDevice[deviceId]`). Subscribe to `playarka/device/+/state` (and +/heartbeat, +/score) or maintain a list of device ids from config/DB. |
| **Provisioning API** | `POST /api/device/provision`: body `{ token, deviceName, mac }`. Verify token; return MQTTS credentials and `deviceId`. Optionally register MAC → deviceId for admin. |
| **Config from env** | MQTT broker URL, credentials, and any secrets in **environment variables**, not in code. Keeps repo safe and allows per-deployment config. |
| **Health / last seen** | Store last heartbeat time per device. Optional admin or status page: list devices, lane, last seen, connection status. |

### 8.3 Device (ESP32) setup

| Improvement | Why |
|-------------|-----|
| **Show device/lane on setup AP page** | After provisioning, or on the setup captive portal, show “This device: Lane 3” (or device name). Installer can confirm they configured the right unit. |
| **Clear “configured” on factory reset** | Button or GPIO hold at boot to clear WiFi + MQTT and set configured=0 so device re-enters setup. |

### 8.4 Ops / installer

| Improvement | Why |
|-------------|-----|
| **Single link pattern + doc** | Document: “For Lane N open: `https://<app>/?device=N`.” One pattern; installer only changes the number. |
| **Optional: printable QR per lane** | Generate and print a QR per lane (e.g. `https://<app>/?device=1`, 2, 3…). Installer sticks the right QR near each lane; scanning on the score tablet opens the right URL (no typing). |

### 8.5 Security

| Improvement | Why |
|-------------|-----|
| **Rate limit provision** | Limit requests per IP/token to avoid brute-force or token abuse. |
| **Token scope** | Tokens scoped per venue or per batch so a leaked token doesn’t affect all installs. |
| **CA pinning (optional)** | On ESP, pin the backend and/or broker certificate or public key in addition to (or instead of) full CA store if you want tighter control. |

### 8.6 Reliability

| Improvement | Why |
|-------------|-----|
| **Frontend reconnect** | MQTT client already reconnects; show “Reconnecting…” in the UI when disconnected and clear it when back. |
| **Backend persistence (optional)** | Persist game state (e.g. SQLite or DB) so a backend restart doesn’t lose the current game; optional for MVP. |
| **ESP reconnect** | Robust WiFi and MQTT reconnect with backoff; optional “provision again” if backend is unreachable for too long. |
