/*
 * dashboard_server.h — Minimal HTTP Server
 * ==========================================
 * Simple web page showing water level, auto-refreshes every 30s.
 * Routes: /  /config (settings form)  /api/config  /api/status
 *         /api/restart  + captive portal for AP mode.
 * No ArduinoJson, no history, no heavy CSS.
 */

#ifndef DASHBOARD_SERVER_H
#define DASHBOARD_SERVER_H

#include "config_logic.h"
#include "custom_mqtt_logging.h"
#include "globals.h"
#include "sensor_logic.h"

// ─── Serve Simple Dashboard HTML ─────────────────────────────
inline void serveDashboard() {
  float level = currentLevel;
  float dist = currentDistance;
  unsigned long upSec = millis() / 1000;

  // Pick background color based on level
  const char *bgColor;
  const char *statusText;
  if (level >= 75) {
    bgColor = "#0d7377";  // Teal-green — good
    statusText = "GOOD";
  } else if (level >= 40) {
    bgColor = "#1a5276"; // Blue — moderate
    statusText = "MODERATE";
  } else if (level >= 15) {
    bgColor = "#7d6608"; // Dark yellow — low
    statusText = "LOW";
  } else {
    bgColor = "#78281f"; // Dark red — critical
    statusText = "CRITICAL";
  }

  // Format uptime
  char uptimeStr[32];
  if (upSec < 3600) {
    snprintf(uptimeStr, sizeof(uptimeStr), "%lum %lus", upSec / 60, upSec % 60);
  } else if (upSec < 86400) {
    snprintf(uptimeStr, sizeof(uptimeStr), "%luh %lum", upSec / 3600, (upSec % 3600) / 60);
  } else {
    snprintf(uptimeStr, sizeof(uptimeStr), "%lud %luh", upSec / 86400, (upSec % 86400) / 3600);
  }

  // Build HTML directly — small, no external deps
  String html;
  html.reserve(800);
  html += F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<meta http-equiv='refresh' content='30'>"
            "<title>Water Tank</title></head>"
            "<body style='margin:0;min-height:100vh;display:flex;align-items:center;"
            "justify-content:center;font-family:system-ui,sans-serif;background:");
  html += bgColor;
  html += F(";color:#fff;text-align:center'>"
            "<div><h2 style='margin:0 0 8px;font-size:1rem;opacity:.7'>");
  html += config.name;
  html += F("</h2><div style='font-size:4.5rem;font-weight:900;line-height:1'>");
  html += String((int)level);
  html += F("%</div><div style='font-size:1rem;margin:8px 0;opacity:.8'>");
  html += statusText;
  html += F("</div><div style='font-size:.8rem;opacity:.6;margin-top:16px'>");
  html += String(dist, 1);
  html += F(" cm &bull; Up: ");
  html += uptimeStr;
  html += F(" &bull; RSSI: ");
  html += String(WiFi.RSSI());
  html += F(" dBm</div><div style='font-size:.65rem;opacity:.4;margin-top:8px'>"
            "Auto-refresh 30s</div></div></body></html>");

  server.send(200, "text/html", html);
}

// ─── Settings form (name + tank calibration) ─────────────────
inline void serveConfigPage() {
  String html;
  html.reserve(1400);
  html += F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Water Tank Settings</title><style>"
            "body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;margin:0;padding:20px}"
            "form{max-width:360px;margin:auto}label{display:block;font-size:.75rem;margin:10px 0 4px}"
            "input{width:100%;padding:8px;border-radius:6px;border:1px solid #333;background:#0f3460;color:#fff;box-sizing:border-box}"
            "button{width:100%;margin-top:16px;padding:10px;background:#0d7377;color:#fff;border:none;border-radius:6px}"
            "</style></head><body><form method='POST' action='/api/config'><h2>Settings</h2>");
  html += F("<label>Device name</label><input name='name' maxlength='32' value='");
  html += config.name;
  html += F("'><label>Empty distance (cm, sensor to bottom)</label><input name='tankMin' type='number' value='");
  html += String(config.tankEmpty);
  html += F("'><label>Full distance (cm, sensor to full water)</label><input name='tankMax' type='number' value='");
  html += String(config.tankFull);
  html += F("'><label>Tank girth (cm)</label><input name='tankGirth' type='number' value='");
  html += String(config.tankGirth);
  html += F("'><label>Number of tanks</label><input name='tankCount' type='number' min='1' value='");
  html += String(config.tankCount);
  html += F("'><button type='submit'>Save</button></form></body></html>");
  server.send(200, "text/html", html);
}

// ─── Save settings (form fields or query args) ───────────────
inline void handleConfigSave() {
  bool calibrationChanged = false;
  if (server.hasArg("name") && server.arg("name").length()) {
    strncpy(config.name, server.arg("name").c_str(), sizeof(config.name) - 1);
    config.name[sizeof(config.name) - 1] = '\0';
  }
  if (server.hasArg("tankMin") && server.arg("tankMin").toInt() > 0) {
    config.tankEmpty = server.arg("tankMin").toInt();
    calibrationChanged = true;
  }
  if (server.hasArg("tankMax") && server.arg("tankMax").toInt() > 0) {
    config.tankFull = server.arg("tankMax").toInt();
    calibrationChanged = true;
  }
  if (server.hasArg("tankGirth")) config.tankGirth = server.arg("tankGirth").toInt();
  if (server.hasArg("tankCount")) {
    long c = server.arg("tankCount").toInt();
    config.tankCount = c > 0 && c < 255 ? c : 1;
  }
  if (config.tankEmpty <= config.tankFull)
    LOGW("CONFIG", "empty distance (%u) should be larger than full distance (%u)", config.tankEmpty,
         config.tankFull);
  saveConfigToEEPROM();
  if (calibrationChanged) resetLevelSmoothing(); // re-home to the new calibration
  LOGI("CONFIG", "settings saved via web: name=%s empty=%u full=%u girth=%u tanks=%u", config.name,
       config.tankEmpty, config.tankFull, config.tankGirth, config.tankCount);
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "Saved");
}

