#pragma once
// ---------------------------------------------------------------------------
//  wifi_connect.h - Wi-Fi for Tank Monitor Device Model-1 v1 (ESP8266):
//  saved credentials (EEPROM), setup portal, power saving, mDNS, reconnects.
//
//    WifiConnect::begin()  connect with the credentials saved in EEPROM.
//                          Opens the setup portal (SETUP_AP_SSID,
//                          http://192.168.4.1) when nothing is saved (stays
//                          open until configured) or when the saved network
//                          can't be reached (WIFI_PORTAL_TIMEOUT_MS, then
//                          reboots and tries again).
//    WifiConnect::loop()   call from loop(): mDNS upkeep, ARP announce,
//                          reconnect, reboot after WIFI_LOST_RESTART_MS.
//  Credentials are stored by Settings (EEPROM bytes 1-96), so the SDK's own
//  flash copy is disabled (WiFi.persistent(false)).
//  HEADER-ONLY.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <lwip/etharp.h>
#include <lwip/netif.h>
#include "../config/globals.h"
#include "../config/settings.h"
#include "../diag/crumbs.h"
#include "../logging/custom_mqtt_logging.h"

namespace WifiConnect {

namespace detail {

struct State {
  bool mdnsUp = false;
  uint32_t downSince = 0;     // 0 = link up
  uint32_t lastRetry = 0;
  uint32_t drops = 0;
  int lastReason = 0;         // last 802.11 disconnect reason
  String lastError;           // shown on the setup portal after a failed join
  bool sleeping = false;      // power saving currently applied
  WiFiEventHandler onDisc, onIp;
};
inline State& S() {
  static State s;
  return s;
}

inline String htmlEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    if (c == '&') o += F("&amp;");
    else if (c == '<') o += F("&lt;");
    else if (c == '>') o += F("&gt;");
    else if (c == '"') o += F("&quot;");
    else if (c == '\'') o += F("&#39;");
    else o += c;
  }
  return o;
}

inline void applyPowerSave() {
#if POWER_SAVE_MODE == 2
  WiFi.setSleepMode(WIFI_LIGHT_SLEEP, WIFI_LISTEN_INTERVAL);
#elif POWER_SAVE_MODE == 1
  WiFi.setSleepMode(WIFI_MODEM_SLEEP, WIFI_LISTEN_INTERVAL);
#else
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif
  WiFi.setOutputPower(WIFI_TX_POWER_DBM);
}

inline void startMdns() {
  MDNS.end();
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "model", MODEL_NUMBER);
    S().mdnsUp = true;
    LOGI("WIFI", "mDNS: http://%s.local", MDNS_NAME);
  } else {
    S().mdnsUp = false;
    LOGW("WIFI", "mDNS failed to start");
  }
}

/** Gratuitous ARP so the router keeps our IP -> MAC entry while we sleep. */
inline void announceArp() {
  for (netif* n = netif_list; n; n = n->next) {
    if (netif_is_up(n) && netif_is_link_up(n) && n->hwaddr_len == ETH_HWADDR_LEN &&
        !ip4_addr_isany_val(*netif_ip4_addr(n)))
      etharp_gratuitous(n);
  }
}

inline const char* statusName(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "idle (associating / waiting for DHCP)";
    case WL_NO_SSID_AVAIL:   return "network not found";
    case WL_CONNECTED:       return "connected";
    case WL_CONNECT_FAILED:  return "connect failed";
    case WL_WRONG_PASSWORD:  return "wrong password";
    case WL_DISCONNECTED:    return "disconnected";
    default:                 return "unknown";
  }
}

inline const char* reasonName(int r) {
  switch (r) {
    case 2:   return "auth expired";
    case 4:   return "assoc expired";
    case 8:   return "left network";
    case 15:  return "4-way handshake timeout (usually wrong password)";
    case 200: return "beacon timeout";
    case 201: return "network not found";
    case 202: return "auth failed (wrong password?)";
    case 203: return "assoc failed";
    case 204: return "handshake timeout (usually wrong password)";
    default:  return "other";
  }
}

