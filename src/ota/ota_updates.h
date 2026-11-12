/*
 * ota_updates.h — OTA updates from FirmwareManager (ESP8266)
 * =================================================================
 * Flow (same device API every FirmwareManager model uses):
 *  1. GET <base>/api/v1/device/firmware/version?macAddress=<MAC>
 *       -> {"data":"x.y.z", ...}  latest build for THIS device
 *  2. GET <base>/api/v1/device/sync?macAddress=<MAC>  (activates a
 *     FACTORY_NEW device and refreshes "last seen" in the console)
 *  3. if x.y.z is newer than the running version:
 *     GET <base>/api/v1/device/firmware?macAddress=<MAC> -> .bin, flashed
 *     with ESPhttpUpdate, then reboot.
 * <base> = FM_PRIMARY_BASE_URL first; the fallback only if the primary is
 * unreachable or its download fails.
 *
 * Running version = max(EEPROM-stored, FW_VERSION): a USB flash with a
 * higher FW_VERSION is never "downgraded", and an uploaded .bin that forgot
 * to bump FW_VERSION can't cause an update loop.
 *
 * Remote logging is suspended while a check runs (ESP8266 RAM only fits one
 * TLS session); lines keep buffering and are sent afterwards.
 */

#ifndef OTA_UPDATE_LOGIC_H
#define OTA_UPDATE_LOGIC_H

#include "../logging/custom_mqtt_logging.h"
#include "../config/globals.h"
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecureBearSSL.h>

