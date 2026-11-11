#pragma once
// ---------------------------------------------------------------------------
//  custom_mqtt_logging.h - remote logging to the FirmwareManager console over
//  MQTT (WebSocket), for Tank Monitor Device Model-1 v1 (TMM1V1).
//
//  Use the macros, never Serial.print, so every line can reach the console:
//    LOGE(tag, fmt, ...)  error : something failed and needs attention
//    LOGW(tag, fmt, ...)  warn  : degraded / unexpected but handled
//    LOGI(tag, fmt, ...)  info  : lifecycle + user-visible events
//    LOGD(tag, fmt, ...)  debug : detail for troubleshooting
//  Never log secrets (Wi-Fi passwords, tokens).
//
//  Which levels are SENT is decided by the console, not the device:
//    - each channel (remote = console, serial = USB) has a SET of enabled
//      levels (any combination, e.g. only ERROR + DEBUG; empty = off);
//    - FirmwareManager -> Devices -> Logging publishes it as a retained
//      command on fm/<mac>/cmd; the device applies it, stores it in NVS and
//      confirms on fm/<mac>/status;
//    - a line whose level is disabled on both channels is dropped before it
//      is formatted, so DEBUG costs no bandwidth until you enable it.
//
//  Wire protocol (shared by every model):
//    fm/<mac>/log     device -> server  batched lines, QoS 1
//    fm/<mac>/status  device -> server  retained {"state","fw","remote","serial"}; LWT offline
//    fm/<mac>/cmd     server -> device  retained {"remote":[...],"serial":[...]}
//    fm/<mac>/ping    device -> server  liveness "1" every 20 s
//    fm/<mac>/ctl     server -> device  one-shot controls, NOT retained: {"action":"restart","id":"..."}
//  The status also advertises "caps":["restart"], which enables the
//  console's Restart button for this device.
//  <mac> = Wi-Fi STA MAC, 12 lowercase hex digits. Client id "fm-<mac>".
//  The board must be registered in Devices with that MAC, or the broker
//  refuses the login.
//
//  Connection supervision runs in its own task: liveness ping, reconnect on
//  silence, client rebuild after 2 min offline, and (optional) a last-resort
//  restart after 10 min offline with Wi-Fi up. Lines logged while offline are
//  buffered (REMOTE_LOG_BUFFER_BYTES, PSRAM when present) and sent later.
//
//  Transport: ESP-IDF's built-in esp-mqtt (no extra library); wss:// is
//  verified against the core's CA bundle. arduino-esp32 2.0.x and 3.x.
//  HEADER-ONLY. Depends only on ../config/globals.h.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <stdarg.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#if __has_include(<esp_mac.h>)
#include <esp_mac.h>   // esp_read_mac (IDF 4.4+ / 5.x)
#else
#include <esp_system.h>
#endif
#include <mqtt_client.h>
#include "../config/globals.h"

// The IDF CA bundle compiled into the core (declared directly: WiFiClientSecure
// ships its own esp_crt_bundle.h that can shadow the IDF one).
extern "C" esp_err_t esp_crt_bundle_attach(void* conf);

namespace RemoteLog {

enum Level : uint8_t { OFF = 0, ERROR = 1, WARN = 2, INFO = 3, DEBUG = 4 };

static constexpr uint8_t bitOf(Level l) {
  return (l >= ERROR && l <= DEBUG) ? static_cast<uint8_t>(1u << (l - 1)) : 0;
}
static constexpr uint8_t kAllLevels = 0x0F;

// ----------------------------------------------------------------------------
// Tunables (globals.h)
// ----------------------------------------------------------------------------
static constexpr size_t   kMaxLine        = REMOTE_LOG_LINE_MAX;     // server accepts up to 1024
static constexpr size_t   kBatchBytes     = REMOTE_LOG_BATCH_BYTES;  // one MQTT publish
static constexpr size_t   kBufBytes       = REMOTE_LOG_BUFFER_BYTES; // backlog (PSRAM)
static constexpr uint32_t kFlushMs        = REMOTE_LOG_FLUSH_MS;     // max delay before a batch is sent
static constexpr uint8_t  kDefaultRemote  = LOG_REMOTE_LEVELS_DEFAULT & kAllLevels;
static constexpr uint8_t  kDefaultSerial  = LOG_SERIAL_LEVELS_DEFAULT & kAllLevels;
static_assert(kMaxLine < kBatchBytes, "a single line must fit in one batch");
static_assert(kBatchBytes <= kBufBytes, "buffer must hold at least one batch");

static constexpr int      kKeepaliveSec     = 30;
static constexpr uint32_t kPingMs           = 20000;   // liveness ping while connected
static constexpr uint32_t kSilentMs         = 45000;   // nothing heard -> reconnect
static constexpr uint32_t kReconnectRetryMs = 5000;    // keep retrying reconnect() after a kick
static constexpr uint32_t kRebuildMs        = 120000;  // offline (Wi-Fi up) -> rebuild client
static constexpr uint32_t kRestartMs        = 600000;  // offline (Wi-Fi up) -> ESP.restart()

// ----------------------------------------------------------------------------
// State. A function-local static in an inline function is one instance
// program-wide (header-only, C++11-safe).
// ----------------------------------------------------------------------------
struct State {
  esp_mqtt_client_handle_t client = nullptr;
  volatile bool connected = false;
  volatile bool paused = false;              // pause(true) from the app
  volatile bool hookPaused = false;          // pause hook result, refreshed by the flush task
  volatile bool force = false;               // flush(): send now, ignore batching delay
  volatile bool sending = false;             // a batch was taken and is being published
  volatile uint8_t remote = kDefaultRemote;  // level bitmask
  volatile uint8_t serial = kDefaultSerial;  // level bitmask
  bool (*pauseHook)() = nullptr;
  bool (*restartGuard)() = nullptr;
  SemaphoreHandle_t lock = nullptr;
  TaskHandle_t task = nullptr;
  char* buf = nullptr;                       // kBufBytes, PSRAM when available
  size_t len = 0;
  volatile uint32_t lines = 0;               // lines currently buffered
  uint32_t firstAt = 0;                      // millis() of the oldest buffered line
  uint32_t dropped = 0;                      // dropped since the last report (sent to server)
  volatile uint32_t droppedTotal = 0;
  volatile uint32_t sent = 0;                // lines handed to esp-mqtt
  char id[13] = "";
  char clientId[16];
  char tLog[24], tStatus[24], tCmd[24], tPing[24], tCtl[24];
  // Remote restart requested on fm/<mac>/ctl; carried out by MqttLogging::loop().
  volatile bool restartRequested = false;
  char restartId[24] = "";
  char fw[33];

