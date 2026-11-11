#pragma once
// ---------------------------------------------------------------------------
//  wifi_connect.h - Wi-Fi for Tank Monitor Device Model-1 v1: saved credentials, a setup
//  portal on first boot, and automatic reconnects.
//
//    WifiConnect::begin()  connect with the credentials saved in NVS. Opens the
//                          setup portal (SETUP_AP_SSID, http://192.168.4.1)
//                          right away when nothing is saved or the setup
//                          button is held at power-up; after a failed
//                          connect it stays up for WIFI_PORTAL_TIMEOUT_MS.
//    WifiConnect::loop()   call from loop(): retries while disconnected.
//    WifiConnect::forget() erase saved credentials (e.g. on a long press).
//  HEADER-ONLY.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include "../config/globals.h"
#include "../logging/custom_mqtt_logging.h"

namespace WifiConnect {

namespace detail {

inline String htmlEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

inline bool waitConnected(uint32_t timeoutMs) {
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
  return WiFi.status() == WL_CONNECTED;
}

inline String portalPage(const String& options, const String& note) {
  String html = F("<!doctype html><html><head><meta charset='utf-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>Wi-Fi setup</title><style>"
                  "body{font-family:system-ui,sans-serif;margin:0;padding:24px;background:#f5f3ee;color:#1d2433}"
                  "main{max-width:380px;margin:auto}h1{font-size:1.4rem;margin:0 0 4px}"
                  "p{color:#5b6475;font-size:.9rem}label{display:block;margin:16px 0 6px;font-weight:600}"
                  "input,select{width:100%;box-sizing:border-box;padding:10px;border:1px solid #c9c4b8;border-radius:6px;font-size:1rem}"
                  "button{margin-top:20px;width:100%;padding:12px;border:0;border-radius:6px;background:#1d2433;color:#fff;font-size:1rem}"
                  "button:focus-visible,input:focus-visible,select:focus-visible{outline:3px solid #7c8cf0;outline-offset:2px}"
                  "</style></head><body><main><h1>");
  html += htmlEscape(DEVICE_NAME);
  html += F("</h1><p>Model ");
  html += htmlEscape(MODEL_NUMBER);
  html += F(" &middot; MAC ");
  html += WiFi.macAddress();
  html += F("</p>");
  if (note.length()) {
    html += F("<p role='alert'><strong>");
    html += htmlEscape(note);
    html += F("</strong></p>");
  }
  html += F("<form method='POST' action='/save'><label for='ssid'>Network</label>"
            "<input id='ssid' name='ssid' list='nets' required maxlength='32' autocomplete='off'>"
            "<datalist id='nets'>");
  html += options;
  html += F("</datalist><label for='pass'>Password</label>"
            "<input id='pass' name='pass' type='password' maxlength='63' autocomplete='current-password'>"
            "<button type='submit'>Save &amp; connect</button></form></main></body></html>");
  return html;
}

/** SSID stored in NVS by a previous run ("" = none). Survives re-flashing. */
inline String savedSsid() {
  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return "";
  return String(reinterpret_cast<const char*>(conf.sta.ssid));
}

/** True while the setup button (BOOT on most boards) is held at power-up. */
inline bool setupButtonHeld() {
#if SETUP_BUTTON_PIN >= 0
  pinMode(SETUP_BUTTON_PIN, INPUT_PULLUP);
  delay(20);
  return digitalRead(SETUP_BUTTON_PIN) == LOW;
#else
  return false;
#endif
}

/**
 * Blocking setup portal. Returns true once connected with new credentials.
 * `timeoutMs` 0 = stay open until configured (first boot, nothing saved).
 */
inline bool runPortal(uint32_t timeoutMs) {
  // Stop the station from retrying a saved network first: while it hops
  // channels looking for it, the hotspot's beacons drop out and phones don't
  // list it. Credentials stay in NVS.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(100);

  // Scan BEFORE the hotspot starts: a scan also hops channels.
  String options;
  const int n = WiFi.scanNetworks();
  for (int i = 0; i < n; ++i) {
    options += "<option value=\"" + htmlEscape(WiFi.SSID(i)) + "\">";
  }
  WiFi.scanDelete();

  WiFi.mode(WIFI_AP_STA);
#if WIFI_TX_POWER_LIMIT
  WiFi.setTxPower(WIFI_POWER_8_5dBm);  // some ESP32-C3 boards only radiate cleanly at low power
#endif
  const bool secured = strlen(SETUP_AP_PASSWORD) >= 8;
  if (!WiFi.softAP(SETUP_AP_SSID, secured ? SETUP_AP_PASSWORD : nullptr)) {
    LOGE("WIFI", "could not start the setup hotspot '%s'", SETUP_AP_SSID);
    WiFi.mode(WIFI_STA);
    return false;
  }
  delay(200);
  const IPAddress ip = WiFi.softAPIP();
  LOGI("WIFI", "setup portal: join '%s'%s and open http://%s (%s)", SETUP_AP_SSID, secured ? " (password set)" : "",
       ip.toString().c_str(), timeoutMs ? "closes after a few minutes" : "open until configured");

  DNSServer dns;
  dns.start(53, "*", ip);  // captive: every name resolves to the portal
  WebServer web(80);
  bool connected = false;
  String note;

  web.on("/save", HTTP_POST, [&]() {
    const String ssid = web.arg("ssid");
    const String pass = web.arg("pass");
    if (!ssid.length()) {
      web.send(400, "text/html", portalPage(options, "Enter a network name."));
      return;
    }
    web.send(200, "text/html",
             "<!doctype html><meta name='viewport' content='width=device-width'>"
             "<p style='font-family:system-ui;padding:24px'>Connecting to <b>" + htmlEscape(ssid) +
             "</b>&hellip; If it fails, this network will reappear.</p>");
    delay(300);
    LOGI("WIFI", "portal: trying '%s'", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());  // persisted in NVS by the driver
    connected = waitConnected(WIFI_CONNECT_TIMEOUT_MS);
    if (!connected) {
      LOGW("WIFI", "portal: could not join '%s'", ssid.c_str());
      note = "Could not join that network. Check the password.";
    }
  });
  web.onNotFound([&]() { web.send(200, "text/html", portalPage(options, note)); });
  web.begin();

  const uint32_t t0 = millis();
  while (!connected && (timeoutMs == 0 || millis() - t0 < timeoutMs)) {
    dns.processNextRequest();
    web.handleClient();
    delay(5);
  }
  web.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  return connected;
}

}  // namespace detail

/**
 * Connect at boot. Opens the setup portal when:
 *   - nothing is saved (first boot)        -> immediately, stays open until configured;
 *   - the setup button is held at power-up -> immediately (change networks);
 *   - the saved network can't be reached   -> after WIFI_CONNECT_TIMEOUT_MS,
 *                                             for WIFI_PORTAL_TIMEOUT_MS.
 * Note: Wi-Fi credentials live in NVS, which a normal USB flash does NOT
 * erase. A board that ran other firmware before may silently join that
 * firmware's network. Hold the setup button at boot, or flash with
 * Tools -> Erase All Flash Before Sketch Upload -> Enabled.
 */
inline bool begin() {
  const bool forcePortal = detail::setupButtonHeld();
  WiFi.mode(WIFI_STA);
#if WIFI_TX_POWER_LIMIT
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
#endif
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);

