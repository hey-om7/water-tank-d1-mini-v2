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
  Serial.printf("[WIFI] Connecting to: %s", config.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid, config.pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT) {
      Serial.println(" FAILED");
      Serial.println("[WIFI] Connection timed out");
      return false;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println(" OK");
  Serial.print("[WIFI] IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.printf("[WIFI] Signal strength (RSSI): %d dBm\n", WiFi.RSSI());
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
  Serial.print("[WIFI] AP Mode started. IP: ");
  Serial.println(WiFi.softAPIP());
}

// ─── Setup mDNS ─────────────────────────────────────────────
inline bool setupMDNS() {
  if (MDNS.begin(MDNS_NAME)) {
    Serial.printf("[WIFI] mDNS started: http://%s.local\n", MDNS_NAME);
    return true;
  }
  Serial.println("[WIFI] mDNS failed to start");
  return false;
}

// ─── Get Device MAC Address ──────────────────────────────────
inline String getDeviceMAC() { return WiFi.macAddress(); }

#endif // WIFI_LOGIC_H
