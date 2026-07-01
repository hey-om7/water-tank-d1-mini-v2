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

  // IMPORTANT: persistent(false) MUST be called before begin() so
  // that credentials are not written to flash on every connect.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
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
  printLog(" OK");
  // NB: use "%s" so IP text is never interpreted as a printf format.
  printLog("[WIFI] IP Address: %s", WiFi.localIP().toString().c_str());
  printLog("[WIFI] Signal strength (RSSI): %d dBm", WiFi.RSSI());
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
  // NB: "%s" prevents the IP text from being treated as a printf format.
  printLog("[WIFI] AP Mode started. IP: %s",
           WiFi.softAPIP().toString().c_str());
}

// ─── mDNS running-state flag ────────────────────────────────
// Shared between setupMDNS() (called from setup()) and
// maintainMDNS() (called from loop()) so that the loop-side
// state machine doesn't tear the mDNS stack down and rebuild
// it on the very first iteration.
inline bool &mdnsRunningRef() {
  static bool running = false;
  return running;
}

// ─── Setup mDNS ─────────────────────────────────────────────
inline bool setupMDNS() {
  MDNS.end(); // Ensure mDNS is stopped before starting it again (for reconnects)
  if (MDNS.begin(MDNS_NAME)) {
    // Advertise HTTP service so mDNS browsers and clients can discover it
    MDNS.addService("http", "tcp", 80);
    printLog("[WIFI] mDNS started: http://%s.local", MDNS_NAME);
    mdnsRunningRef() = true;
    return true;
  }
  printLog("[WIFI] mDNS failed to start");
  mdnsRunningRef() = false;
  return false;
}

// ─── Maintain mDNS (call from loop) ─────────────────────────
// Re-initialises mDNS if WiFi reconnects after a drop so that
// watertank.local keeps resolving reliably.
inline void maintainMDNS() {
  static unsigned long lastCheck = 0;
  const unsigned long CHECK_INTERVAL = 5000; // check every 5 s

  unsigned long now = millis();
  if (now - lastCheck < CHECK_INTERVAL) return;
  lastCheck = now;

  bool &mdnsRunning = mdnsRunningRef();
  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected && !mdnsRunning) {
    // WiFi just came (back) up — (re)start mDNS
    setupMDNS();
  } else if (!connected && mdnsRunning) {
    // WiFi dropped — mark mDNS as down so we restart it when it returns
    mdnsRunning = false;
  } else if (connected && mdnsRunning) {
    // Normal operation — pump the mDNS stack
    MDNS.update();
  }
}

// ─── Get Device MAC Address ──────────────────────────────────
inline String getDeviceMAC() { return WiFi.macAddress(); }

#endif // WIFI_LOGIC_H