  const String saved = detail::savedSsid();
  if (forcePortal) {
    LOGI("WIFI", "setup button held - opening the setup portal");
  } else if (!saved.length()) {
    LOGI("WIFI", "no saved network - opening the setup portal");
  } else {
    WiFi.begin();  // credentials saved in NVS by a previous portal run
    LOGI("WIFI", "connecting to saved network '%s'...", saved.c_str());
    if (detail::waitConnected(WIFI_CONNECT_TIMEOUT_MS)) {
      LOGI("WIFI", "connected to '%s' ip=%s rssi=%d", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
           WiFi.RSSI());
      return true;
    }
    LOGW("WIFI", "saved network '%s' not reachable (status=%d)", saved.c_str(), static_cast<int>(WiFi.status()));
  }

  const uint32_t timeout = (saved.length() && !forcePortal) ? WIFI_PORTAL_TIMEOUT_MS : 0;
  if (detail::runPortal(timeout)) {
    LOGI("WIFI", "connected via portal to '%s' ip=%s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    return true;
  }
  LOGW("WIFI", "setup portal timed out - continuing offline, will keep retrying");
  WiFi.begin();
  return false;
}

/** Call from loop(): logs link changes and retries while disconnected. */
inline void loop() {
  static bool was = false;
  static uint32_t lastTry = 0;
  const bool now = WiFi.status() == WL_CONNECTED;
  if (now && !was) LOGI("WIFI", "online ip=%s rssi=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  if (!now && was) LOGW("WIFI", "link lost (status=%d)", static_cast<int>(WiFi.status()));
  was = now;
  if (!now && millis() - lastTry >= WIFI_RECONNECT_INTERVAL_MS) {
    lastTry = millis();
    if (!WiFi.reconnect()) WiFi.begin();
  }
}

/** Erase saved credentials; the setup portal opens on the next boot. */
inline void forget() {
  WiFi.mode(WIFI_STA);
  delay(100);
  WiFi.disconnect(true, true);
  LOGW("WIFI", "saved credentials erased");
}

}  // namespace WifiConnect
