/*
 * dashboard_server.h — Minimal HTTP Server
 * ==========================================
 * Simple web page showing water level, auto-refreshes every 30s.
 * Retains: /api/restart, captive portal for AP mode.
 * No ArduinoJson, no Google Sheets, no history, no heavy CSS.
 */

#ifndef DASHBOARD_SERVER_H
#define DASHBOARD_SERVER_H

#include "config_logic.h"
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

// ─── Setup Dashboard Routes (Normal Mode) ────────────────────
inline void setupRoutes() {
  // Serve simple dashboard
  server.on("/", HTTP_GET, []() {
    serveDashboard();
  });

  // API: Remote restart
  server.on("/api/restart", HTTP_GET, []() {
    server.send(200, "text/plain", "Restarting...");
    printLog("[SERVER] Restart requested via API");
    delay(500);
    ESP.restart();
  });

  // 404 handler — serve dashboard for any unknown route
  server.onNotFound([]() {
    serveDashboard();
  });

  printLog("[SERVER] Routes configured (minimal)");
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
    ESP.restart();
  });

  // Captive portal: redirect all unknown requests to root
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });

  printLog("[SERVER] Captive portal routes configured");
}

#endif // DASHBOARD_SERVER_H
