/*
 * wifi_logic.h — WiFi Connection Management
 * ============================================
 * Handles STA mode connection, AP fallback with captive portal,
 * and mDNS hostname setup.
 */

#ifndef WIFI_LOGIC_H
#define WIFI_LOGIC_H

#include "globals.h"
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>

// ─── Connect to WiFi (STA Mode) ─────────────────────────────
inline bool connectWiFi() {
  printLog("[WIFI] Connecting to: %s", config.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid, config.pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT) {
      printLog(" FAILED");
      printLog("[WIFI] Connection timed out");
      return false;
    }
    delay(500);
    printLog(".");
  }
  printLog(" OK\n");
  printLog("[WIFI] IP Address: ");
  printLog(WiFi.localIP().toString().c_str());
  printLog("\n");
  printLog("[WIFI] Signal strength (RSSI): %d dBm\n", WiFi.RSSI());
  return true;
}

// ─── Start Access Point Mode ─────────────────────────────────
inline void startAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);

  // Explicitly configure AP IP to 192.168.4.1
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  WiFi.softAP(AP_SSID, AP_PASS);
  printLog("[WIFI] AP Mode started. IP: ");
  printLog(WiFi.softAPIP().toString().c_str());
  printLog("\n");
}

// ─── Setup mDNS ─────────────────────────────────────────────
inline bool setupMDNS() {
  if (MDNS.begin(MDNS_NAME)) {
    printLog("[WIFI] mDNS started: http://%s.local\n", MDNS_NAME);
    return true;
  }
  printLog("[WIFI] mDNS failed to start");
  return false;
}

// ─── Get Device MAC Address ──────────────────────────────────
inline String getDeviceMAC() { return WiFi.macAddress(); }

#endif // WIFI_LOGIC_H