inline void setupEvents() {
  S().onDisc = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& e) {
    const int r = static_cast<int>(e.reason);
    if (r != S().lastReason)  // every distinct reason, also while still connecting
      Serial.printf("[W][WIFI] disconnect reason %d: %s\n", r, reasonName(r));
    S().lastReason = r;
    if (S().downSince) return;  // the SDK repeats this on every retry
    S().downSince = millis() ? millis() : 1;
    S().mdnsUp = false;
    ++S().drops;
    LOGW("WIFI", "disconnected from '%s' reason=%d (drop #%u)", e.ssid.c_str(), static_cast<int>(e.reason),
         static_cast<unsigned>(S().drops));
  });
  S().onIp = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& e) {
    if (!S().downSince) return;  // first connect is logged by begin()
    LOGI("WIFI", "reconnected after %lus: ip=%s", static_cast<unsigned long>((millis() - S().downSince) / 1000),
         e.ip.toString().c_str());
    S().downSince = 0;
  });
}

/**
 * Join the saved network. The radio is kept fully awake (no sleep) until an
 * IP is assigned: with light/modem sleep active during association the
 * ESP8266 misses EAPOL/DHCP replies, and phone hotspots in particular kick
 * the station within seconds. Power saving is applied once connected.
 */
inline bool connectSaved() {
  const Settings::Config& c = Settings::get();
  LOGI("WIFI", "connecting to '%s' (ssid %u bytes, password %u chars)", c.ssid,
       static_cast<unsigned>(strlen(c.ssid)), static_cast<unsigned>(strlen(c.pass)));
  Crumbs::stage("wifi-connect");
  WiFi.mode(WIFI_STA);
  WiFi.hostname(DEVICE_HOSTNAME);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setOutputPower(20.5f);  // full power while joining
  WiFi.setAutoReconnect(true);
  WiFi.begin(c.ssid, c.pass);
  const uint32_t t0 = millis();
  wl_status_t last = WL_IDLE_STATUS;
  while (WiFi.status() != WL_CONNECTED) {
    const wl_status_t st = WiFi.status();
    if (st != last) {
      LOGI("WIFI", "status after %lums: %s (%d)", static_cast<unsigned long>(millis() - t0), statusName(st),
           static_cast<int>(st));
      last = st;
    }
    if (millis() - t0 > WIFI_CONNECT_TIMEOUT_MS) {
      S().lastError = String("Could not join '") + c.ssid + "': " + statusName(st);
      if (S().lastReason) S().lastError += String(", reason ") + S().lastReason + " " + reasonName(S().lastReason);
      LOGE("WIFI", "%s", S().lastError.c_str());
      return false;
    }
    ESP.wdtFeed();
    delay(100);
  }
  LOGI("WIFI", "connected in %lums: ip=%s gw=%s rssi=%d dBm ch=%d", static_cast<unsigned long>(millis() - t0),
       WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
  applyPowerSave();
  S().sleeping = true;
  return true;
}

inline String portalPage(const String& options, const String& note) {
  String h;
  h.reserve(1800 + options.length());
  h += F("<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'><title>Wi-Fi setup</title><style>"
         "body{font-family:system-ui,sans-serif;margin:0;padding:24px;background:#0f172a;color:#e2e8f0}"
         "main{max-width:380px;margin:auto}h1{font-size:1.3rem;margin:0 0 4px}p{color:#94a3b8;font-size:.85rem}"
         "label{display:block;margin:16px 0 6px;font-size:.85rem}"
         "input{width:100%;box-sizing:border-box;padding:10px;border:1px solid #334155;border-radius:8px;"
         "background:#1e293b;color:#fff;font-size:1rem}"
         "button{margin-top:20px;width:100%;padding:12px;border:0;border-radius:8px;background:#0ea5e9;"
         "color:#fff;font-size:1rem}</style></head><body><main><h1>");
  h += htmlEscape(DEVICE_NAME);
  h += F("</h1><p>Model " MODEL_NUMBER " &middot; MAC ");
  h += WiFi.macAddress();
  h += F("</p>");
  if (note.length()) {
    h += F("<p role='alert' style='color:#fca5a5'>");
    h += htmlEscape(note);
    h += F("</p>");
  }
  h += F("<form method='POST' action='/save'><label for='ssid'>Network</label>"
         "<input id='ssid' name='ssid' list='nets' required maxlength='32' autocomplete='off'><datalist id='nets'>");
  h += options;
  h += F("</datalist><label for='pass'>Password</label>"
         "<input id='pass' name='pass' type='password' maxlength='64'>"
         "<button type='submit'>Save &amp; connect</button></form></main></body></html>");
  return h;
}

/**
 * Blocking captive portal. Saves the credentials and reboots on success.
 * timeoutMs 0 = stay open until configured.
 */
inline void runPortal(uint32_t timeoutMs) {
  Crumbs::stage("portal");
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  delay(100);
  String options;
  const int n = WiFi.scanNetworks();
  for (int i = 0; i < n && i < 20; ++i) options += "<option value=\"" + htmlEscape(WiFi.SSID(i)) + "\">";
  WiFi.scanDelete();

  WiFi.mode(WIFI_AP);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);  // phones must see the hotspot reliably
  const IPAddress ip(192, 168, 4, 1);
  WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
  const bool secured = strlen(SETUP_AP_PASSWORD) >= 8;
  WiFi.softAP(SETUP_AP_SSID, secured ? SETUP_AP_PASSWORD : nullptr);
  LOGW("WIFI", "setup portal: join '%s' and open http://%s (%s)", SETUP_AP_SSID, ip.toString().c_str(),
       timeoutMs ? "closes after a few minutes" : "open until configured");

  DNSServer dns;
  dns.start(53, "*", ip);
  ESP8266WebServer web(80);
  bool saved = false;
  String note = S().lastError;  // why the saved network failed (empty on first boot)

  web.on("/save", HTTP_POST, [&]() {
    const String ssid = web.arg("ssid");
    const String pass = web.arg("pass");
    if (!ssid.length() || ssid.length() > 32 || pass.length() > 64) {
      web.send(400, "text/html", portalPage(options, "Enter a network name (max 32) and password (max 64)."));
      return;
    }
    Settings::Config& c = Settings::get();
    memset(c.ssid, 0, sizeof c.ssid);
    memset(c.pass, 0, sizeof c.pass);
    strncpy(c.ssid, ssid.c_str(), sizeof c.ssid - 1);
    strncpy(c.pass, pass.c_str(), sizeof c.pass - 1);
    Settings::save();
    web.send(200, "text/html",
             "<!doctype html><meta name='viewport' content='width=device-width'>"
             "<p style='font-family:system-ui;padding:24px'>Saved. Restarting and joining <b>" + htmlEscape(ssid) +
                 "</b>. Then open <b>http://" MDNS_NAME ".local</b>. If it fails, this hotspot comes back.</p>");
    saved = true;
  });
  web.onNotFound([&]() { web.send(200, "text/html", portalPage(options, note)); });
  web.begin();

  const uint32_t t0 = millis();
  while (!saved && (timeoutMs == 0 || millis() - t0 < timeoutMs)) {
    dns.processNextRequest();
    web.handleClient();
    ESP.wdtFeed();
    delay(5);
  }
  for (int i = 0; i < 50; ++i) { web.handleClient(); delay(10); }  // let the reply go out
  web.stop();
  dns.stop();
  Diag::restart(saved ? "new Wi-Fi credentials from setup portal" : "setup portal timed out, retrying saved Wi-Fi");
}

}  // namespace detail

