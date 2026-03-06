/*
 * Water Tank Monitoring System
 * =============================
 * D1 Mini V2 (ESP8266) + JSN-SR04T Ultrasonic Sensor
 *
 * Features:
 *  - WiFi auto-connect with AP fallback & captive portal
 *  - mDNS (watertank.local)
 *  - Real-time water level monitoring
 *  - Fill detection with rate & ETA calculation
 *  - Google Sheets logging (every 5 min)
 *  - Modern web dashboard
 *  - EEPROM configuration persistence
 *  - Factory reset
 *
 * Wiring:
 *  JSN-SR04T Trigger → D6 (GPIO12)
 *  JSN-SR04T Echo    → D7 (GPIO13)
 *  VCC               → 5V
 *  GND               → GND
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include "dashboard.h"

// ─── Pin Definitions ──────────────────────────────────────────
#define TRIGGER_PIN D6  // GPIO12
#define ECHO_PIN    D7  // GPIO13

// ─── Constants ───────────────────────────────────────────────
#define EEPROM_SIZE         512
#define EEPROM_MAGIC        0xA5
#define WIFI_CONNECT_TIMEOUT 15000  // 15 seconds
#define AP_SSID             "WaterTank-Setup"
#define AP_PASS             ""      // Open network for easy setup
#define MDNS_NAME           "watertank"
#define SENSOR_SAMPLES      5       // Number of readings to average
#define SENSOR_INTERVAL     2000    // Read sensor every 2s
#define LOG_INTERVAL        300000  // Log to Google Sheets every 5 min
#define HISTORY_SIZE        288     // 24h at 5-min intervals
#define FILL_WINDOW         24      // 2 min window (24 * 5s readings)
#define FILL_THRESHOLD      3.0     // >3% rise in window = filling

// ─── EEPROM Layout ───────────────────────────────────────────
// Byte 0:   Magic byte (0xA5 = configured)
// Byte 1-32:  WiFi SSID (32 bytes)
// Byte 33-96: WiFi Password (64 bytes)
// Byte 97-98: Tank empty distance (cm) [uint16]
// Byte 99-100: Tank full distance (cm) [uint16]
// Byte 101-356: Webhook URL (256 bytes)
// Byte 357-388: Device name (32 bytes)
struct Config {
  char     ssid[33];
  char     pass[65];
  uint16_t tankEmpty;    // Distance when tank is empty (sensor to bottom)
  uint16_t tankFull;     // Distance when tank is full (sensor to water surface)
  char     webhook[257];
  char     name[33];
};

// ─── Global State ────────────────────────────────────────────
ESP8266WebServer server(80);
Config config;
bool apMode = false;

// Sensor
float currentDistance = 0;
float currentLevel = 0;       // 0-100%
unsigned long lastSensorRead = 0;

// Fill detection
float levelHistory[FILL_WINDOW];
int levelHistIdx = 0;
bool isFilling = false;
float fillRate = 0;            // %/min
float fillEta = 0;             // minutes to full
unsigned long lastFillCheck = 0;

// History buffer (circular)
struct HistoryEntry {
  uint32_t timestamp;
  float    level;
};
HistoryEntry historyBuf[HISTORY_SIZE];
int historyHead = 0;
int historyCount = 0;

// Logging
unsigned long lastLogTime = 0;

// ─── Function Declarations ────────────────────────────────────
void loadConfig();
void saveConfigToEEPROM();
void factoryReset();
bool connectWiFi();
void startAP();
void setupRoutes();
void setupCaptivePortalRoutes();
float readDistance();
float calculateLevel(float distance);
void updateFillDetection();
void logToGoogleSheets();
void addHistoryEntry();

// ══════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n=== Water Tank Monitor ===");

  // Sensor pins
  pinMode(TRIGGER_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIGGER_PIN, LOW);

  // Initialize fill detection history
  for (int i = 0; i < FILL_WINDOW; i++) levelHistory[i] = -1;

  // Load config from EEPROM
  EEPROM.begin(EEPROM_SIZE);
  loadConfig();

  // Attempt WiFi connection
  if (strlen(config.ssid) > 0 && connectWiFi()) {
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());

    // Start mDNS
    if (MDNS.begin(MDNS_NAME)) {
      Serial.println("mDNS: http://watertank.local");
    }

    // Setup normal dashboard routes
    setupRoutes();
  } else {
    // No saved WiFi or connection failed → AP mode
    Serial.println("Starting Access Point...");
    startAP();
    setupCaptivePortalRoutes();
  }

  server.begin();
  Serial.println("Web server started");
}

// ══════════════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════════════
void loop() {
  server.handleClient();
  if (!apMode) MDNS.update();

  unsigned long now = millis();

  // Read sensor periodically
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    currentDistance = readDistance();
    currentLevel = calculateLevel(currentDistance);
    Serial.printf("Distance: %.1f cm | Level: %.1f%%\n", currentDistance, currentLevel);
    updateFillDetection();
  }

  // Log to Google Sheets & add history entry every 5 min
  if (!apMode && now - lastLogTime >= LOG_INTERVAL) {
    lastLogTime = now;
    addHistoryEntry();
    logToGoogleSheets();
  }
}

// ══════════════════════════════════════════════════════════════
//  CONFIG (EEPROM)
// ══════════════════════════════════════════════════════════════
void loadConfig() {
  memset(&config, 0, sizeof(config));

  if (EEPROM.read(0) != EEPROM_MAGIC) {
    Serial.println("No saved config found");
    config.tankEmpty = 200;  // Default: 200cm when empty
    config.tankFull = 20;    // Default: 20cm when full
    strncpy(config.name, "WaterTank", sizeof(config.name) - 1);
    return;
  }

  // Read SSID
  for (int i = 0; i < 32; i++) config.ssid[i] = EEPROM.read(1 + i);
  config.ssid[32] = '\0';

  // Read password
  for (int i = 0; i < 64; i++) config.pass[i] = EEPROM.read(33 + i);
  config.pass[64] = '\0';

  // Read tank parameters
  config.tankEmpty = EEPROM.read(97) | (EEPROM.read(98) << 8);
  config.tankFull = EEPROM.read(99) | (EEPROM.read(100) << 8);

  // Read webhook URL
  for (int i = 0; i < 256; i++) config.webhook[i] = EEPROM.read(101 + i);
  config.webhook[256] = '\0';

  // Read device name
  for (int i = 0; i < 32; i++) config.name[i] = EEPROM.read(357 + i);
  config.name[32] = '\0';

  // Validate
  if (config.tankEmpty == 0) config.tankEmpty = 200;
  if (config.tankFull == 0) config.tankFull = 20;
  if (strlen(config.name) == 0) strncpy(config.name, "WaterTank", sizeof(config.name) - 1);

  Serial.printf("Config loaded: SSID=%s, Empty=%dcm, Full=%dcm\n",
                config.ssid, config.tankEmpty, config.tankFull);
}

void saveConfigToEEPROM() {
  EEPROM.write(0, EEPROM_MAGIC);

  for (int i = 0; i < 32; i++) EEPROM.write(1 + i, config.ssid[i]);
  for (int i = 0; i < 64; i++) EEPROM.write(33 + i, config.pass[i]);

  EEPROM.write(97, config.tankEmpty & 0xFF);
  EEPROM.write(98, (config.tankEmpty >> 8) & 0xFF);
  EEPROM.write(99, config.tankFull & 0xFF);
  EEPROM.write(100, (config.tankFull >> 8) & 0xFF);

  for (int i = 0; i < 256; i++) EEPROM.write(101 + i, config.webhook[i]);
  for (int i = 0; i < 32; i++) EEPROM.write(357 + i, config.name[i]);

  EEPROM.commit();
  Serial.println("Config saved to EEPROM");
}

void factoryReset() {
  Serial.println("=== FACTORY RESET ===");
  for (int i = 0; i < EEPROM_SIZE; i++) EEPROM.write(i, 0);
  EEPROM.commit();
  delay(500);
  ESP.restart();
}

// ══════════════════════════════════════════════════════════════
//  WIFI
// ══════════════════════════════════════════════════════════════
bool connectWiFi() {
  Serial.printf("Connecting to WiFi: %s", config.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid, config.pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT) {
      Serial.println(" FAILED");
      return false;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println(" OK");
  return true;
}

void startAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

// ══════════════════════════════════════════════════════════════
//  SENSOR
// ══════════════════════════════════════════════════════════════
float readDistance() {
  float total = 0;
  int valid = 0;

  for (int i = 0; i < SENSOR_SAMPLES; i++) {
    // Trigger pulse
    digitalWrite(TRIGGER_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIGGER_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIGGER_PIN, LOW);

    // Read echo
    long duration = pulseIn(ECHO_PIN, HIGH, 50000); // 50ms timeout (~8.5m)

    if (duration > 0) {
      float dist = (duration * 0.0343) / 2.0;  // Speed of sound = 343 m/s
      if (dist > 2 && dist < 500) {  // Valid range: 2-500cm
        total += dist;
        valid++;
      }
    }
    delay(30);  // JSN-SR04T needs ~30ms between readings
  }

  return valid > 0 ? total / valid : currentDistance;  // Keep last reading if no valid
}

float calculateLevel(float distance) {
  if (config.tankEmpty <= config.tankFull) return 0;

  float level = 100.0 * (config.tankEmpty - distance) /
                (config.tankEmpty - config.tankFull);

  // Clamp 0-100
  if (level < 0) level = 0;
  if (level > 100) level = 100;
  return level;
}

// ══════════════════════════════════════════════════════════════
//  FILL DETECTION
// ══════════════════════════════════════════════════════════════
void updateFillDetection() {
  // Store in circular buffer
  levelHistory[levelHistIdx] = currentLevel;
  levelHistIdx = (levelHistIdx + 1) % FILL_WINDOW;

  // Need at least half the window filled
  int filled = 0;
  for (int i = 0; i < FILL_WINDOW; i++) {
    if (levelHistory[i] >= 0) filled++;
  }
  if (filled < FILL_WINDOW / 2) return;

  // Find oldest valid entry
  int oldest = levelHistIdx;  // Points to oldest entry in circular buffer
  float oldestLevel = levelHistory[oldest];
  if (oldestLevel < 0) return;

  float diff = currentLevel - oldestLevel;
  float windowMinutes = (FILL_WINDOW * SENSOR_INTERVAL) / 60000.0;

  if (diff > FILL_THRESHOLD) {
    isFilling = true;
    fillRate = diff / windowMinutes;  // %/min
    if (fillRate > 0.01) {
      fillEta = (100.0 - currentLevel) / fillRate;
    } else {
      fillEta = 0;
    }
  } else {
    isFilling = false;
    fillRate = 0;
    fillEta = 0;
  }
}

// ══════════════════════════════════════════════════════════════
//  HISTORY
// ══════════════════════════════════════════════════════════════
void addHistoryEntry() {
  historyBuf[historyHead].timestamp = millis() / 1000;
  historyBuf[historyHead].level = currentLevel;
  historyHead = (historyHead + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) historyCount++;
}

// ══════════════════════════════════════════════════════════════
//  GOOGLE SHEETS LOGGING
// ══════════════════════════════════════════════════════════════
void logToGoogleSheets() {
  if (strlen(config.webhook) < 10) return;  // No webhook configured
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("Logging to Google Sheets...");

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();  // Google Apps Script uses valid SSL

  HTTPClient http;
  if (http.begin(*client, config.webhook)) {
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> doc;
    doc["level"] = round(currentLevel * 10) / 10.0;
    doc["distance"] = round(currentDistance * 10) / 10.0;
    doc["filling"] = isFilling;
    doc["fillRate"] = round(fillRate * 100) / 100.0;
    doc["deviceName"] = config.name;

    String payload;
    serializeJson(doc, payload);

    int code = http.POST(payload);
    Serial.printf("Sheets response: %d\n", code);

    // Handle redirect (Google Apps Script redirects)
    if (code == HTTP_CODE_MOVED_PERMANENTLY || code == HTTP_CODE_FOUND) {
      String redirectUrl = http.getLocation();
      http.end();
      if (http.begin(*client, redirectUrl)) {
        http.addHeader("Content-Type", "application/json");
        code = http.POST(payload);
        Serial.printf("Sheets redirect response: %d\n", code);
      }
    }
    http.end();
  }
}

// ══════════════════════════════════════════════════════════════
//  WEB SERVER ROUTES (Dashboard Mode)
// ══════════════════════════════════════════════════════════════
void setupRoutes() {
  // Serve gzipped dashboard
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char*)DASHBOARD_GZ, DASHBOARD_GZ_LEN);
  });

  // API: Current status
  server.on("/api/status", HTTP_GET, []() {
    StaticJsonDocument<256> doc;
    doc["level"] = round(currentLevel * 10) / 10.0;
    doc["distance"] = round(currentDistance * 10) / 10.0;
    doc["filling"] = isFilling;
    doc["fillRate"] = round(fillRate * 100) / 100.0;
    doc["eta"] = round(fillEta * 10) / 10.0;
    doc["uptime"] = millis() / 1000;
    doc["rssi"] = WiFi.RSSI();
    doc["name"] = config.name;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // API: History
  server.on("/api/history", HTTP_GET, []() {
    // Build JSON array of history entries
    String json = "{\"data\":[";
    int start = (historyCount < HISTORY_SIZE) ? 0 : historyHead;
    for (int i = 0; i < historyCount; i++) {
      int idx = (start + i) % HISTORY_SIZE;
      if (i > 0) json += ",";
      json += "{\"t\":";
      json += historyBuf[idx].timestamp;
      json += ",\"l\":";
      json += String(historyBuf[idx].level, 1);
      json += "}";
    }
    json += "]}";
    server.send(200, "application/json", json);
  });

  // API: Get config
  server.on("/api/config", HTTP_GET, []() {
    StaticJsonDocument<512> doc;
    doc["ssid"] = config.ssid;
    doc["tankMin"] = config.tankEmpty;
    doc["tankMax"] = config.tankFull;
    doc["webhook"] = config.webhook;
    doc["name"] = config.name;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // API: Save config
  server.on("/api/config", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"No body\"}");
      return;
    }

    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"Bad JSON\"}");
      return;
    }

    // Update WiFi if provided
    if (doc.containsKey("ssid") && strlen(doc["ssid"]) > 0) {
      strncpy(config.ssid, doc["ssid"], sizeof(config.ssid) - 1);
    }
    if (doc.containsKey("pass") && strlen(doc["pass"]) > 0) {
      strncpy(config.pass, doc["pass"], sizeof(config.pass) - 1);
    }

    // Update tank params
    if (doc.containsKey("tankMin")) config.tankEmpty = doc["tankMin"];
    if (doc.containsKey("tankMax")) config.tankFull = doc["tankMax"];

    // Update webhook
    if (doc.containsKey("webhook")) {
      strncpy(config.webhook, doc["webhook"], sizeof(config.webhook) - 1);
    }

    // Update name
    if (doc.containsKey("name")) {
      strncpy(config.name, doc["name"], sizeof(config.name) - 1);
    }

    saveConfigToEEPROM();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // API: Factory reset
  server.on("/api/reset", HTTP_POST, []() {
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Resetting...\"}");
    delay(500);
    factoryReset();
  });

  // 404 handler
  server.onNotFound([]() {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char*)DASHBOARD_GZ, DASHBOARD_GZ_LEN);
  });
}

// ══════════════════════════════════════════════════════════════
//  WEB SERVER ROUTES (Captive Portal / AP Mode)
// ══════════════════════════════════════════════════════════════
void setupCaptivePortalRoutes() {
  // Captive portal HTML
  server.on("/", HTTP_GET, []() {
    String html = R"rawliteral(
<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Water Tank Setup</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:linear-gradient(135deg,#0a0e1a,#1a1a3e);min-height:100vh;display:flex;align-items:center;justify-content:center;color:#e2e8f0}
.card{background:rgba(30,42,58,.95);border:1px solid rgba(42,58,92,.5);border-radius:20px;padding:36px;max-width:380px;width:90%;backdrop-filter:blur(10px);box-shadow:0 20px 60px rgba(0,0,0,.5)}
h1{font-size:1.4rem;text-align:center;margin-bottom:4px;background:linear-gradient(135deg,#06b6d4,#3b82f6);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.sub{text-align:center;color:#64748b;font-size:.8rem;margin-bottom:24px}
label{display:block;font-size:.75rem;font-weight:600;color:#94a3b8;margin-bottom:6px;text-transform:uppercase;letter-spacing:.5px}
input{width:100%;padding:12px 16px;background:#111827;border:1px solid #2a3a5c;border-radius:10px;color:#e2e8f0;font-size:.95rem;margin-bottom:16px;transition:border .2s}
input:focus{outline:none;border-color:#3b82f6;box-shadow:0 0 0 3px rgba(59,130,246,.15)}
button{width:100%;padding:14px;background:linear-gradient(135deg,#3b82f6,#06b6d4);color:#fff;border:none;border-radius:10px;font-size:1rem;font-weight:600;cursor:pointer;transition:transform .2s,box-shadow .2s}
button:hover{transform:translateY(-2px);box-shadow:0 8px 25px rgba(59,130,246,.3)}
.emoji{font-size:2.5rem;text-align:center;margin-bottom:12px}
.msg{text-align:center;padding:12px;border-radius:8px;margin-top:12px;font-size:.85rem;display:none}
.msg.ok{display:block;background:rgba(16,185,129,.1);border:1px solid rgba(16,185,129,.3);color:#10b981}
.msg.err{display:block;background:rgba(239,68,68,.1);border:1px solid rgba(239,68,68,.3);color:#ef4444}
</style></head><body>
<div class="card">
<div class="emoji">💧</div>
<h1>Water Tank Setup</h1>
<p class="sub">Connect your water tank monitor to WiFi</p>
<form id="wf" onsubmit="return submitWifi(event)">
<label>WiFi Network (SSID)</label>
<input type="text" id="ssid" placeholder="Enter your WiFi name" required>
<label>Password</label>
<input type="password" id="pass" placeholder="Enter WiFi password">
<button type="submit">Connect</button>
</form>
<div class="msg" id="msg"></div>
</div>
<script>
async function submitWifi(e){
e.preventDefault();
const m=document.getElementById('msg');
m.className='msg';m.style.display='none';
try{
const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({ssid:document.getElementById('ssid').value,pass:document.getElementById('pass').value})});
const d=await r.json();
if(d.ok){m.className='msg ok';m.textContent='Saved! Device will restart and connect to your WiFi.';m.style.display='block';}
else{m.className='msg err';m.textContent='Error saving. Try again.';m.style.display='block';}
}catch(err){m.className='msg err';m.textContent='Connection error.';m.style.display='block';}
}
</script></body></html>
)rawliteral";
    server.send(200, "text/html", html);
  });

  // API: Save WiFi credentials and restart
  server.on("/api/wifi", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"ok\":false}");
      return;
    }

    StaticJsonDocument<256> doc;
    deserializeJson(doc, server.arg("plain"));

    if (doc.containsKey("ssid")) {
      strncpy(config.ssid, doc["ssid"], sizeof(config.ssid) - 1);
    }
    if (doc.containsKey("pass")) {
      strncpy(config.pass, doc["pass"], sizeof(config.pass) - 1);
    }

    // Set defaults
    if (config.tankEmpty == 0) config.tankEmpty = 200;
    if (config.tankFull == 0) config.tankFull = 20;
    if (strlen(config.name) == 0) strncpy(config.name, "WaterTank", sizeof(config.name) - 1);

    saveConfigToEEPROM();
    server.send(200, "application/json", "{\"ok\":true}");

    delay(1000);
    ESP.restart();
  });

  // Captive portal: redirect all unknown requests to root
  server.onNotFound([]() {
    server.sendHeader("Location", "http://0.0.0.0/", true);
    server.send(302, "text/plain", "");
  });
}
