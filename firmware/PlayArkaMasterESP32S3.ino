/*
  PlayArka Master - ESP32-S3  (with Device Provisioning)

  Boot flow
  ─────────
  1. Read config from FRAM (via RP board over Serial2).
  2. If "configured" flag is set  → connect WiFi STA → connect MQTT → normal mode.
  3. If NOT configured            → start WiFi AP + captive portal.
     a. Installer connects to AP, opens setup page, enters venue WiFi creds.
     b. ESP connects to venue WiFi (AP+STA).
     c. ESP calls  POST /api/device/provision  with pre-stored token + MAC.
     d. Backend returns MQTT credentials + deviceId.
     e. ESP writes config to FRAM (via RP) and enters normal mode.

  Factory reset
  ─────────────
  Hold BOOT button (GPIO 0) LOW for 3+ seconds at power-on → FRAM config cleared → reboot → setup mode.

  Config storage
  ──────────────
  I2C FRAM (FM24CL64B) is on the RP master board.
  ESP32-S3 accesses it via Serial2 JSON commands to the RP:
    {"cmd":"configRead"}
    {"cmd":"configWrite", "configured":1, "ssid":"...", ...}
    {"cmd":"configClear"}

  Pre-stored (compiled per device)
  ────────────────────────────────
  DEVICE_NAME, PROVISION_TOKEN, BACKEND_URL, CA certificate.
  MAC address is read from hardware at runtime.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ═══════════════════════════════════════════════════════════════════
// Factory constants — change these before flashing each device
// ═══════════════════════════════════════════════════════════════════

static const char *DEVICE_NAME      = "device1";
static const char *PROVISION_TOKEN  = "playarka-provision-token-001";
static const char *BACKEND_URL      = "http://192.168.0.106:3001"; // your backend LAN IP

// ═══════════════════════════════════════════════════════════════════
// EMQX Root CA Certificate (DigiCert Global Root G2)
// Downloaded from EMQX Cloud dashboard and stored in src/assets/emqxsl-ca.crt
// ═══════════════════════════════════════════════════════════════════

static const char CA_CERT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----
)EOF";

// ═══════════════════════════════════════════════════════════════════
// Pin configuration
// ═══════════════════════════════════════════════════════════════════

static const int SERIAL2_RX_PIN    = 44;
static const int SERIAL2_TX_PIN    = 43;
static const int FACTORY_RESET_PIN = 0;   // BOOT button on most ESP32-S3 boards
static const uint32_t SERIAL_BAUD  = 115200;
static const uint32_t SERIAL2_BAUD = 115200;

// ═══════════════════════════════════════════════════════════════════
// Runtime config (loaded from FRAM via RP, or filled during provisioning)
// ═══════════════════════════════════════════════════════════════════

struct DeviceConfig {
  bool     configured;
  char     ssid[33];
  char     wifiPass[65];
  char     mqttHost[129];
  uint16_t mqttPort;
  char     mqttUser[65];
  char     mqttPass[65];
  char     deviceId[17];
};

static DeviceConfig cfg;
static bool configLoaded = false;

// ═══════════════════════════════════════════════════════════════════
// Global objects
// ═══════════════════════════════════════════════════════════════════

WiFiClientSecure wifiSecureClient;
PubSubClient     mqttClient(wifiSecureClient);
WebServer        webServer(80);
DNSServer        dnsServer;

static char macAddress[18];    // "AA:BB:CC:DD:EE:FF"
static char apSSID[32];        // "PlayArka-Setup-XXXX"
static char masterTopic[64];   // "playarka/device/{id}/state"

static bool     setupMode = false;
static uint32_t lastHeartbeatMs = 0;
static uint32_t HEARTBEAT_INTERVAL_MS = 10UL * 1000UL;

static char serial2Line[512];
static size_t serial2LineLen = 0;

static char currentMatchId[80] = "uuid";

// Setup-mode state machine
enum SetupPhase {
  PHASE_IDLE,              // serving captive portal, waiting for form submit
  PHASE_CONNECTING_WIFI,   // connecting to venue WiFi
  PHASE_PROVISIONING,      // calling backend /api/device/provision
  PHASE_DONE,              // success — about to switch to normal mode
  PHASE_ERROR              // failure — user can retry
};

static SetupPhase  setupPhase = PHASE_IDLE;
static uint32_t    phaseStartMs = 0;
static char        setupStatusMsg[128] = "Waiting for WiFi credentials...";

// ═══════════════════════════════════════════════════════════════════
// Captive portal HTML
// ═══════════════════════════════════════════════════════════════════

static const char SETUP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>PlayArka Device Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#e2e8f0;max-width:420px;margin:0 auto;padding:20px}
h1{color:#f97316;text-align:center;margin-bottom:20px;font-size:24px}
.card{background:#16213e;border:1px solid #334155;border-radius:10px;padding:16px;margin-bottom:16px}
.label{color:#94a3b8;font-size:13px;margin-bottom:2px}.value{color:#e2e8f0;font-weight:600}
label{display:block;color:#94a3b8;font-size:14px;margin-top:14px;margin-bottom:4px}
input,select{width:100%;padding:10px 12px;border:1px solid #334155;background:#0f172a;color:#e2e8f0;border-radius:8px;font-size:15px}
input:focus,select:focus{outline:none;border-color:#f97316}
.btn{display:block;width:100%;padding:14px;margin-top:20px;background:#f97316;color:#fff;border:none;border-radius:8px;font-size:16px;font-weight:600;cursor:pointer}
.btn:active{background:#ea580c}
.status{text-align:center;padding:12px;border-radius:8px;margin-top:16px;font-size:14px}
.ok{background:#14532d;color:#86efac}.err{background:#7f1d1d;color:#fca5a5}
.spin{display:inline-block;width:16px;height:16px;border:2px solid #f97316;border-top-color:transparent;border-radius:50%;animation:s .8s linear infinite}
@keyframes s{to{transform:rotate(360deg)}}
</style></head><body>
<h1>PlayArka Setup</h1>
<div class='card'>
  <div class='label'>Device</div><div class='value'>%DEVICE_NAME%</div>
  <div class='label' style='margin-top:8px'>MAC Address</div><div class='value' style='font-family:monospace'>%MAC%</div>
</div>
<form id='f' action='/setup' method='POST'>
  <div class='card'>
    <label for='nets'>WiFi Network</label>
    <select name='ssid' id='nets'><option value=''>Scanning...</option></select>
    <label for='sm'>Or enter SSID manually</label>
    <input id='sm' name='ssid_manual' placeholder='Type SSID here'>
    <label for='pw'>WiFi Password</label>
    <input id='pw' type='password' name='password' placeholder='Enter password' required>
  </div>
  <button class='btn' type='submit'>Connect &amp; Provision</button>
</form>
<div id='st'></div>
<script>
fetch('/scan').then(r=>r.json()).then(n=>{var s=document.getElementById('nets');s.innerHTML='<option value="">-- Select Network --</option>';n.forEach(function(w){s.innerHTML+='<option value="'+w.ssid+'">'+w.ssid+' ('+w.rssi+' dBm)</option>'})}).catch(function(){document.getElementById('nets').innerHTML='<option value="">Scan failed</option>'});
document.getElementById('f').addEventListener('submit',function(e){e.preventDefault();var fd=new FormData(this);fetch('/setup',{method:'POST',body:new URLSearchParams(fd)}).then(function(){poll()}).catch(function(){poll()})});
function poll(){var box=document.getElementById('st');box.innerHTML='<div class="status"><span class="spin"></span> Connecting...</div>';var iv=setInterval(function(){fetch('/status').then(r=>r.json()).then(d=>{box.innerHTML='<div class="status'+(d.state==='done'?' ok':d.state==='error'?' err':'')+'">'+d.message+'</div>';if(d.state==='done'||d.state==='error')clearInterval(iv)}).catch(function(){box.innerHTML='<div class="status ok">Setup complete! Device is now online.</div>';clearInterval(iv)})},2000)}
</script>
</body></html>
)rawliteral";

// ═══════════════════════════════════════════════════════════════════
// Time helpers
// ═══════════════════════════════════════════════════════════════════

static uint32_t epochNow() {
  time_t now = time(nullptr);
  return (now > 1700000000) ? (uint32_t)now : (uint32_t)(millis() / 1000UL);
}

static void setupTimeNtp() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

static bool waitForNtpSync(unsigned long timeoutMs = 15000) {
  Serial.print("🕐 Waiting for NTP time sync");
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    time_t now = time(nullptr);
    if (now > 1700000000) {
      Serial.printf("\n✅ NTP synced — epoch: %lu\n", (unsigned long)now);
      return true;
    }
    Serial.print(".");
    delay(500);
  }
  Serial.println("\n⚠️ NTP sync timeout — TLS may fail");
  return false;
}

// ═══════════════════════════════════════════════════════════════════
// Config communication with RP (Serial2 ↔ FRAM)
// ═══════════════════════════════════════════════════════════════════

static bool waitForSerial2Line(char *buf, size_t bufSize, unsigned long timeoutMs) {
  size_t pos = 0;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (Serial2.available()) {
      char c = (char)Serial2.read();
      if (c == '\r') continue;
      if (c == '\n') {
        buf[pos] = '\0';
        return (pos > 0);
      }
      if (pos < bufSize - 1) buf[pos++] = c;
    }
    delay(5);
  }
  buf[pos] = '\0';
  return false;
}

static bool requestConfigFromRP() {
  Serial2.println("{\"cmd\":\"configRead\"}");
  Serial2.flush();

  char line[512];
  if (!waitForSerial2Line(line, sizeof(line), 5000)) return false;

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) return false;

  const char *type = doc["type"] | "";
  if (strcmp(type, "config") != 0) return false;

  cfg.configured = (doc["configured"] | 0) == 1;
  strlcpy(cfg.ssid,     doc["ssid"]     | "", sizeof(cfg.ssid));
  strlcpy(cfg.wifiPass,  doc["pass"]     | "", sizeof(cfg.wifiPass));
  strlcpy(cfg.mqttHost,  doc["mqttHost"] | "", sizeof(cfg.mqttHost));
  cfg.mqttPort = doc["mqttPort"] | 8883;
  strlcpy(cfg.mqttUser,  doc["mqttUser"] | "", sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPass,  doc["mqttPass"] | "", sizeof(cfg.mqttPass));
  strlcpy(cfg.deviceId,  doc["deviceId"] | "", sizeof(cfg.deviceId));
  configLoaded = true;

  Serial.println("┌─────────── FRAM Config ───────────┐");
  Serial.printf("│ configured : %s\n", cfg.configured ? "YES" : "NO");
  Serial.printf("│ ssid       : [%s]\n", cfg.ssid);
  Serial.printf("│ wifiPass   : [%s]\n", cfg.wifiPass);
  Serial.printf("│ mqttHost   : [%s]\n", cfg.mqttHost);
  Serial.printf("│ mqttPort   : %u\n",   cfg.mqttPort);
  Serial.printf("│ mqttUser   : [%s]\n", cfg.mqttUser);
  Serial.printf("│ mqttPass   : [%s]\n", cfg.mqttPass);
  Serial.printf("│ deviceId   : [%s]\n", cfg.deviceId);
  Serial.println("└───────────────────────────────────┘");
  return true;
}

static bool sendConfigToRP() {
  StaticJsonDocument<512> doc;
  doc["cmd"]        = "configWrite";
  doc["configured"] = cfg.configured ? 1 : 0;
  doc["ssid"]       = cfg.ssid;
  doc["pass"]       = cfg.wifiPass;
  doc["mqttHost"]   = cfg.mqttHost;
  doc["mqttPort"]   = cfg.mqttPort;
  doc["mqttUser"]   = cfg.mqttUser;
  doc["mqttPass"]   = cfg.mqttPass;
  doc["deviceId"]   = cfg.deviceId;

  char out[512];
  serializeJson(doc, out, sizeof(out));
  Serial2.println(out);
  Serial2.flush();

  char line[128];
  if (!waitForSerial2Line(line, sizeof(line), 3000)) {
    Serial.println("⚠️ No ack from RP for configWrite");
    return false;
  }
  if (strstr(line, "configWriteOk")) {
    Serial.println("✅ Config saved to FRAM");
    return true;
  }
  Serial.printf("⚠️ Unexpected RP response: %s\n", line);
  return false;
}

static void clearConfigOnRP() {
  Serial2.println("{\"cmd\":\"configClear\"}");
  Serial2.flush();
  char line[128];
  waitForSerial2Line(line, sizeof(line), 3000);
  Serial.println("Config cleared on RP");
}

// ═══════════════════════════════════════════════════════════════════
// WiFi AP + Captive Portal
// ═══════════════════════════════════════════════════════════════════

static void handleRoot() {
  String html = FPSTR(SETUP_HTML);
  html.replace("%DEVICE_NAME%", DEVICE_NAME);
  html.replace("%MAC%", macAddress);
  webServer.send(200, "text/html", html);
}

static void handleScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  WiFi.scanDelete();
  webServer.send(200, "application/json", json);
}

static void handleSetup() {
  String ssid = webServer.arg("ssid");
  String ssidManual = webServer.arg("ssid_manual");
  String password = webServer.arg("password");

  // Prefer manual SSID if provided
  if (ssidManual.length() > 0) ssid = ssidManual;

  if (ssid.length() == 0) {
    webServer.send(400, "text/plain", "SSID is required");
    return;
  }

  strlcpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid));
  strlcpy(cfg.wifiPass, password.c_str(), sizeof(cfg.wifiPass));

  webServer.send(200, "text/plain", "OK");

  // Kick off the setup state machine
  setupPhase = PHASE_CONNECTING_WIFI;
  phaseStartMs = millis();
  strlcpy(setupStatusMsg, "Connecting to WiFi...", sizeof(setupStatusMsg));

  Serial.printf("Setup: connecting to SSID '%s'\n", cfg.ssid);

  // Switch to AP+STA so portal stays up while we connect
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(cfg.ssid, cfg.wifiPass);
}

static void handleStatus() {
  String state;
  switch (setupPhase) {
    case PHASE_IDLE:             state = "idle";         break;
    case PHASE_CONNECTING_WIFI:  state = "connecting";   break;
    case PHASE_PROVISIONING:     state = "provisioning"; break;
    case PHASE_DONE:             state = "done";         break;
    case PHASE_ERROR:            state = "error";        break;
  }
  String json = "{\"state\":\"" + state + "\",\"message\":\"" + String(setupStatusMsg) + "\"}";
  webServer.send(200, "application/json", json);
}

static void startSetupMode() {
  setupMode = true;
  setupPhase = PHASE_IDLE;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSSID);
  delay(100);

  // Captive portal DNS: redirect all hostnames to our IP
  dnsServer.start(53, "*", WiFi.softAPIP());

  webServer.on("/",       HTTP_GET,  handleRoot);
  webServer.on("/scan",   HTTP_GET,  handleScan);
  webServer.on("/setup",  HTTP_POST, handleSetup);
  webServer.on("/status", HTTP_GET,  handleStatus);
  webServer.onNotFound(handleRoot); // catch-all for captive portal
  webServer.begin();

  Serial.printf("📡 AP started: %s  IP: %s\n",
                apSSID, WiFi.softAPIP().toString().c_str());
}

// ═══════════════════════════════════════════════════════════════════
// Provisioning — POST /api/device/provision
// ═══════════════════════════════════════════════════════════════════

static bool provisionWithBackend() {
  WiFiClient httpClient;   // plain HTTP for backend (use WiFiClientSecure for HTTPS)
  HTTPClient http;

  String url = String(BACKEND_URL) + "/api/device/provision";
  Serial.printf("Provisioning: POST %s\n", url.c_str());

  http.begin(httpClient, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);

  StaticJsonDocument<256> reqDoc;
  reqDoc["token"]      = PROVISION_TOKEN;
  reqDoc["deviceName"] = DEVICE_NAME;
  reqDoc["mac"]        = macAddress;

  char body[256];
  serializeJson(reqDoc, body, sizeof(body));

  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("Provision failed: HTTP %d\n", code);
    http.end();
    return false;
  }

  String resp = http.getString();
  http.end();

  StaticJsonDocument<384> resDoc;
  if (deserializeJson(resDoc, resp) != DeserializationError::Ok) {
    Serial.println("Provision: bad JSON response");
    return false;
  }

  const char *status = resDoc["status"] | "";
  if (strcmp(status, "ok") != 0) {
    Serial.printf("Provision rejected: %s\n", (const char *)(resDoc["error"] | "unknown"));
    return false;
  }

  strlcpy(cfg.mqttHost, resDoc["mqttHost"] | "", sizeof(cfg.mqttHost));
  cfg.mqttPort = resDoc["mqttPort"] | 8883;
  strlcpy(cfg.mqttUser, resDoc["mqttUsername"] | "", sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPass, resDoc["mqttPassword"] | "", sizeof(cfg.mqttPass));
  strlcpy(cfg.deviceId, resDoc["deviceId"]     | "", sizeof(cfg.deviceId));
  cfg.configured = true;

  Serial.printf("✅ Provisioned! deviceId=%s mqttHost=%s\n", cfg.deviceId, cfg.mqttHost);
  return true;
}

// ═══════════════════════════════════════════════════════════════════
// MQTT
// ═══════════════════════════════════════════════════════════════════

static void sendCommandToRP(const char *cmd) {
  StaticJsonDocument<128> doc;
  doc["cmd"] = cmd;
  char out[128];
  size_t n = serializeJson(doc, out, sizeof(out));
  if (n > 0) {
    Serial2.println(out);
    Serial2.flush();
    Serial.printf("📤 Sent to RP: %s\n", out);
  }
}

static void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  static char msg[512];
  unsigned int n = (length < sizeof(msg) - 1) ? length : (sizeof(msg) - 1);
  memcpy(msg, payload, n);
  msg[n] = '\0';

  Serial.printf("📥 MQTT [%s]: %s\n", topic, msg);

  // Skip large state updates (frontend-only)
  if (strncmp(msg, "{\"action\":\"update\"", 18) == 0) return;

  StaticJsonDocument<384> doc;
  if (deserializeJson(doc, msg)) return;

  const char *action = doc["action"] | doc["type"] | "";

  if (strcmp(action, "setMatchId") == 0) {
    const char *mid = doc["matchId"] | "";
    if (mid[0]) {
      strlcpy(currentMatchId, mid, sizeof(currentMatchId));
      Serial.printf("✅ matchId = %s\n", currentMatchId);
    }
  } else if (strcmp(action, "setHeartbeatIntervalMs") == 0) {
    uint32_t ms = doc["ms"] | HEARTBEAT_INTERVAL_MS;
    if (ms < 1000) ms = 1000;
    HEARTBEAT_INTERVAL_MS = ms;
  } else if (strcmp(action, "requestHeartbeat") == 0) {
    lastHeartbeatMs = 0;
  } else if (strcmp(action, "gameStart") == 0) {
    Serial.println("🎮 gameStart → RP");
    sendCommandToRP("gameStart");
  } else if (strcmp(action, "gameOver") == 0) {
    Serial.println("🏁 gameOver → RP");
    sendCommandToRP("gameOver");
    strlcpy(currentMatchId, "uuid", sizeof(currentMatchId));
    serial2LineLen = 0;
  }
}

static void connectMqtt() {
  if (mqttClient.connected()) return;

  mqttClient.setServer(cfg.mqttHost, cfg.mqttPort);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(2048);

  wifiSecureClient.setCACert(CA_CERT);
  wifiSecureClient.setTimeout(5);  // 5-second TLS handshake timeout

  Serial.printf("🔌 Connecting MQTT to %s:%u as user [%s] ...\n",
                cfg.mqttHost, cfg.mqttPort, cfg.mqttUser);

  int retries = 0;
  while (!mqttClient.connected() && retries < 10) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("⚠️ WiFi not connected — aborting MQTT connect");
      return;
    }

    char clientId[64];
    snprintf(clientId, sizeof(clientId), "esp32s3_%s_%lu",
             cfg.deviceId, (unsigned long)esp_random());

    Serial.printf("  attempt %d/10 (clientId=%s) ... ", retries + 1, clientId);
    if (mqttClient.connect(clientId, cfg.mqttUser, cfg.mqttPass)) {
      Serial.println("OK");
      if (mqttClient.subscribe(masterTopic, 1)) {
        Serial.printf("✅ MQTT connected & subscribed: %s\n", masterTopic);
      }
      return;
    }
    Serial.printf("fail (rc=%d)\n", mqttClient.state());
    retries++;
    delay(2000);
  }
  Serial.println("❌ MQTT connect failed after 10 attempts (will retry in loop)");
}

// ═══════════════════════════════════════════════════════════════════
// Publish helpers
// ═══════════════════════════════════════════════════════════════════

static void publishHeartbeat() {
  StaticJsonDocument<256> doc;
  doc["type"]      = "HEARTBEAT";
  doc["deviceId"]  = DEVICE_NAME;
  doc["uptime"]    = (uint32_t)millis();
  doc["timestamp"] = epochNow();

  char out[256];
  if (serializeJson(doc, out, sizeof(out)) == 0) return;
  mqttClient.publish(masterTopic, out, false);
  Serial.printf("📤 HEARTBEAT → %s\n", masterTopic);
}

static const int PINS_PER_FRAME = 8;

static void publishScore(
  const char *matchId,
  const int *fallen, size_t fallenCnt,
  const int *remaining, size_t remainCnt,
  bool isStrike, bool isSpare
) {
  StaticJsonDocument<512> doc;
  doc["type"]    = "SCORE";
  doc["matchId"] = matchId;

  JsonArray fa = doc.createNestedArray("fallenPins");
  for (size_t i = 0; i < fallenCnt; i++) fa.add(fallen[i]);

  doc["isStrike"] = isStrike;
  doc["isSpare"]  = isSpare;

  JsonArray ra = doc.createNestedArray("remainingPins");
  for (size_t i = 0; i < remainCnt; i++) ra.add(remaining[i]);

  doc["timestamp"] = epochNow();

  char out[512];
  if (serializeJson(doc, out, sizeof(out)) == 0) return;
  mqttClient.publish(masterTopic, out, false);
  Serial.printf("📤 SCORE → %s\n", masterTopic);
}

static void publishPinError(int pin) {
  StaticJsonDocument<256> doc;
  doc["type"]      = "PIN_ERROR";
  doc["pin"]       = pin;
  doc["deviceId"]  = cfg.deviceId;
  doc["timestamp"] = epochNow();

  char out[256];
  if (serializeJson(doc, out, sizeof(out)) == 0) return;
  mqttClient.publish(masterTopic, out, false);
  Serial.printf("🚨 PIN_ERROR (pin %d) → %s\n", pin, masterTopic);
}

// ═══════════════════════════════════════════════════════════════════
// Serial2 — data from RP board (ball detection + pin errors)
// ═══════════════════════════════════════════════════════════════════

static void handleRPData(const char *line) {
  if (!line) return;
  while (*line == ' ' || *line == '\t') line++;
  if (*line == '\0') return;

  Serial.printf("📥 RP data: %s\n", line);

  if (line[0] != '{') return;

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, line)) return;

  const char *type = doc["type"] | "";

  if (strcmp(type, "ballDetected") == 0) {
    JsonArray fallenArr = doc["fallenPins"].as<JsonArray>();
    JsonArray remainArr = doc["remainingPins"].as<JsonArray>();
    if (fallenArr.isNull() || remainArr.isNull()) return;

    int fallen[PINS_PER_FRAME], remaining[PINS_PER_FRAME];
    size_t fc = 0, rc = 0;
    for (JsonVariant v : fallenArr)  { if (fc < PINS_PER_FRAME) fallen[fc++] = v.as<int>(); }
    for (JsonVariant v : remainArr)  { if (rc < PINS_PER_FRAME) remaining[rc++] = v.as<int>(); }

    bool strike = doc["isStrike"] | false;
    bool spare  = doc["isSpare"]  | false;

    Serial.println("🎳 Ball detected → publishing score");
    publishScore(currentMatchId, fallen, fc, remaining, rc, strike, spare);
  } else if (strcmp(type, "pinError") == 0) {
    int pin = doc["pin"] | -1;
    Serial.printf("🚨 Pin error from RP: pin %d stuck after retries\n", pin);
    publishPinError(pin);
  }
  // config responses are handled in requestConfigFromRP / sendConfigToRP
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
    if (serial2LineLen < sizeof(serial2Line) - 1) {
      serial2Line[serial2LineLen++] = c;
    } else {
      serial2LineLen = 0;
    }
  }
}

// ═══════════════════════════════════════════════════════════════════
// Factory reset check (hold BOOT button at power-on)
// ═══════════════════════════════════════════════════════════════════

static void checkFactoryReset() {
  pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);
  if (digitalRead(FACTORY_RESET_PIN) != LOW) return;

  Serial.println("⚠️ BOOT button held — hold 3 s for factory reset...");
  unsigned long start = millis();
  while (digitalRead(FACTORY_RESET_PIN) == LOW) {
    if (millis() - start >= 3000) {
      Serial.println("🔄 Factory reset triggered!");
      clearConfigOnRP();
      Serial.println("Rebooting...");
      delay(500);
      ESP.restart();
    }
  }
  Serial.println("Button released — normal boot.");
}

// ═══════════════════════════════════════════════════════════════════
// Normal-mode WiFi reconnect
// ═══════════════════════════════════════════════════════════════════

static void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.wifiPass);

  Serial.print("📶 Reconnecting WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\n❌ WiFi reconnect timeout — will retry next loop");
      return;
    }
  }
  Serial.printf("\n✅ WiFi reconnected  IP: %s\n", WiFi.localIP().toString().c_str());
}

// ═══════════════════════════════════════════════════════════════════
// Enter normal mode (WiFi + MQTT already ready)
// ═══════════════════════════════════════════════════════════════════

static void enterNormalMode() {
  snprintf(masterTopic, sizeof(masterTopic), "playarka/device/%s/state", cfg.deviceId);

  Serial.println("─── Entering Normal Mode ───");
  Serial.printf("  deviceId  : %s\n", cfg.deviceId);
  Serial.printf("  mqttHost  : %s\n", cfg.mqttHost);
  Serial.printf("  mqttPort  : %u\n", cfg.mqttPort);
  Serial.printf("  mqttUser  : %s\n", cfg.mqttUser);
  Serial.printf("  topic     : %s\n", masterTopic);
  Serial.printf("  WiFi IP   : %s\n", WiFi.localIP().toString().c_str());

  setupTimeNtp();
  waitForNtpSync(15000);  // TLS cert validation needs correct time

  connectMqtt();

  if (mqttClient.connected()) {
    publishHeartbeat();
    lastHeartbeatMs = millis();
    Serial.println("🟢 Normal mode — MQTT online");
  } else {
    lastHeartbeatMs = millis();
    Serial.println("🟡 Normal mode — MQTT will retry in loop");
  }
}

// ═══════════════════════════════════════════════════════════════════
// setup()
// ═══════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial2.begin(SERIAL2_BAUD, SERIAL_8N1, SERIAL2_RX_PIN, SERIAL2_TX_PIN);
  delay(200);

  // Read MAC address
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(macAddress, sizeof(macAddress), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(apSSID, sizeof(apSSID), "PlayArka-Setup-%02X%02X", mac[4], mac[5]);

  Serial.println("\n══════════════════════════════════════");
  Serial.println("  PlayArka Master (ESP32-S3)");
  Serial.printf("  Device : %s\n", DEVICE_NAME);
  Serial.printf("  MAC    : %s\n", macAddress);
  Serial.printf("  Backend: %s\n", BACKEND_URL);
  Serial.println("══════════════════════════════════════");

  // Factory reset check
  checkFactoryReset();

  // Wait for RP board to boot and initialize FRAM
  Serial.println("⏳ Waiting for RP board...");
  delay(3000);

  // Request config from RP (FRAM)
  Serial.println("📖 Reading config from FRAM...");
  bool gotConfig = false;
  for (int attempt = 0; attempt < 3 && !gotConfig; attempt++) {
    gotConfig = requestConfigFromRP();
    if (!gotConfig) {
      Serial.printf("  Attempt %d failed, retrying...\n", attempt + 1);
      delay(1000);
    }
  }

  if (!gotConfig) {
    Serial.println("⚠️ Could not read FRAM — treating as unconfigured");
    memset(&cfg, 0, sizeof(cfg));
    configLoaded = true;
  }

  // ── Configured: connect WiFi + MQTT ──
  if (cfg.configured) {
    Serial.printf("✅ Configured — connecting to '%s'\n", cfg.ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid, cfg.wifiPass);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
      delay(300);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\n✅ WiFi OK  IP: %s\n", WiFi.localIP().toString().c_str());
      enterNormalMode();
      return;  // normal operation in loop()
    }

    Serial.println("\n❌ WiFi failed — falling back to setup mode");
    cfg.configured = false;
  }

  // ── Not configured (or WiFi failed): enter setup mode ──
  Serial.println("📡 Entering setup mode...");
  startSetupMode();
}

// ═══════════════════════════════════════════════════════════════════
// loop()
// ═══════════════════════════════════════════════════════════════════

void loop() {

  // ── Setup mode ──
  if (setupMode) {
    dnsServer.processNextRequest();
    webServer.handleClient();

    switch (setupPhase) {
      case PHASE_IDLE:
        // waiting for user to submit the form
        break;

      case PHASE_CONNECTING_WIFI:
        if (WiFi.status() == WL_CONNECTED) {
          Serial.printf("✅ WiFi connected  IP: %s\n", WiFi.localIP().toString().c_str());
          strlcpy(setupStatusMsg, "WiFi connected! Provisioning with backend...", sizeof(setupStatusMsg));
          setupPhase = PHASE_PROVISIONING;
          phaseStartMs = millis();
        } else if (millis() - phaseStartMs > 20000) {
          Serial.println("❌ WiFi connection timeout");
          strlcpy(setupStatusMsg, "WiFi connection failed. <a href='/'>Try again</a>", sizeof(setupStatusMsg));
          setupPhase = PHASE_ERROR;
          WiFi.disconnect();
          WiFi.mode(WIFI_AP);
          WiFi.softAP(apSSID);
        }
        break;

      case PHASE_PROVISIONING:
        if (provisionWithBackend()) {
          // Save everything to FRAM via RP
          sendConfigToRP();

          snprintf(setupStatusMsg, sizeof(setupStatusMsg),
                   "Success! Assigned Lane %s. Device going online...", cfg.deviceId);
          setupPhase = PHASE_DONE;
          phaseStartMs = millis();
        } else {
          strlcpy(setupStatusMsg,
                  "Provisioning failed. Check backend. <a href='/'>Try again</a>",
                  sizeof(setupStatusMsg));
          setupPhase = PHASE_ERROR;
          WiFi.disconnect();
          WiFi.mode(WIFI_AP);
          WiFi.softAP(apSSID);
        }
        break;

      case PHASE_DONE:
        if (millis() - phaseStartMs > 5000) {
          Serial.println("🔄 Leaving setup mode → normal mode");
          webServer.stop();
          dnsServer.stop();

          WiFi.disconnect(true);
          delay(200);
          WiFi.mode(WIFI_STA);
          WiFi.begin(cfg.ssid, cfg.wifiPass);

          Serial.print("📶 Reconnecting WiFi (STA)");
          unsigned long s = millis();
          while (WiFi.status() != WL_CONNECTED && millis() - s < 20000) {
            Serial.print(".");
            delay(300);
          }

          if (WiFi.status() != WL_CONNECTED) {
            Serial.println("\n❌ WiFi reconnect failed — retrying...");
            WiFi.disconnect();
            WiFi.begin(cfg.ssid, cfg.wifiPass);
            unsigned long s2 = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - s2 < 15000) delay(300);
          }

          if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\n✅ WiFi OK  IP: %s\n", WiFi.localIP().toString().c_str());
          } else {
            Serial.println("\n⚠️ WiFi still not connected — will keep retrying in loop");
          }

          setupMode = false;
          enterNormalMode();
        }
        break;

      case PHASE_ERROR:
        // Portal is back up — user can retry. Reset to IDLE after a moment.
        if (millis() - phaseStartMs > 1000) {
          setupPhase = PHASE_IDLE;
        }
        break;
    }
    return;  // don't run normal-mode code while in setup
  }

  // ── Normal mode ──
  ensureWiFi();

  if (!mqttClient.connected()) {
    connectMqtt();
  }
  mqttClient.loop();

  pollSerial2();

  uint32_t nowMs = millis();
  if ((uint32_t)(nowMs - lastHeartbeatMs) >= HEARTBEAT_INTERVAL_MS) {
    publishHeartbeat();
    lastHeartbeatMs = nowMs;
  }
}