// ─── Setup Dashboard Routes (Normal Mode) ────────────────────
inline void setupRoutes() {
  // Serve simple dashboard
  server.on("/", HTTP_GET, []() {
    serveDashboard();
  });

  // Settings
  server.on("/config", HTTP_GET, []() { serveConfigPage(); });
  server.on("/api/config", HTTP_POST, []() { handleConfigSave(); });
  server.on("/api/config", HTTP_GET, []() {
    char out[200];
    snprintf(out, sizeof out,
             "{\"name\":\"%s\",\"ssid\":\"%s\",\"tankMin\":%u,\"tankMax\":%u,\"tankGirth\":%u,"
             "\"tankCount\":%u}",
             config.name, config.ssid, config.tankEmpty, config.tankFull, config.tankGirth, config.tankCount);
    server.send(200, "application/json", out);
  });

  // Status (JSON)
  server.on("/api/status", HTTP_GET, []() {
    char out[240];
    snprintf(out, sizeof out,
             "{\"level\":%.1f,\"distance\":%.1f,\"filling\":%s,\"fillRate\":%.2f,\"uptime\":%lu,"
             "\"heap\":%u,\"rssi\":%d,\"mqtt\":%s,\"fw\":\"%s\"}",
             currentLevel, currentDistance, isFilling ? "true" : "false", fillRate,
             static_cast<unsigned long>(millis() / 1000), ESP.getFreeHeap(), WiFi.RSSI(),
             RemoteLog::connected() ? "true" : "false", FW_VERSION);
    server.send(200, "application/json", out);
  });

  // API: Remote restart
  server.on("/api/restart", HTTP_GET, []() {
    server.send(200, "text/plain", "Restarting...");
    LOGW("SERVER", "restart requested via /api/restart from %s", server.client().remoteIP().toString().c_str());
    delay(500);
    Diag::restart("restart via /api/restart");
  });

  // 404 handler — serve dashboard for any unknown route
  server.onNotFound([]() {
    LOGD("SERVER", "unknown route %s -> dashboard", server.uri().c_str());
    serveDashboard();
  });

  LOGI("SERVER", "dashboard routes configured");
}

// ─── Setup Captive Portal Routes (AP Mode) ───────────────────
inline void setupCaptivePortalRoutes() {
  server.on("/", HTTP_GET, []() {
    String html = F(
      "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Water Tank Setup</title></head>"
      "<body style='margin:0;min-height:100vh;display:flex;align-items:center;"
      "justify-content:center;font-family:system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0'>"
      "<div style='background:#16213e;padding:30px;border-radius:12px;max-width:320px;width:90%'>"
      "<h2 style='text-align:center;margin:0 0 20px'>Water Tank Setup</h2>"
      "<form method='POST' action='/api/wifi'>"
      "<label style='font-size:.75rem;display:block;margin-bottom:4px'>WiFi SSID</label>"
      "<input name='ssid' style='width:100%;padding:8px;margin-bottom:12px;border-radius:6px;"
      "border:1px solid #333;background:#0f3460;color:#fff' required>"
      "<label style='font-size:.75rem;display:block;margin-bottom:4px'>Password</label>"
      "<input name='pass' type='password' style='width:100%;padding:8px;margin-bottom:16px;"
      "border-radius:6px;border:1px solid #333;background:#0f3460;color:#fff'>"

      "<button type='submit' style='width:100%;padding:10px;background:#0d7377;color:#fff;"
      "border:none;border-radius:6px;font-size:1rem;cursor:pointer'>Connect</button>"
      "</form></div></body></html>"
    );
    server.send(200, "text/html", html);
  });

  // API: Save WiFi credentials and restart
  server.on("/api/wifi", HTTP_POST, []() {
    if (server.hasArg("ssid")) {
      strncpy(config.ssid, server.arg("ssid").c_str(), sizeof(config.ssid) - 1);
    }
    if (server.hasArg("pass")) {
      strncpy(config.pass, server.arg("pass").c_str(), sizeof(config.pass) - 1);
    }
    LOGI("SERVER", "setup portal: Wi-Fi '%s' saved", config.ssid);

    // Set defaults
    if (config.tankEmpty == 0) config.tankEmpty = 200;
    if (config.tankFull == 0) config.tankFull = 20;
    if (strlen(config.name) == 0)
      strncpy(config.name, "WaterTank", sizeof(config.name) - 1);

    saveConfigToEEPROM();

    server.send(200, "text/html",
      "<html><body style='background:#1a1a2e;color:#fff;text-align:center;"
      "font-family:system-ui;padding-top:40vh'>"
      "<h2>Saved! Restarting...</h2></body></html>");

    delay(1000);
    Diag::restart("new Wi-Fi credentials from setup portal");
  });

  // Captive portal: redirect all unknown requests to root
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });

  LOGI("SERVER", "captive portal routes configured");
}

#endif // DASHBOARD_SERVER_H