namespace OtaUpdates {

enum class Status : uint8_t { IDLE, CHECKING, UP_TO_DATE, DOWNLOADING, FAILED, NOT_CONFIGURED, NO_PARTITION };

inline const char *statusName(Status s) {
  switch (s) {
  case Status::CHECKING: return "checking";
  case Status::UP_TO_DATE: return "up_to_date";
  case Status::DOWNLOADING: return "downloading";
  case Status::FAILED: return "failed";
  case Status::NOT_CONFIGURED: return "not_configured";
  case Status::NO_PARTITION: return "no_partition";
  default: return "idle";
  }
}

struct State {
  Status status = Status::IDLE;
  String current, latest, error;
  uint32_t nextCheckAt = 0;
  bool scheduled = false;
  bool requested = false;
  bool (*canUpdate)() = nullptr;
  void (*beforeFlash)() = nullptr;
};
inline State &St() {
  static State s;
  return s;
}

static constexpr const char *TAG = "OTA";

// true when `latest` is a higher x.y.z than `current`.
inline bool isNewer(String current, String latest) {
  current.trim();
  latest.trim();
  int cM = 0, cm = 0, cp = 0, lM = 0, lm = 0, lp = 0;
  sscanf(current.c_str(), "%d.%d.%d", &cM, &cm, &cp);
  sscanf(latest.c_str(), "%d.%d.%d", &lM, &lm, &lp);
  if (lM != cM) return lM > cM;
  if (lm != cm) return lm > cm;
  return lp > cp;
}

namespace detail {

inline String loadVersion() {
  char v[EEPROM_FIRMWARE_VERSION_SIZE + 1];
  for (int i = 0; i < EEPROM_FIRMWARE_VERSION_SIZE; i++)
    v[i] = EEPROM.read(EEPROM_FIRMWARE_VERSION_OFFSET + i);
  v[EEPROM_FIRMWARE_VERSION_SIZE] = '\0';
  if (v[0] < '0' || v[0] > '9') return "";
  return String(v);
}

inline void saveVersion(const String &v) {
  for (int i = 0; i < EEPROM_FIRMWARE_VERSION_SIZE; i++)
    EEPROM.write(EEPROM_FIRMWARE_VERSION_OFFSET + i, i < (int)v.length() ? v[i] : 0);
  EEPROM.commit();
}

inline void fail(const String &e) {
  St().error = e;
  St().status = Status::FAILED;
  LOGE(TAG, "%s", e.c_str());
}

// HTTP(S) client pair. TLS certificates are not verified (no CA store on
// ESP8266); the client objects are only allocated when used.
struct Http {
  WiFiClient plain;
  std::unique_ptr<BearSSL::WiFiClientSecure> secure;
  HTTPClient http;
  WiFiClient *client(const String &url) {
    if (!url.startsWith("https://")) return &plain;
    if (!secure) {
      secure.reset(new (std::nothrow) BearSSL::WiFiClientSecure);
      if (secure) secure->setInsecure();
    }
    return secure.get();
  }
  bool begin(const String &url, uint32_t timeoutMs) {
    WiFiClient *c = client(url);
    if (!c) return false;
    http.setTimeout(timeoutMs);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    return http.begin(*c, url);
  }
};

inline String macQuery() { return "?macAddress=" + WiFi.macAddress(); }

inline void sync(const String &base, uint32_t timeoutMs) {
  Http h;
  const String url = base + "/api/v1/device/sync" + macQuery();
  if (!h.begin(url, timeoutMs)) return;
  ESP.wdtFeed();
  const int code = h.http.GET();
  h.http.end();
  if (code == 404) LOGW(TAG, "this board (MAC %s) is not registered in FirmwareManager", WiFi.macAddress().c_str());
  else if (code == 403) LOGW(TAG, "this device is suspended in FirmwareManager");
  else LOGD(TAG, "sync %s -> %d", base.c_str(), code);
}

// Latest version for this device from one server. true + `out` on a usable
// answer; `reachable` tells whether the server replied at all.
inline bool fetchLatest(const String &base, uint32_t timeoutMs, String &out, bool &reachable) {
  reachable = false;
  Http h;
  const String url = base + "/api/v1/device/firmware/version" + macQuery();
  if (!h.begin(url, timeoutMs)) {
    LOGW(TAG, "cannot start request to %s (heap=%u)", url.c_str(), ESP.getFreeHeap());
    return false;
  }
  const uint32_t t0 = millis();
  ESP.wdtFeed();
  const int code = h.http.GET();
  ESP.wdtFeed();
  LOGD(TAG, "GET %s -> %d in %lu ms (heap=%u)", url.c_str(), code, static_cast<unsigned long>(millis() - t0),
       ESP.getFreeHeap());
  if (code <= 0) {
    LOGW(TAG, "%s unreachable: %s", base.c_str(), HTTPClient::errorToString(code).c_str());
    h.http.end();
    return false;
  }
  reachable = true;
  // Small JSON body; refuse anything large so it cannot exhaust the heap.
  if (h.http.getSize() > 1024) {
    LOGW(TAG, "version response from %s too large (%d bytes)", base.c_str(), h.http.getSize());
    h.http.end();
    return false;
  }
  const String body = h.http.getString();
  h.http.end();
  if (code == 404) {
    LOGI(TAG, "no firmware for this device on %s", base.c_str());
    out = "";
    return true;
  }
  if (code != 200) {
    LOGW(TAG, "version check on %s failed: HTTP %d", base.c_str(), code);
    return false;
  }
  // {"message":"...","data":"x.y.z","success":true,"statusCode":200}
  const int i = body.indexOf("\"data\":\"");
  if (i < 0) {
    LOGW(TAG, "unexpected version response from %s", base.c_str());
    return false;
  }
  const int start = i + 8;
  const int end = body.indexOf('"', start);
  out = end > start ? body.substring(start, end) : "";
  out.trim();
  return true;
}

// Download + flash from `base`. Returns only on failure (reboots on success).
inline bool install(const String &base) {
  const String url = base + "/api/v1/device/firmware" + macQuery();
  LOGI(TAG, "updating %s -> %s from %s (heap=%u maxBlock=%u)", St().current.c_str(), St().latest.c_str(),
       base.c_str(), ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
  St().status = Status::DOWNLOADING;
  if (St().beforeFlash) St().beforeFlash();
  Crumbs::stage("ota-download");

  Http h;
  WiFiClient *c = h.client(url);
  if (!c) {
    fail("out of memory for the TLS client");
    return false;
  }
  ESPhttpUpdate.setLedPin(LED_BUILTIN, LOW);
  ESPhttpUpdate.rebootOnUpdate(false);
  ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  static int lastPct = -1;
  lastPct = -1;
  ESPhttpUpdate.onProgress([](int done, int total) {
    const int pct = total > 0 ? static_cast<int>(static_cast<int64_t>(done) * 100 / total) : 0;
    if (pct / 25 != lastPct / 25) { // serial only: logging is suspended
      lastPct = pct;
      LOGD(TAG, "download %d%%", pct);
    }
  });

  const uint32_t t0 = millis();
  const t_httpUpdate_return r = ESPhttpUpdate.update(*c, url);

  switch (r) {
  case HTTP_UPDATE_OK: {
    saveVersion(St().latest);
    LOGI(TAG, "update to %s OK in %lus", St().latest.c_str(), static_cast<unsigned long>((millis() - t0) / 1000));
    RemoteLog::resume(); // reconnect so the result reaches the console
    const uint32_t w = millis();
    while (!RemoteLog::connected() && millis() - w < 15000) {
      MqttLogging::loop();
      ESP.wdtFeed();
      delay(10);
    }
    String note = "OTA update to " + St().latest;
    Diag::restart(note.c_str());
    return true; // not reached
  }
  case HTTP_UPDATE_NO_UPDATES:
    LOGI(TAG, "server reported no update");
    St().status = Status::UP_TO_DATE;
    return true;
  case HTTP_UPDATE_FAILED:
  default:
    fail("update from " + base + " failed (" + String(ESPhttpUpdate.getLastError()) +
         "): " + ESPhttpUpdate.getLastErrorString());
    return false;
  }
}

} // namespace detail

// Running firmware version: max(EEPROM-stored, FW_VERSION).
inline const String &currentVersion() {
  if (!St().current.length()) {
    const String stored = detail::loadVersion();
    St().current = (stored.length() && isNewer(FW_VERSION, stored)) ? stored : String(FW_VERSION);
  }
  return St().current;
}

inline Status status() { return St().status; }
inline const String &latestVersion() { return St().latest; }
inline const String &lastError() { return St().error; }

inline void setCanUpdate(bool (*fn)()) { St().canUpdate = fn; }
inline void setBeforeFlash(void (*fn)()) { St().beforeFlash = fn; }
inline void requestCheck() { St().requested = true; }

// Blocking: check primary then fallback, install if newer.
inline void checkNow() {
  St().error = "";
  const String primary = FmServer::primaryBase();
  const String fallback = FmServer::fallbackBase();
  if (!primary.length() && !fallback.length()) {
    St().status = Status::NOT_CONFIGURED;
    St().error = "set FM_PRIMARY_BASE_URL or FM_FALLBACK_HOST in globals.h";
    LOGW(TAG, "%s", St().error.c_str());
    return;
  }
  if (ESP.getFreeSketchSpace() < ESP.getSketchSize()) {
    St().status = Status::NO_PARTITION;
    St().error = "not enough free flash for an OTA download: pick a Flash Size with OTA space and flash over USB";
    LOGE(TAG, "%s", St().error.c_str());
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    detail::fail("Wi-Fi not connected");
    return;
  }

  St().status = Status::CHECKING;
  LOGI(TAG, "checking for updates (running %s, heap=%u maxBlock=%u)", currentVersion().c_str(), ESP.getFreeHeap(),
       ESP.getMaxFreeBlockSize());
  Crumbs::stage("ota-check");
  RemoteLog::suspend(); // free the MQTT TLS session for this request

  struct Server {
    String base;
    uint32_t timeout;
  } servers[] = {{primary, FM_PRIMARY_TIMEOUT_MS}, {fallback, FM_FALLBACK_TIMEOUT_MS}};

  bool anyReachable = false;
  bool done = false;
  for (const Server &s : servers) {
    if (!s.base.length()) continue;
    bool reachable = false;
    String latest;
    if (!detail::fetchLatest(s.base, s.timeout, latest, reachable)) {
      anyReachable = anyReachable || reachable;
      continue;
    }
    detail::sync(s.base, s.timeout);
    St().latest = latest;
    if (!latest.length() || !isNewer(currentVersion(), latest)) {
      St().status = Status::UP_TO_DATE;
      LOGI(TAG, "up to date (running %s, server %s)", currentVersion().c_str(),
           latest.length() ? latest.c_str() : "none");
      done = true;
      break;
    }
    if (detail::install(s.base)) {
      done = true;
      break;
    }
  }
  if (!done && St().status != Status::FAILED)
    detail::fail(anyReachable ? "version check failed (bad server response)" : "update server unreachable");

  RemoteLog::resume();
  Crumbs::stage("loop");
}

// Call from loop(): boot check, periodic checks and requests.
inline void loop() {
  if (WiFi.status() != WL_CONNECTED) return;
  const uint32_t now = millis();
  auto at = [](uint32_t t) -> uint32_t { return t ? t : 1; };
  if (!St().scheduled) {
    St().scheduled = true;
    if (OTA_CHECK_ON_BOOT) St().nextCheckAt = at(now);
    else if ((OTA_CHECK_INTERVAL_MS) != 0UL) St().nextCheckAt = at(now + OTA_CHECK_INTERVAL_MS);
  }
  const bool due = St().requested || (St().nextCheckAt && static_cast<int32_t>(now - St().nextCheckAt) >= 0);
  if (!due) return;
  if (St().canUpdate && !St().canUpdate()) return;
  St().requested = false;

  checkNow(); // reboots if it installed an update

  const uint32_t after = millis();
  if (St().status == Status::FAILED) St().nextCheckAt = at(after + OTA_RETRY_INTERVAL_MS);
  else St().nextCheckAt = (OTA_CHECK_INTERVAL_MS) != 0UL ? at(after + OTA_CHECK_INTERVAL_MS) : 0;
}

} // namespace OtaUpdates

#endif // OTA_UPDATE_LOGIC_H
