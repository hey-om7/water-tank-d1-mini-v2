/*
 * dashboard_server.h — HTTP Server & API Routes
 * ================================================
 * Lightweight web server for the dashboard UI, sensor data APIs,
 * config management, history logging, and captive portal for AP mode.
 */

#ifndef DASHBOARD_SERVER_H
#define DASHBOARD_SERVER_H

#include "config_logic.h"
#include "dashboard.h"
#include "globals.h"
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>

// ─── Add History Entry ───────────────────────────────────────
inline void addHistoryEntry() {
  historyBuf[historyHead].timestamp = millis() / 1000;
  historyBuf[historyHead].level = currentLevel;
  historyHead = (historyHead + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE)
    historyCount++;
}

// ─── Log to Google Sheets ────────────────────────────────────
inline void logToGoogleSheets() {
  if (strlen(config.webhook) < 10)
    return; // No webhook configured
  if (WiFi.status() != WL_CONNECTED)
    return;

  Serial.println("[SERVER] Logging to Google Sheets...");

  std::unique_ptr<BearSSL::WiFiClientSecure> client(
      new BearSSL::WiFiClientSecure);
  client->setInsecure(); // Google Apps Script uses valid SSL

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
    Serial.printf("[SERVER] Sheets response: %d\n", code);

    // Handle redirect (Google Apps Script redirects)
    if (code == HTTP_CODE_MOVED_PERMANENTLY || code == HTTP_CODE_FOUND) {
      String redirectUrl = http.getLocation();
      http.end();
      if (http.begin(*client, redirectUrl)) {
        http.addHeader("Content-Type", "application/json");
        code = http.POST(payload);
        Serial.printf("[SERVER] Sheets redirect response: %d\n", code);
      }
    }
    http.end();
  }
}

// ─── Setup Dashboard Routes (Normal Mode) ────────────────────
inline void setupRoutes() {
  // Serve gzipped dashboard
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char *)DASHBOARD_GZ,
                  DASHBOARD_GZ_LEN);
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
    String json = "{\"data\":[";
    int start = (historyCount < HISTORY_SIZE) ? 0 : historyHead;
    for (int i = 0; i < historyCount; i++) {
      int idx = (start + i) % HISTORY_SIZE;
      if (i > 0)
        json += ",";
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
      server.send(400, "application/json",
                  "{\"ok\":false,\"error\":\"No body\"}");
      return;
    }

    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
      server.send(400, "application/json",
                  "{\"ok\":false,\"error\":\"Bad JSON\"}");
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
    if (doc.containsKey("tankMin"))
      config.tankEmpty = doc["tankMin"];
    if (doc.containsKey("tankMax"))
      config.tankFull = doc["tankMax"];

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
    server.send(200, "application/json",
                "{\"ok\":true,\"msg\":\"Resetting...\"}");
    delay(500);
    factoryReset();
  });

  // 404 handler — serve dashboard for any unknown route
  server.onNotFound([]() {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char *)DASHBOARD_GZ,
                  DASHBOARD_GZ_LEN);
  });

  Serial.println("[SERVER] Dashboard routes configured");
}

// ─── Setup Captive Portal Routes (AP Mode) ───────────────────
inline void setupCaptivePortalRoutes() {
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
    if (config.tankEmpty == 0)
      config.tankEmpty = 200;
    if (config.tankFull == 0)
      config.tankFull = 20;
    if (strlen(config.name) == 0)
      strncpy(config.name, "WaterTank", sizeof(config.name) - 1);

    saveConfigToEEPROM();
    server.send(200, "application/json", "{\"ok\":true}");

    delay(1000);
    ESP.restart();
  });

  // Captive portal: redirect all unknown requests to root
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });

  Serial.println("[SERVER] Captive portal routes configured");
}

#endif // DASHBOARD_SERVER_H