  // Kept for client rebuilds.
  esp_mqtt_client_config_t cfg;
  char uri[160], user[48], pass[64];
  const char* rootCa = nullptr;

  // URI change requested by setUri(); applied by the flush task.
  volatile bool uriPending = false;
  char newUri[160];

  // Supervision; written by the flush task unless marked volatile.
  volatile uint32_t lastHeard = 0;
  volatile bool gotIp = false;
  volatile bool everConnected = false;
  volatile bool refused = false;
  uint32_t lastPing = 0;
  bool offline = false;
  uint32_t offlineSince = 0;
  uint32_t rebuildAt = 0;
  bool reconnectPending = false;
  uint32_t reconnectUntil = 0;

  // Last MQTT_EVENT_ERROR: captured by the handler, printed by the flush task.
  volatile bool errPending = false;
  volatile uint32_t errCount = 0;
  int errType = 0, errSock = 0, errRc = 0;
  esp_err_t errTls = ESP_OK;
};
inline State& S() {
  static State s;
  return s;
}

inline const char* levelName(Level l) {
  switch (l) {
    case OFF:   return "off";
    case ERROR: return "error";
    case WARN:  return "warn";
    case DEBUG: return "debug";
    default:    return "info";
  }
}
inline char levelLetter(Level l) {
  switch (l) {
    case ERROR: return 'E';
    case WARN:  return 'W';
    case DEBUG: return 'D';
    default:    return 'I';
  }
}

inline void log(Level level, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

// ----------------------------------------------------------------------------
// Internals
// ----------------------------------------------------------------------------
namespace detail {

/** Match a level name at p (followed by a closing quote); 0 if none. */
inline uint8_t nameBit(const char* p) {
  for (uint8_t i = ERROR; i <= DEBUG; ++i) {
    const char* name = levelName(static_cast<Level>(i));
    size_t n = strlen(name);
    if (strncmp(p, name, n) == 0 && p[n] == '"') return bitOf(static_cast<Level>(i));
  }
  return 0;
}

/**
 * Read "key" from a tiny flat JSON object as a level bitmask:
 *   "key":["error","debug"]  -> any combination ([] = off)
 *   "key":"info"             -> legacy threshold (error+warn+info)
 */
inline bool jsonLevels(const char* json, const char* key, uint8_t& out) {
  char pat[16];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char* p = strstr(json, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  ++p;
  while (*p == ' ') ++p;
  if (*p == '[') {
    const char* end = strchr(p, ']');
    if (!end) return false;
    uint8_t mask = 0;
    for (const char* q = strchr(p, '"'); q && q < end; q = strchr(q + 1, '"')) {
      mask |= nameBit(q + 1);
      q = strchr(q + 1, '"');  // skip to this name's closing quote
      if (!q) break;
    }
    out = mask;
    return true;
  }
  if (*p == '"') {
    ++p;
    if (strncmp(p, "off\"", 4) == 0) { out = 0; return true; }
    uint8_t b = nameBit(p);
    if (!b) return false;
    out = static_cast<uint8_t>((b << 1) - 1);  // threshold: this level and every more severe one
    return true;
  }
  return false;
}

/** Bitmask -> JSON array, e.g. ["error","info"]. */
inline int levelsJson(char* out, size_t cap, uint8_t mask) {
  int n = snprintf(out, cap, "[");
  for (uint8_t i = ERROR; i <= DEBUG; ++i) {
    if (!(mask & bitOf(static_cast<Level>(i)))) continue;
    n += snprintf(out + n, cap - n, "%s\"%s\"", n > 1 ? "," : "", levelName(static_cast<Level>(i)));
  }
  n += snprintf(out + n, cap - n, "]");
  return n;
}

/** Retained status: the console shows these as the device's applied levels. */
inline void publishStatus(esp_mqtt_client_handle_t c) {
  if (!c) return;
  char r[48], s[48], json[256];
  levelsJson(r, sizeof r, S().remote);
  levelsJson(s, sizeof s, S().serial);
  int n = snprintf(json, sizeof json, "{\"state\":\"online\",\"fw\":\"%s\",\"remote\":%s,\"serial\":%s,\"caps\":[\"restart\"]}",
                   S().fw, r, s);
  // enqueue = non-blocking, safe from any task including the MQTT event handler.
  esp_mqtt_client_enqueue(c, S().tStatus, json, n, 1, 1, true);
}

inline void saveLevels() {
  Preferences p;
  if (p.begin("rlog", false)) {
    p.putUChar("rmask", S().remote);
    p.putUChar("smask", S().serial);
    p.end();
  }
}

/** Loads levels and takes (reads + clears) the reason saved before a watchdog restart. */
inline void loadLevels(char* restartNote, size_t noteCap) {
  restartNote[0] = '\0';
  Preferences p;
  if (p.begin("rlog", false)) {   // read-write: creates the namespace on first boot
    S().remote = p.getUChar("rmask", kDefaultRemote) & kAllLevels;
    S().serial = p.getUChar("smask", kDefaultSerial) & kAllLevels;
    if (p.isKey("rst")) {
      p.getString("rst", restartNote, noteCap);
      p.remove("rst");
    }
    p.end();
  }
}

inline void saveRestartNote(const char* note) {
  Preferences p;
  if (p.begin("rlog", false)) {
    p.putString("rst", note);
    p.end();
  }
}

/** {"remote":["error","warn"],"serial":[]} - keys optional. Empty = cleared, ignore. */
inline void applyCommand(esp_mqtt_client_handle_t c, const char* json) {
  uint8_t r = S().remote, s = S().serial;
  const bool hasRemote = jsonLevels(json, "remote", r);   // no short-circuit:
  const bool hasSerial = jsonLevels(json, "serial", s);   // either may be present
  if (!hasRemote && !hasSerial) return;
  bool changed = (r != S().remote) || (s != S().serial);
  S().remote = r;
  S().serial = s;
  if (changed) saveLevels();
  publishStatus(c);  // always confirm, so the console shows "synced"
}

/** Runs in the esp-mqtt task: cheap bookkeeping only. Uses the event's own client handle. */
inline void onEvent(void*, esp_event_base_t, int32_t id, void* data) {
  auto* e = static_cast<esp_mqtt_event_handle_t>(data);
  switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
      S().connected = true;
      S().everConnected = true;
      S().refused = false;
      S().lastHeard = millis();
#if ESP_IDF_VERSION_MAJOR >= 5
      esp_mqtt_client_subscribe_single(e->client, S().tCmd, 1);
      esp_mqtt_client_subscribe_single(e->client, S().tCtl, 1);
#else
      esp_mqtt_client_subscribe(e->client, S().tCmd, 1);
      esp_mqtt_client_subscribe(e->client, S().tCtl, 1);
#endif
      publishStatus(e->client);
      if (S().task) xTaskNotifyGive(S().task);  // send whatever piled up
      break;
    case MQTT_EVENT_DISCONNECTED:
      S().connected = false;
      if (S().task) xTaskNotifyGive(S().task);
      break;
    case MQTT_EVENT_PUBLISHED:
    case MQTT_EVENT_SUBSCRIBED:
      S().lastHeard = millis();
      break;
    case MQTT_EVENT_DATA: {
      S().lastHeard = millis();
      // One-shot control (never retained). Only flag it here: the restart
      // itself runs on the app task so logs can be flushed first.
      const size_t cl = strlen(S().tCtl);
      if (e->topic_len == static_cast<int>(cl) && strncmp(e->topic, S().tCtl, cl) == 0 &&
          e->current_data_offset == 0 && e->data_len == e->total_data_len && e->data_len > 0 &&
          e->data_len < 128) {
        char ctl[128];
        memcpy(ctl, e->data, e->data_len);
        ctl[e->data_len] = '\0';
        if (strstr(ctl, "\"action\":\"restart\"")) {
          S().restartId[0] = '\0';
          const char* id = strstr(ctl, "\"id\":\"");
          if (id) {
            id += 6;
            size_t n = 0;
            while (id[n] && id[n] != '"' && n < sizeof S().restartId - 1) ++n;
            memcpy(S().restartId, id, n);
            S().restartId[n] = '\0';
          }
          S().restartRequested = true;
        }
        break;
      }
      // Commands are tiny (server caps them at 96 bytes) and never fragmented.
      size_t tl = strlen(S().tCmd);
      if (e->topic_len == static_cast<int>(tl) && strncmp(e->topic, S().tCmd, tl) == 0 &&
          e->current_data_offset == 0 && e->data_len == e->total_data_len && e->data_len > 0 &&
          e->data_len < 128) {
        char cmd[128];
        memcpy(cmd, e->data, e->data_len);
        cmd[e->data_len] = '\0';
        applyCommand(e->client, cmd);
      }
      break;
    }
    case MQTT_EVENT_ERROR:
      if (e->error_handle) {
        S().errType = static_cast<int>(e->error_handle->error_type);
        S().errTls  = e->error_handle->esp_tls_last_esp_err;
        S().errSock = e->error_handle->esp_transport_sock_errno;
        S().errRc   = static_cast<int>(e->error_handle->connect_return_code);
        S().refused = e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED;
      }
      ++S().errCount;
      S().errPending = true;
      if (S().task) xTaskNotifyGive(S().task);
      break;
    default:
      break;
  }
}

/** Arduino Wi-Fi event task: just flag it; the flush task does the reconnect. */
inline void onWifiGotIp(arduino_event_id_t) {
  S().gotIp = true;
  if (S().task) xTaskNotifyGive(S().task);
}

inline void printError() {
  S().errPending = false;
  // Expected while Wi-Fi is down (esp-mqtt keeps retrying): don't spam Serial.
  if (WiFi.status() != WL_CONNECTED) return;
  // Serial only: the link is down, and this repeats on every retry.
  Serial.printf("[E][RLOG] MQTT error #%u: type=%d tls_err=0x%x sock_errno=%d connect_rc=%d largest_free=%u\n",
                static_cast<unsigned>(S().errCount), S().errType, static_cast<unsigned>(S().errTls),
                S().errSock, S().errRc,
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

/** Fill S().cfg from S().uri / user / pass. */
inline void buildConfig() {
  static const char kOffline[] = "{\"state\":\"offline\"}";
  const bool tls = strncmp(S().uri, "wss://", 6) == 0 || strncmp(S().uri, "mqtts://", 8) == 0;
  esp_mqtt_client_config_t& cfg = S().cfg;
  cfg = esp_mqtt_client_config_t();
#if ESP_IDF_VERSION_MAJOR >= 5
  cfg.broker.address.uri = S().uri;
  if (tls) {
    if (S().rootCa) cfg.broker.verification.certificate = S().rootCa;
    else cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  }
  cfg.credentials.client_id = S().clientId;
  cfg.credentials.username = S().user;
  cfg.credentials.authentication.password = S().pass;
  cfg.session.last_will.topic = S().tStatus;
  cfg.session.last_will.msg = kOffline;
  cfg.session.last_will.msg_len = sizeof(kOffline) - 1;
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.session.keepalive = kKeepaliveSec;
  cfg.network.reconnect_timeout_ms = REMOTE_LOG_RETRY_MS;
  cfg.network.timeout_ms = 15000;
  cfg.buffer.size = 1536;
  cfg.buffer.out_size = kBatchBytes + 1024;
  cfg.task.priority = TASK_MQTT_PRIO;
  cfg.task.stack_size = TASK_MQTT_STACK;
#else
  cfg.uri = S().uri;
  if (tls) {
    if (S().rootCa) cfg.cert_pem = S().rootCa;
    else cfg.crt_bundle_attach = esp_crt_bundle_attach;
  }
  cfg.client_id = S().clientId;
  cfg.username = S().user;
  cfg.password = S().pass;
  cfg.lwt_topic = S().tStatus;
  cfg.lwt_msg = kOffline;
  cfg.lwt_msg_len = sizeof(kOffline) - 1;
  cfg.lwt_qos = 1;
  cfg.lwt_retain = 1;
  cfg.keepalive = kKeepaliveSec;
  cfg.reconnect_timeout_ms = REMOTE_LOG_RETRY_MS;
  cfg.network_timeout_ms = 15000;
  cfg.buffer_size = 1536;
  cfg.out_buffer_size = kBatchBytes + 1024;
  cfg.task_prio = TASK_MQTT_PRIO;
  cfg.task_stack = TASK_MQTT_STACK;
#endif
}

/** init + register + start with the stored config. Flush task / begin() only. */
inline bool startClient() {
  S().client = esp_mqtt_client_init(&S().cfg);
  if (!S().client) return false;
  esp_mqtt_client_register_event(S().client, MQTT_EVENT_ANY, onEvent, nullptr);
  return esp_mqtt_client_start(S().client) == ESP_OK;
}

inline void stopClient() {
  S().connected = false;
  S().reconnectPending = false;
  if (S().client) {
    esp_mqtt_client_handle_t old = S().client;
    S().client = nullptr;  // publishStatus() and friends skip a null client
    esp_mqtt_client_stop(old);
    esp_mqtt_client_destroy(old);
  }
  S().connected = false;  // the old task may have flipped it while stopping
  S().offline = false;
}

/** Full teardown + rebuild for a fresh transport / TLS state. Flush task only. */
inline void rebuildClient() {
  log(WARN, "RLOG", "MQTT offline %us with Wi-Fi up - rebuilding client (largest_free=%u)",
      static_cast<unsigned>((millis() - S().offlineSince) / 1000),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  stopClient();
  if (!startClient())
    log(ERROR, "RLOG", "MQTT client rebuild failed, retry in %us", static_cast<unsigned>(kRebuildMs / 1000));
}

/** setUri(): switch broker (backend host/port changed). Flush task only. */
inline void applyNewUri() {
  S().uriPending = false;
  if (strcmp(S().newUri, S().uri) == 0 && S().client) return;
  stopClient();
  snprintf(S().uri, sizeof S().uri, "%s", S().newUri);
  if (!S().uri[0]) {
    log(WARN, "RLOG", "Broker cleared - remote logging stopped (lines keep buffering)");
    return;
  }
  buildConfig();
  if (startClient()) log(INFO, "RLOG", "Broker changed -> %s", S().uri);
  else log(ERROR, "RLOG", "MQTT client start failed for %s", S().uri);
}

inline void kickReconnect(uint32_t now) {
  S().reconnectPending = true;
  S().reconnectUntil = now + kReconnectRetryMs;
}

inline bool isPaused() { return S().paused || (S().hookPaused && !S().force); }

/** Connection supervision, once per flush-task wake-up (<= every 250 ms). */
inline void supervise() {
  const uint32_t now = millis();
  S().hookPaused = S().pauseHook ? S().pauseHook() : false;
  if (S().uriPending) applyNewUri();
  if (S().errPending) printError();

  if (S().gotIp) {
    S().gotIp = false;
    if (S().client && !S().connected) kickReconnect(now);
  }

  // esp_mqtt_client_reconnect() returns ESP_FAIL until the client reached its
  // wait-reconnect state, so keep trying briefly (esp-mqtt also auto-retries).
  if (S().reconnectPending) {
    if (S().connected || (S().client && esp_mqtt_client_reconnect(S().client) == ESP_OK) ||
        static_cast<int32_t>(now - S().reconnectUntil) >= 0)
      S().reconnectPending = false;
  }

  if (isPaused() || !S().client) {
    // Paused time must not count as silence / offline time.
    S().lastHeard = now;
    S().offline = false;
    return;
  }

  if (S().connected) {
    S().offline = false;
    if (now - S().lastPing >= kPingMs) {          // 1. liveness ping
      S().lastPing = now;
      esp_mqtt_client_enqueue(S().client, S().tPing, "1", 1, 1, 0, true);
    }
    if (now - S().lastHeard >= kSilentMs) {
      log(WARN, "RLOG", "No reply from server for %us - forcing MQTT reconnect",
          static_cast<unsigned>((now - S().lastHeard) / 1000));
      S().connected = false;  // stop publishing into a dead link right away
      S().lastHeard = now;
      esp_mqtt_client_disconnect(S().client);
      kickReconnect(now);
    }
    return;
  }

  // Offline: only count time while Wi-Fi is actually up.
  if (WiFi.status() != WL_CONNECTED) { S().offline = false; return; }
  if (!S().offline) {
    S().offline = true;
    S().offlineSince = now;
    S().rebuildAt = now;
    return;
  }

  if (now - S().rebuildAt >= kRebuildMs) {        // 3. rebuild, then every 2 min
    S().rebuildAt = now;
    rebuildClient();
    return;
  }

#if REMOTE_LOG_WATCHDOG_RESTART
  // 6. Last resort; deferred (re-checked every tick) while the app is busy.
  if (now - S().offlineSince >= kRestartMs && S().everConnected && !S().refused &&
      (!S().restartGuard || S().restartGuard())) {
    char note[96];
    snprintf(note, sizeof note, "MQTT offline %us with Wi-Fi up (errors=%u, last type=%d sock_errno=%d)",
             static_cast<unsigned>((now - S().offlineSince) / 1000), static_cast<unsigned>(S().errCount),
             S().errType, S().errSock);
    saveRestartNote(note);
    log(ERROR, "RLOG", "Restarting: %s", note);
    Serial.flush();
    delay(100);
    ESP.restart();
  }
#endif
}

/**
 * Take up to kBatchBytes of complete lines from the front of the buffer.
 * Caller holds S().lock. Returns the byte count copied into out.
 */
inline size_t takeBatch(char* out, uint32_t& linesOut) {
  size_t n = S().len < kBatchBytes ? S().len : kBatchBytes;
  if (n < S().len) {                       // cut at the last full line that fits
    while (n > 0 && S().buf[n - 1] != '\n') --n;
    if (n == 0) n = S().len < kBatchBytes ? S().len : kBatchBytes;   // can't happen (kMaxLine < batch)
  }
  memcpy(out, S().buf, n);
  uint32_t lines = 0;
  for (size_t i = 0; i < n; ++i) if (out[i] == '\n') ++lines;
  memmove(S().buf, S().buf + n, S().len - n);
  S().len -= n;
  S().lines = S().lines > lines ? S().lines - lines : 0;
  if (S().len == 0) S().lines = 0;
  linesOut = lines;
  return n;
}

/** Sends buffered batches and supervises the connection. Never holds S().lock while calling into esp-mqtt. */
inline void flushTask(void*) {
  static char out[kBatchBytes + 1];
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    supervise();
    if (!S().connected || isPaused() || !S().client) continue;

    // Drain: several batches back to back if a backlog built up.
    for (int burst = 0; burst < 16 && S().connected && S().client && !detail::isPaused(); ++burst) {
      size_t n = 0;
      uint32_t lines = 0, dropped = 0;
      xSemaphoreTake(S().lock, portMAX_DELAY);
      bool due = S().len > 0 &&
                 (S().force || S().len >= kBatchBytes * 3 / 4 || millis() - S().firstAt >= kFlushMs);
      if (due) {
        n = takeBatch(out, lines);
        dropped = S().dropped;
        S().dropped = 0;
        S().sending = true;
      }
      xSemaphoreGive(S().lock);
      if (n == 0) break;

      if (out[n - 1] == '\n') --n;  // lines are joined by '\n'
      // publish() sends now; QoS 1 keeps it in the outbox until the server acks.
      if (esp_mqtt_client_publish(S().client, S().tLog, out, n, 1, 0) < 0)
        esp_mqtt_client_enqueue(S().client, S().tLog, out, n, 1, 0, true);
      S().sent = S().sent + lines;
      if (dropped) {
        char note[72];
        int m = snprintf(note, sizeof note, "[W][RLOG] %u log lines dropped (offline or buffer full)",
                         static_cast<unsigned>(dropped));
        esp_mqtt_client_publish(S().client, S().tLog, note, m, 1, 0);
      }
      S().sending = false;
      vTaskDelay(pdMS_TO_TICKS(20));   // give the MQTT task room between batches
    }
  }
}

/** Append one line ("[I][TAG] text\n"); oldest lines go first if it's full. */
inline void enqueueLine(const char* line, size_t len) {
  if (!S().lock || !S().buf || len == 0) return;
  xSemaphoreTake(S().lock, portMAX_DELAY);
  while (S().len > 0 && S().len + len + 1 > kBufBytes) {
    char* nl = static_cast<char*>(memchr(S().buf, '\n', S().len));
    size_t cut = nl ? static_cast<size_t>(nl - S().buf) + 1 : S().len;
    memmove(S().buf, S().buf + cut, S().len - cut);
    S().len -= cut;
    if (S().lines) S().lines = S().lines - 1;
    ++S().dropped;
    S().droppedTotal = S().droppedTotal + 1;
  }
  if (S().len == 0) S().firstAt = millis();
  memcpy(S().buf + S().len, line, len);
  S().len += len;
  S().buf[S().len++] = '\n';
  S().lines = S().lines + 1;
  bool full = S().len >= kBatchBytes * 3 / 4;
  xSemaphoreGive(S().lock);
  if (full && S().task) xTaskNotifyGive(S().task);
}

}  // namespace detail

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

/**
 * Load saved levels and start buffering. Call first thing in setup() (after
 * Serial.begin) so boot lines reach the console once MQTT connects.
 */
inline void init() {
  if (S().lock) return;
  char note[96];
  detail::loadLevels(note, sizeof note);
  S().buf = static_cast<char*>(heap_caps_malloc(kBufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!S().buf) S().buf = static_cast<char*>(malloc(kBufBytes));
  S().lock = xSemaphoreCreateMutex();
  if (!S().buf) Serial.println("[E][RLOG] no memory for the remote log buffer - remote logging disabled");
  if (note[0]) log(WARN, "RLOG", "Previous restart by RemoteLog watchdog: %s", note);
}

/**
 * Start the MQTT client (calling again is a no-op; use setUri() to switch
 * broker). Wi-Fi must be started (WiFiMgr.begin()). The client reconnects by
 * itself after Wi-Fi or server drops. Strings are copied; rootCaPem (optional
 * PEM; nullptr = built-in CA bundle) must stay valid for the program's life.
 */
inline bool begin(const char* uri, const char* username, const char* password, const char* fwVersion,
                  const char* rootCaPem = nullptr) {
  if (S().task) return true;
  init();
  if (!S().lock) return false;

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);   // valid before Wi-Fi connects (unlike WiFi.macAddress())
  snprintf(S().id, sizeof S().id, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(S().clientId, sizeof S().clientId, "fm-%s", S().id);
  snprintf(S().tLog, sizeof S().tLog, "fm/%s/log", S().id);
  snprintf(S().tStatus, sizeof S().tStatus, "fm/%s/status", S().id);
  snprintf(S().tCmd, sizeof S().tCmd, "fm/%s/cmd", S().id);
  snprintf(S().tPing, sizeof S().tPing, "fm/%s/ping", S().id);
  snprintf(S().tCtl, sizeof S().tCtl, "fm/%s/ctl", S().id);
  snprintf(S().fw, sizeof S().fw, "%s", fwVersion ? fwVersion : "");
  snprintf(S().uri, sizeof S().uri, "%s", uri ? uri : "");
  snprintf(S().user, sizeof S().user, "%s", username ? username : "");
  snprintf(S().pass, sizeof S().pass, "%s", password ? password : "");
  S().rootCa = rootCaPem;

  WiFi.onEvent(detail::onWifiGotIp, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  bool ok = true;
  if (S().uri[0]) {
    detail::buildConfig();
    ok = detail::startClient();   // before the task, so it never sees a half-built handle
  }
  xTaskCreatePinnedToCore(detail::flushTask, "remote_log", TASK_REMOTE_LOG_STACK, nullptr,
                          TASK_REMOTE_LOG_PRIO, &S().task, TASK_REMOTE_LOG_CORE);
  return ok && S().task;
}

/** Switch broker at runtime ("" = stop). Applied by the flush task. */
inline void setUri(const char* uri) {
  snprintf(S().newUri, sizeof S().newUri, "%s", uri ? uri : "");
  S().uriPending = true;
  if (S().task) xTaskNotifyGive(S().task);
}
inline const char* uri() { return S().uri; }

/** Polled predicate: true = hold sending (lines keep buffering). */
inline void setPauseHook(bool (*hook)()) { S().pauseHook = hook; }
/** Polled predicate: false = defer the watchdog restart (device busy). */
inline void setRestartGuard(bool (*guard)()) { S().restartGuard = guard; }

/** Pause sending during heavy work (OTA, big uploads). Lines keep buffering. */
inline void pause(bool paused) {
  if (!paused) {
    S().lastHeard = millis();
    S().offline = false;
  }
  S().paused = paused;
  if (!paused && S().task) xTaskNotifyGive(S().task);
}

/**
 * Send everything buffered now and wait (up to timeoutMs, including time to
 * connect) until it was handed to the socket. Use right before ESP.restart().
 */
inline bool flush(uint32_t timeoutMs = 3000) {
  if (!S().client || !S().task) return false;
  pause(false);
  S().force = true;
  const uint32_t start = millis();
  bool done = false;
  while (millis() - start < timeoutMs) {
    xTaskNotifyGive(S().task);
    if (S().connected && S().len == 0 && !S().sending) { done = true; break; }
    delay(20);
  }
  S().force = false;
  if (done) delay(150);  // let lwIP put the last frames on the air
  return done;
}

inline bool connected() { return S().connected; }
inline bool started() { return S().task != nullptr; }
inline bool paused() { return detail::isPaused(); }
inline uint8_t remoteLevels() { return S().remote; }
inline uint8_t serialLevels() { return S().serial; }
inline const char* deviceId() { return S().id; }
inline uint32_t sent() { return S().sent; }
inline uint32_t queued() { return S().lines; }
inline uint32_t dropped() { return S().droppedTotal; }
/** Take (read + clear) a pending remote restart request. `id` gets the request id. */
inline bool takeRestartRequest(char* id = nullptr, size_t cap = 0) {
  if (!S().restartRequested) return false;
  S().restartRequested = false;
  if (id && cap) snprintf(id, cap, "%s", S().restartId);
  return true;
}
inline unsigned taskStackFree() {
  return S().task ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(S().task)) * sizeof(StackType_t) : 0;
}
/** Human-readable reason the link is down ("" when connected). */
inline const char* lastError() {
  if (S().connected) return "";
  if (!S().task) return "not started";
  if (!S().uri[0]) return "server host not set";
  if (WiFi.status() != WL_CONNECTED) return "Wi-Fi not connected";
  if (S().refused) return "broker refused login (check MQTT_USERNAME / MQTT_PASSWORD and that this MAC is registered)";
  if (S().errCount) return "broker unreachable";
  return "connecting";
}

/** Bitmask -> "error,warn" style list for the UI ("" = off). */
inline String levelsList(uint8_t mask) {
  String s;
  for (uint8_t i = ERROR; i <= DEBUG; ++i) {
    if (!(mask & bitOf(static_cast<Level>(i)))) continue;
    if (s.length()) s += ',';
    s += levelName(static_cast<Level>(i));
  }
  return s;
}

/** Core logger. Prefer the LOGx / RLOGx macros. */
inline void logv(Level level, const char* tag, const char* fmt, va_list ap) {
  const uint8_t bit = bitOf(level);
  if (!bit) return;
  const bool toSerial = (S().serial & bit) != 0;
  const bool toRemote = (S().remote & bit) != 0 && S().lock;  // buffered even before begin()
  if (!toSerial && !toRemote) return;

  char line[kMaxLine];
  int head = snprintf(line, sizeof line, "[%c][%s] ", levelLetter(level), tag ? tag : "APP");
  if (head < 0) return;
  if (head >= static_cast<int>(sizeof line)) head = sizeof line - 1;
  int body = vsnprintf(line + head, sizeof line - head, fmt, ap);
  size_t len = static_cast<size_t>(head) + (body > 0 ? static_cast<size_t>(body) : 0);
  if (len > sizeof line - 1) len = sizeof line - 1;  // truncated
  for (size_t i = 0; i < len; ++i)
    if (line[i] == '\n' || line[i] == '\r') line[i] = ' ';  // one line per entry

  if (toSerial) Serial.printf("%.*s\n", static_cast<int>(len), line);
  if (toRemote) detail::enqueueLine(line, len);
}

inline void log(Level level, const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  logv(level, tag, fmt, ap);
  va_end(ap);
}

}  // namespace RemoteLog

#define RLOGE(tag, fmt, ...) RemoteLog::log(RemoteLog::ERROR, tag, fmt, ##__VA_ARGS__)
#define RLOGW(tag, fmt, ...) RemoteLog::log(RemoteLog::WARN, tag, fmt, ##__VA_ARGS__)
#define RLOGI(tag, fmt, ...) RemoteLog::log(RemoteLog::INFO, tag, fmt, ##__VA_ARGS__)
#define RLOGD(tag, fmt, ...) RemoteLog::log(RemoteLog::DEBUG, tag, fmt, ##__VA_ARGS__)

// ----------------------------------------------------------------------------
// App-facing logging macros (LOG_LEVEL in globals.h is the compile-time ceiling)
// ----------------------------------------------------------------------------
#define LOGE(tag, fmt, ...) do { if (LOG_LEVEL >= 1) RLOGE(tag, fmt, ##__VA_ARGS__); } while (0)
#define LOGW(tag, fmt, ...) do { if (LOG_LEVEL >= 2) RLOGW(tag, fmt, ##__VA_ARGS__); } while (0)
#define LOGI(tag, fmt, ...) do { if (LOG_LEVEL >= 3) RLOGI(tag, fmt, ##__VA_ARGS__); } while (0)
#define LOGD(tag, fmt, ...) do { if (LOG_LEVEL >= 4) RLOGD(tag, fmt, ##__VA_ARGS__); } while (0)

// ----------------------------------------------------------------------------
// FirmwareManager backend URLs (shared with ota_updates.h)
// ----------------------------------------------------------------------------
namespace FmServer {

/** FM_PRIMARY_BASE_URL without trailing slashes ("" = not set). */
inline String primaryBase() {
  String b = FM_PRIMARY_BASE_URL;
  b.trim();
  while (b.endsWith("/")) b.remove(b.length() - 1);
  return b;
}

/** Fallback base: https://host when FM_FALLBACK_PORT is 443, else http://host:port. */
inline String fallbackBase() {
  String h = FM_FALLBACK_HOST;
  h.trim();
  if (h.startsWith("http://")) h = h.substring(7);
  if (h.startsWith("https://")) h = h.substring(8);
  while (h.endsWith("/")) h.remove(h.length() - 1);
  if (!h.length()) return "";
  const uint16_t port = FM_FALLBACK_PORT;
  if (port == 443) return "https://" + h;
  return "http://" + h + ":" + String(port);
}

/** http(s)://host[:port] -> ws(s)://host[:port]/mqtt. */
inline String brokerFor(const String& base) {
  if (base.startsWith("https://")) return "wss://" + base.substring(8) + "/mqtt";
  if (base.startsWith("http://")) return "ws://" + base.substring(7) + "/mqtt";
  return "";
}

}  // namespace FmServer

// ----------------------------------------------------------------------------
// MqttLogging - starts RemoteLog once Wi-Fi is up and fails over between the
// primary and fallback brokers. Call MqttLogging::loop() from loop().
// ----------------------------------------------------------------------------
namespace MqttLogging {

struct Brokers {
  String uri[2];
  int count = 0;
};

/** MQTT_BROKER_URI if pinned; otherwise primary then fallback backend. */
inline Brokers brokers() {
  Brokers b;
  if (MQTT_BROKER_URI[0]) {
    b.uri[b.count++] = MQTT_BROKER_URI;
    return b;
  }
  String p = FmServer::brokerFor(FmServer::primaryBase());
  String f = FmServer::brokerFor(FmServer::fallbackBase());
  if (p.length()) b.uri[b.count++] = p;
  if (f.length() && f != p) b.uri[b.count++] = f;
  return b;
}

struct State {
  void (*beforeRestart)() = nullptr;
  const char* fw = FW_VERSION;
  int index = 0;            // broker currently in use
  uint32_t switchedAt = 0;  // millis() of the last start / switch
  uint32_t checkedAt = 0;
};
inline State& St() {
  static State s;
  return s;
}

/** Remember the running firmware version (reported on fm/<mac>/status). */
inline void setFirmwareVersion(const char* fw) { St().fw = fw; }

/**
 * Runs right before a console-requested restart (save state, stop motors,
 * close files). Keep it short: the device reboots right after it returns.
 */
inline void setBeforeRestart(void (*fn)()) { St().beforeRestart = fn; }

/** Remote restart from the console (Devices -> device -> Restart). */
inline void serviceRestart() {
  char id[24];
  if (!RemoteLog::takeRestartRequest(id, sizeof id)) return;
  LOGW("SYS", "restart requested from FirmwareManager console (request %s) - rebooting", id[0] ? id : "?");
  if (St().beforeRestart) St().beforeRestart();
  RemoteLog::flush(3000);  // get the line to the console before the link drops
  delay(200);
  ESP.restart();
}

/** Cheap; call every loop(). Does real work at most every 2 s. */
inline void loop() {
  serviceRestart();
  const uint32_t now = millis();
  if (St().checkedAt && now - St().checkedAt < 2000) return;
  St().checkedAt = now ? now : 1;

  Brokers b = brokers();
  if (!b.count) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      LOGW("RLOG", "remote logging idle: set FM_PRIMARY_BASE_URL or FM_FALLBACK_HOST in globals.h");
    }
    return;
  }

  if (!RemoteLog::started()) {
    if (WiFi.status() != WL_CONNECTED) return;
    if (RemoteLog::begin(b.uri[0].c_str(), MQTT_USERNAME, MQTT_PASSWORD, St().fw)) {
      LOGI("RLOG", "remote logging started id=%s broker=%s console=[%s] serial=[%s]", RemoteLog::deviceId(),
           b.uri[0].c_str(), RemoteLog::levelsList(RemoteLog::remoteLevels()).c_str(),
           RemoteLog::levelsList(RemoteLog::serialLevels()).c_str());
    } else {
      LOGE("RLOG", "remote logging failed to start (broker=%s)", b.uri[0].c_str());
    }
    St().index = 0;
    St().switchedAt = now;
    return;
  }

  if (b.count < 2) return;  // nothing to fail over to; esp-mqtt keeps retrying

  if (RemoteLog::connected()) {
    // On the fallback: periodically try to move back to the primary.
    if (St().index == 1 && now - St().switchedAt >= REMOTE_LOG_PREFER_RETRY_MS) {
      LOGI("RLOG", "retrying primary broker %s", b.uri[0].c_str());
      RemoteLog::setUri(b.uri[0].c_str());
      St().index = 0;
      St().switchedAt = now;
    }
    return;
  }

  if (WiFi.status() == WL_CONNECTED && now - St().switchedAt >= REMOTE_LOG_FALLBACK_MS) {
    const int next = St().index ^ 1;
    LOGW("RLOG", "broker %s unreachable for %lus; switching to %s", b.uri[St().index].c_str(),
         static_cast<unsigned long>((now - St().switchedAt) / 1000), b.uri[next].c_str());
    RemoteLog::setUri(b.uri[next].c_str());
    St().index = next;
    St().switchedAt = now;
  }
}

}  // namespace MqttLogging
