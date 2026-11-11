/*
 * wifi_logic.h — WiFi Connection Management
 * ============================================
 * Handles STA mode connection, AP fallback with captive portal,
 * and mDNS hostname setup.
 */

#ifndef WIFI_LOGIC_H
#define WIFI_LOGIC_H

#include "custom_mqtt_logging.h"
#include "globals.h"
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <lwip/etharp.h>
#include <lwip/netif.h>

// ─── Connect to WiFi (STA Mode) ─────────────────────────────
inline bool connectWiFi() {
  LOGI("WIFI", "connecting to '%s'", config.ssid);
  Crumbs::stage("wifi-connect");

  // IMPORTANT: persistent(false) MUST be called before begin() so
  // that credentials are not written to flash on every connect.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  // Default modem sleep makes the radio miss incoming packets (ARP requests,
  // TCP SYNs). Outgoing traffic like MQTT keeps working, but after a while the
  // router drops the device from its ARP table and the web page stops
  // loading. Keep the radio awake: this device is mains powered.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoReconnect(true);
  WiFi.begin(config.ssid, config.pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT) {
      LOGE("WIFI", "connection to '%s' timed out after %lums (status=%d)", config.ssid,
           static_cast<unsigned long>(WIFI_CONNECT_TIMEOUT), static_cast<int>(WiFi.status()));
      return false;
    }
    delay(500);
  }
  LOGI("WIFI", "connected in %lums: ip=%s rssi=%d dBm ch=%d bssid=%s", static_cast<unsigned long>(millis() - start),
       WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel(), WiFi.BSSIDstr().c_str());
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
  LOGW("WIFI", "setup portal started: SSID '%s' at http://%s", AP_SSID,
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
    LOGI("WIFI", "mDNS started: http://%s.local", MDNS_NAME);
    mdnsRunningRef() = true;
    return true;
  }
  LOGW("WIFI", "mDNS failed to start");
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

// ─── Keep the router's ARP entry fresh (call from loop) ─────
// Sends a gratuitous ARP every 60 s so the router always knows which MAC owns
// our IP. Without it, some routers forget the device after a day or two and
// inbound connections (the dashboard) fail while outbound MQTT still works.
inline void maintainArp() {
  static unsigned long lastArp = 0;
  unsigned long now = millis();
  if (now - lastArp < 60000UL) return;
  lastArp = now;
  if (WiFi.status() != WL_CONNECTED) return;
  for (netif *n = netif_list; n; n = n->next) {
    if (netif_is_up(n) && netif_is_link_up(n) && n->hwaddr_len == ETH_HWADDR_LEN &&
        !ip4_addr_isany_val(*netif_ip4_addr(n)))
      etharp_gratuitous(n);
  }
}

// ─── Wi-Fi event logging ─────────────────────────────────────
// Logs every drop with the 802.11 reason code (e.g. 2/15 = auth, 8 = left,
// 200 = beacon timeout, 201 = AP not found, 202 = auth failed) and every
// reconnect with how long the link was down.
inline void setupWifiEvents() {
  static WiFiEventHandler onDisc, onIp;
  static uint32_t downSince = 0;
  static uint32_t drops = 0;
  onDisc = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected &e) {
    if (downSince) return; // already down: the SDK repeats this every retry
    downSince = millis() ? millis() : 1;
    ++drops;
    LOGW("WIFI", "disconnected from '%s' reason=%d (drop #%u, up %lus)", e.ssid.c_str(), static_cast<int>(e.reason),
         static_cast<unsigned>(drops), static_cast<unsigned long>(millis() / 1000));
  });
  onIp = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP &e) {
    if (!downSince) return; // first connect is logged by connectWiFi()
    LOGI("WIFI", "reconnected after %lus: ip=%s", static_cast<unsigned long>((millis() - downSince) / 1000),
         e.ip.toString().c_str());
    downSince = 0;
  });
}

// ─── Get Device MAC Address ──────────────────────────────────
inline String getDeviceMAC() { return WiFi.macAddress(); }

#endif // WIFI_LOGIC_H
