#pragma once
// ---------------------------------------------------------------------------
//  ota_updates.h - over-the-air updates from FirmwareManager for
//  Tank Monitor Device Model-1 v1 (TMM1V1).
//
//  Flow (same device API every model uses):
//    1. GET <base>/api/v1/device/firmware/version?macAddress=<MAC>
//         -> {"data":"x.y.z", ...}  latest build for THIS device (its model,
//            release channel and rollout are resolved by the server)
//    2. if x.y.z is newer than the running version:
//       GET <base>/api/v1/device/firmware?macAddress=<MAC>  -> .bin, flashed
//       with HTTPUpdate into the inactive OTA slot, then reboot.
//  <base> = FM_PRIMARY_BASE_URL first; FM_FALLBACK_HOST only if the primary is
//  unreachable or its download fails.
//
//  Also calls GET /api/v1/device/sync once per check, which activates a
//  FACTORY_NEW device and refreshes its "last seen" in the console.
//
//  Running version = max(NVS-stored, FW_VERSION): a USB flash with a higher
//  FW_VERSION is never "downgraded", and an uploaded .bin that forgot to bump
//  FW_VERSION can't cause an update loop.
//
//  Non-blocking scheduling: call OtaUpdates::loop() from loop(). The check /
//  download itself blocks while it runs (remote logging is paused meanwhile
//  so the download has the radio; lines keep buffering).
//  HEADER-ONLY. Depends on ../config/globals.h and ../logging/custom_mqtt_logging.h.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include "../config/globals.h"
#include "../logging/custom_mqtt_logging.h"

// The CA bundle compiled into the ESP32 core (verifies https:// hosts).
extern const uint8_t fm_ota_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

namespace OtaUpdates {

enum class Status : uint8_t {
  IDLE,            // nothing checked yet
  CHECKING,
  UP_TO_DATE,
  DOWNLOADING,     // device reboots on success
  FAILED,          // see lastError()
  NOT_CONFIGURED,  // no backend URL in globals.h
  NO_PARTITION     // partition scheme has no OTA slot
};

inline const char* statusName(Status s) {
  switch (s) {
    case Status::CHECKING:       return "checking";
    case Status::UP_TO_DATE:     return "up_to_date";
    case Status::DOWNLOADING:    return "downloading";
    case Status::FAILED:         return "failed";
    case Status::NOT_CONFIGURED: return "not_configured";
    case Status::NO_PARTITION:   return "no_partition";
    default:                     return "idle";
  }
}

struct State {
  Status status = Status::IDLE;
  String current, latest, error;
  int progress = 0;           // 0..100 while DOWNLOADING
  uint32_t nextCheckAt = 0;   // millis(); 0 = as soon as Wi-Fi is up (if OTA_CHECK_ON_BOOT)
  bool scheduled = false;
  bool requested = false;
  bool (*canUpdate)() = nullptr;     // false = device busy, try later
  void (*beforeFlash)() = nullptr;   // release peripherals / show "updating" screen
  void (*onProgress)(int pct) = nullptr;
};
inline State& St() {
  static State s;
  return s;
}

static constexpr const char* TAG = "OTA";

/** true when `latest` is a higher x.y.z than `current`. */
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
  Preferences p;
  String v;
  if (p.begin("ota", true)) {
    v = p.getString("ver", "");
    p.end();
  }
  return v;
}

inline void saveVersion(const String& v) {
  Preferences p;
  if (p.begin("ota", false)) {
    p.putString("ver", v);
    p.end();
  }
}

inline void fail(const String& e) {
  St().error = e;
  St().status = Status::FAILED;
  LOGE(TAG, "%s", e.c_str());
}

/** HTTPClient bound to a plain or TLS client depending on the URL. */
struct Http {
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  bool begin(const String& url, uint32_t timeoutMs) {
    http.setConnectTimeout(timeoutMs);
    http.setTimeout(timeoutMs);
    if (url.startsWith("https://")) {
      secure.setCACertBundle(fm_ota_crt_bundle_start);
      return http.begin(secure, url);
    }
    return http.begin(plain, url);
  }
  WiFiClient& client(const String& url) {
    return url.startsWith("https://") ? static_cast<WiFiClient&>(secure) : plain;
  }
};