inline bool connected() { return WiFi.status() == WL_CONNECTED; }

/** Connect at boot; never returns without a connection (portal reboots). */
inline void begin() {
  WiFi.persistent(false);  // credentials live in EEPROM, don't wear the SDK flash sector
  detail::setupEvents();
  if (!Settings::hasWifi()) {
    LOGI("WIFI", "no saved network - opening the setup portal");
    detail::runPortal(0);
  }
  if (detail::connectSaved()) {
    detail::startMdns();
    detail::announceArp();
    return;
  }
  detail::runPortal(WIFI_PORTAL_TIMEOUT_MS);
}

/** Call from loop(). */
inline void loop() {
  detail::State& s = detail::S();
  const uint32_t now = millis();
  if (connected()) {
    s.downSince = 0;
    if (!s.sleeping) {  // back online: power saving again
      detail::applyPowerSave();
      s.sleeping = true;
    }
    if (!s.mdnsUp) detail::startMdns();
    else MDNS.update();
    static uint32_t lastArp = 0;
    if (now - lastArp >= ARP_ANNOUNCE_INTERVAL_MS) {
      lastArp = now;
      detail::announceArp();
    }
    return;
  }
  if (!s.downSince) s.downSince = now ? now : 1;
  if (s.sleeping) {  // rejoin with the radio fully awake (see connectSaved)
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setOutputPower(20.5f);
    s.sleeping = false;
  }
  if (now - s.downSince >= WIFI_LOST_RESTART_MS) Diag::restart("Wi-Fi lost for too long");
  if (now - s.lastRetry >= WIFI_RECONNECT_INTERVAL_MS) {
    s.lastRetry = now;
    LOGD("WIFI", "still offline (status=%d), reconnecting", static_cast<int>(WiFi.status()));
    WiFi.reconnect();
  }
}

}  // namespace WifiConnect