inline String macQuery() { return "?macAddress=" + WiFi.macAddress(); }

/** Heartbeat: activates a FACTORY_NEW device and refreshes "last seen". */
inline void sync(const String& base, uint32_t timeoutMs) {
  Http h;
  const String url = base + "/api/v1/device/sync" + macQuery();
  if (!h.begin(url, timeoutMs)) return;
  const int code = h.http.GET();
  h.http.end();
  if (code == 404) LOGW(TAG, "this board (MAC %s) is not registered in FirmwareManager", WiFi.macAddress().c_str());
  else if (code == 403) LOGW(TAG, "this device is suspended in FirmwareManager");
  else LOGD(TAG, "sync %s -> %d", base.c_str(), code);
}

/**
 * Latest version for this device from one server. Returns true + fills `out`
 * on a usable answer; `reachable` tells the caller whether the server replied.
 */
inline bool fetchLatest(const String& base, uint32_t timeoutMs, String& out, bool& reachable) {
  reachable = false;
  Http h;
  const String url = base + "/api/v1/device/firmware/version" + macQuery();
  if (!h.begin(url, timeoutMs)) {
    LOGW(TAG, "invalid version URL: %s", url.c_str());
    return false;
  }
  const uint32_t t0 = millis();
  const int code = h.http.GET();
  LOGD(TAG, "GET %s -> %d in %lu ms", url.c_str(), code, static_cast<unsigned long>(millis() - t0));
  if (code <= 0) {
    LOGW(TAG, "%s unreachable: %s", base.c_str(), HTTPClient::errorToString(code).c_str());
    h.http.end();
    return false;
  }
  reachable = true;
  const String body = h.http.getString();
  h.http.end();
  if (code == 404) {
    // Unregistered board, or no build uploaded / released for it yet.
    LOGI(TAG, "no firmware for this device on %s (%s)", base.c_str(), body.c_str());
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

/** Download + flash from `base`. Returns only on failure (reboots on success). */
inline bool install(const String& base, uint32_t timeoutMs) {
  const String url = base + "/api/v1/device/firmware" + macQuery();
  LOGI(TAG, "updating %s -> %s from %s", St().current.c_str(), St().latest.c_str(), base.c_str());
  St().status = Status::DOWNLOADING;
  St().progress = 0;
  if (St().beforeFlash) St().beforeFlash();

  Http h;
  if (url.startsWith("https://")) h.secure.setCACertBundle(fm_ota_crt_bundle_start);
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.onProgress([](int done, int total) {
    const int pct = total > 0 ? static_cast<int>(static_cast<int64_t>(done) * 100 / total) : 0;
    if (pct != St().progress) {
      St().progress = pct;
      if (St().onProgress) St().onProgress(pct);
    }
  });

  RemoteLog::pause(true);  // give the download the radio; lines keep buffering
  const uint32_t t0 = millis();
  const t_httpUpdate_return r = httpUpdate.update(h.client(url), url);
  RemoteLog::pause(false);

  switch (r) {
    case HTTP_UPDATE_OK:
      saveVersion(St().latest);
      St().progress = 100;
      LOGI(TAG, "update to %s OK in %lus - rebooting", St().latest.c_str(),
           static_cast<unsigned long>((millis() - t0) / 1000));
      RemoteLog::flush(3000);  // get the result to the console before the reboot
      delay(500);
      ESP.restart();
      return true;  // not reached
    case HTTP_UPDATE_NO_UPDATES:
      LOGI(TAG, "server reported no update");
      St().status = Status::UP_TO_DATE;
      return true;
    case HTTP_UPDATE_FAILED:
    default:
      fail("update from " + base + " failed (" + String(httpUpdate.getLastError()) + "): " +
           httpUpdate.getLastErrorString());
      return false;
  }
}

}  // namespace detail

/** Running firmware version: max(NVS-stored, FW_VERSION). */
inline const String& currentVersion() {
  if (!St().current.length()) {
    const String stored = detail::loadVersion();
    St().current = (stored.length() && isNewer(FW_VERSION, stored)) ? stored : String(FW_VERSION);
  }
  return St().current;
}

inline Status status() { return St().status; }
inline const String& latestVersion() { return St().latest; }
inline const String& lastError() { return St().error; }
inline int progress() { return St().progress; }

/** Polled: return false while the device must not reboot (e.g. mid-task). */
inline void setCanUpdate(bool (*fn)()) { St().canUpdate = fn; }
/** Runs right before the download starts (show a screen, stop peripherals). */
inline void setBeforeFlash(void (*fn)()) { St().beforeFlash = fn; }
inline void setOnProgress(void (*fn)(int)) { St().onProgress = fn; }

/** Ask for a check (and install, if newer) on the next loop(). */
inline void requestCheck() { St().requested = true; }

/**
 * Blocking: check primary then fallback, install if a newer build exists.
 * Reboots on a successful update; otherwise returns with status() set.
 */
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
  if (!esp_ota_get_next_update_partition(nullptr)) {
    St().status = Status::NO_PARTITION;
    St().error = "no OTA partition: flash once over USB with a partition scheme that has OTA";
    LOGE(TAG, "%s", St().error.c_str());
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    detail::fail("Wi-Fi not connected");
    return;
  }

  St().status = Status::CHECKING;
  LOGI(TAG, "checking for updates (running %s)", currentVersion().c_str());

  struct Server {
    String base;
    uint32_t timeout;
  } servers[] = {{primary, FM_PRIMARY_TIMEOUT_MS}, {fallback, FM_FALLBACK_TIMEOUT_MS}};

  bool anyReachable = false;
  for (const Server& s : servers) {
    if (!s.base.length()) continue;
    bool reachable = false;
    String latest;
    if (!detail::fetchLatest(s.base, s.timeout, latest, reachable)) {
      anyReachable = anyReachable || reachable;
      continue;  // try the fallback
    }
    detail::sync(s.base, s.timeout);
    St().latest = latest;
    if (!latest.length() || !isNewer(currentVersion(), latest)) {
      St().status = Status::UP_TO_DATE;
      LOGI(TAG, "up to date (running %s, server %s)", currentVersion().c_str(),
           latest.length() ? latest.c_str() : "none");
      return;
    }
    if (detail::install(s.base, s.timeout)) return;
    // Download failed on this server: fall through to the next one.
  }
  if (St().status != Status::FAILED)
    detail::fail(anyReachable ? "version check failed (bad server response)" : "update server unreachable");
}

/** Call from loop(): runs the boot check, periodic checks and requests. */
inline void loop() {
  if (WiFi.status() != WL_CONNECTED) return;
  const uint32_t now = millis();
  // nextCheckAt == 0 means "no automatic check scheduled".
  auto at = [](uint32_t t) -> uint32_t { return t ? t : 1; };
  if (!St().scheduled) {
    St().scheduled = true;
    if (OTA_CHECK_ON_BOOT) St().nextCheckAt = at(now);
    else if ((OTA_CHECK_INTERVAL_MS) != 0UL) St().nextCheckAt = at(now + OTA_CHECK_INTERVAL_MS);
  }
  const bool due = St().requested || (St().nextCheckAt && static_cast<int32_t>(now - St().nextCheckAt) >= 0);
  if (!due) return;
  if (St().canUpdate && !St().canUpdate()) return;  // busy: re-checked next loop
  St().requested = false;

  checkNow();  // reboots if it installed an update

  const uint32_t after = millis();
  if (St().status == Status::FAILED) St().nextCheckAt = at(after + OTA_RETRY_INTERVAL_MS);
  else St().nextCheckAt = (OTA_CHECK_INTERVAL_MS) != 0UL ? at(after + OTA_CHECK_INTERVAL_MS) : 0;
}

}  // namespace OtaUpdates
