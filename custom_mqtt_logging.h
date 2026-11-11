/*
 * custom_mqtt_logging.h — Remote logging to the FirmwareManager console
 * =======================================================================
 * ESP8266 port of the FirmwareManager logging module (MQTT 3.1.1 over
 * WebSocket, built on the "WebSockets" library by Markus Sattler).
 *
 * Use the macros, never Serial.print, so every line can reach the console:
 *   LOGE(tag, fmt, ...)  error : something failed and needs attention
 *   LOGW(tag, fmt, ...)  warn  : degraded / unexpected but handled
 *   LOGI(tag, fmt, ...)  info  : lifecycle + user-visible events
 *   LOGD(tag, fmt, ...)  debug : detail for troubleshooting
 * Never log secrets (Wi-Fi passwords, tokens).
 *
 * Which levels are SENT is decided by the console (Devices -> Logging):
 * it publishes a retained command on fm/<mac>/cmd, the device applies it,
 * stores it in EEPROM and confirms on fm/<mac>/status. A line whose level is
 * disabled on both channels is dropped before it is formatted.
 *
 * Wire protocol (shared by every FirmwareManager model):
 *   fm/<mac>/log     device -> server  batched lines, QoS 1
 *   fm/<mac>/status  device -> server  retained {"state","fw","remote","serial","caps"}; LWT offline
 *   fm/<mac>/cmd     server -> device  retained {"remote":[...],"serial":[...]}
 *   fm/<mac>/ping    device -> server  liveness "1" every 20 s
 *   fm/<mac>/ctl     server -> device  one-shot {"action":"restart","id":"..."}
 * <mac> = Wi-Fi STA MAC, 12 lowercase hex digits. Client id "fm-<mac>".
 *
 * ESP8266 notes:
 *  - Single-threaded: call MqttLogging::loop() from loop(). Nothing here
 *    blocks except the WebSocket connect (TCP + TLS handshake).
 *  - A wss:// connection holds ~20 KB of heap for TLS. Before any other
 *    HTTPS request (OTA check) call RemoteLog::suspend(), which
 *    sends pending lines and closes the connection; RemoteLog::resume()
 *    reconnects. Two TLS sessions at once do not fit in ESP8266 RAM.
 *  - The TLS certificate is NOT verified (setInsecure): the ESP8266 has no
 *    CA bundle and Render rotates certificates.
 *  - Lines logged while offline are kept in a RAM buffer and sent later.
 *    A batch is only removed from the buffer once the server acks it.
 */

#ifndef CUSTOM_MQTT_LOGGING_H
#define CUSTOM_MQTT_LOGGING_H

#include "diagnostics.h"
#include "globals.h"
#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <WebSocketsClient.h>
#include <stdarg.h>

namespace RemoteLog {

// Names are prefixed: ESP8266 / library headers define plain ERROR / DEBUG.
enum Level : uint8_t { LV_OFF = 0, LV_ERROR = 1, LV_WARN = 2, LV_INFO = 3, LV_DEBUG = 4 };

static constexpr uint8_t bitOf(Level l) {
  return (l >= LV_ERROR && l <= LV_DEBUG) ? static_cast<uint8_t>(1u << (l - 1)) : 0;
}
static constexpr uint8_t kAllLevels = 0x0F;

static constexpr size_t kMaxLine = REMOTE_LOG_LINE_MAX;
static constexpr size_t kBatchBytes = REMOTE_LOG_BATCH_BYTES;
static constexpr size_t kBufBytes = REMOTE_LOG_BUFFER_BYTES;
static constexpr uint32_t kFlushMs = REMOTE_LOG_FLUSH_MS;
static constexpr uint8_t kDefaultRemote = LOG_REMOTE_LEVELS_DEFAULT & kAllLevels;
static constexpr uint8_t kDefaultSerial = LOG_SERIAL_LEVELS_DEFAULT & kAllLevels;
static_assert(kMaxLine < kBatchBytes, "a single line must fit in one batch");
static_assert(kBatchBytes <= kBufBytes, "buffer must hold at least one batch");

static constexpr uint16_t kKeepaliveSec = 30;
static constexpr uint32_t kPingMs = 20000;      // liveness ping while connected
static constexpr uint32_t kSilentMs = 45000;    // nothing heard -> reconnect
static constexpr uint32_t kConnackMs = 10000;   // CONNECT sent, no CONNACK -> reconnect
static constexpr uint32_t kAckTimeoutMs = 15000; // log batch not acked -> reconnect + resend

struct State {
  bool started = false;
  bool suspended = false;
  bool wsUp = false;        // WebSocket open
  bool connectSent = false; // MQTT CONNECT sent, waiting for CONNACK
  bool connected = false;   // MQTT session up
  bool everConnected = false;
  bool refused = false;     // broker rejected the login
  uint8_t refusedRc = 0;
  bool force = false;       // flush(): send now, ignore batching delay
  bool planned = false;     // current disconnect/reconnect was caused by suspend()
  uint8_t remote = kDefaultRemote;
  uint8_t serial = kDefaultSerial;

  // Line buffer. [0, inflightLen) is the batch waiting for its PUBACK.
  char buf[kBufBytes];
  size_t len = 0;
  uint32_t lines = 0;
  uint32_t firstAt = 0;
  size_t inflightLen = 0;
  uint32_t inflightLines = 0;
  uint16_t inflightId = 0;
  uint32_t inflightAt = 0;
  uint32_t dropped = 0; // since the last report
  uint32_t droppedTotal = 0;
  uint32_t sent = 0;
  uint16_t nextId = 1;

  char id[13] = "";
  char clientId[16] = "";
  char tLog[24], tStatus[24], tCmd[24], tPing[24], tCtl[24];
  char fw[33] = "";
  char user[48] = "", pass[64] = "";

  char uri[128] = "";
  char host[64] = "";
  char path[32] = "/mqtt";
  uint16_t port = 0;
  bool tls = false;

  uint32_t lastHeard = 0;
  uint32_t lastPing = 0;
  uint32_t connectAt = 0;
  uint32_t upSince = 0;
  uint32_t lastHeapWarn = 0;
  uint32_t wsFailures = 0;   // failed connection attempts since the last success
  uint32_t sessions = 0;     // successful MQTT sessions since boot

  bool restartRequested = false;
  char restartId[24] = "";

  // MQTT packet reassembly (WebSocket frames need not align with packets).
  uint8_t rx[256];
  size_t rxLen = 0;
  size_t rxSkip = 0; // bytes of an oversized packet still to discard

  uint8_t tx[kBatchBytes + 96];
};
inline State &S() {
  static State s;
  return s;
}
// WebSocketsClient leaks its connection object in two places (library 2.7.x):
//  - a FAILED connect leaves `_client.ssl` (BearSSL, ~22 KB) allocated until
//    the next attempt, and
//  - begin() sets `_client.ssl/tcp = NULL` without deleting them.
// So every broker switch after a failed wss:// attempt leaked ~22 KB, leaving
// ~8 KB of heap: too little to send MQTT frames, so the socket kept dropping.
// release() frees the objects through the library's own clientDisconnect().
class FmWsClient : public WebSocketsClient {
public:
  void release() {
    if (_client.ssl || _client.tcp) clientDisconnect(&_client, nullptr);
  }
  // true when a connection object exists but its socket is down (failed
  // attempt). A socket that is up but still handshaking is left alone.
  bool holdsDeadConnection() { return (_client.ssl || _client.tcp) && !clientIsConnected(&_client); }
};
inline FmWsClient &ws() {
  static FmWsClient c;
  return c;
}

inline const char *levelName(Level l) {
  switch (l) {
  case LV_OFF: return "off";
  case LV_ERROR: return "error";
  case LV_WARN: return "warn";
  case LV_DEBUG: return "debug";
  default: return "info";
  }
}
inline char levelLetter(Level l) {
  switch (l) {
  case LV_ERROR: return 'E';
  case LV_WARN: return 'W';
  case LV_DEBUG: return 'D';
  default: return 'I';
  }
}

inline void log(Level level, const char *tag, const char *fmt, ...);

} // namespace RemoteLog

// ─── App-facing macros (LOG_LEVEL in globals.h = compile-time ceiling) ──
// Format strings are placed in flash (PSTR) to save ~5 KB of RAM.
#define LOGE(tag, fmt, ...) do { if (LOG_LEVEL >= 1) RemoteLog::log(RemoteLog::LV_ERROR, tag, PSTR(fmt), ##__VA_ARGS__); } while (0)
#define LOGW(tag, fmt, ...) do { if (LOG_LEVEL >= 2) RemoteLog::log(RemoteLog::LV_WARN, tag, PSTR(fmt), ##__VA_ARGS__); } while (0)
#define LOGI(tag, fmt, ...) do { if (LOG_LEVEL >= 3) RemoteLog::log(RemoteLog::LV_INFO, tag, PSTR(fmt), ##__VA_ARGS__); } while (0)
#define LOGD(tag, fmt, ...) do { if (LOG_LEVEL >= 4) RemoteLog::log(RemoteLog::LV_DEBUG, tag, PSTR(fmt), ##__VA_ARGS__); } while (0)

namespace RemoteLog {

namespace detail {

// ── Level storage (EEPROM bytes 424..426) ────────────────────
inline void loadLevels() {
  if (EEPROM.read(EEPROM_LOG_MAGIC_OFFSET) == EEPROM_LOG_MAGIC) {
    S().remote = EEPROM.read(EEPROM_LOG_REMOTE_OFFSET) & kAllLevels;
    S().serial = EEPROM.read(EEPROM_LOG_SERIAL_OFFSET) & kAllLevels;
  } else {
    S().remote = kDefaultRemote;
    S().serial = kDefaultSerial;
  }
}
inline void saveLevels() {
  EEPROM.write(EEPROM_LOG_MAGIC_OFFSET, EEPROM_LOG_MAGIC);
  EEPROM.write(EEPROM_LOG_REMOTE_OFFSET, S().remote);
  EEPROM.write(EEPROM_LOG_SERIAL_OFFSET, S().serial);
  EEPROM.commit();
}

// ── Tiny JSON helpers (payloads are small and flat) ──────────
inline uint8_t nameBit(const char *p) {
  for (uint8_t i = LV_ERROR; i <= LV_DEBUG; ++i) {
    const char *name = levelName(static_cast<Level>(i));
    size_t n = strlen(name);
    if (strncmp(p, name, n) == 0 && p[n] == '"') return bitOf(static_cast<Level>(i));
  }
  return 0;
}

// "key":["error","debug"] -> any combination ([] = off)
// "key":"info"            -> legacy threshold (error+warn+info)
inline bool jsonLevels(const char *json, const char *key, uint8_t &out) {
  char pat[16];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  ++p;
  while (*p == ' ') ++p;
  if (*p == '[') {
    const char *end = strchr(p, ']');
    if (!end) return false;
    uint8_t mask = 0;
    for (const char *q = strchr(p, '"'); q && q < end; q = strchr(q + 1, '"')) {
      mask |= nameBit(q + 1);
      q = strchr(q + 1, '"');
      if (!q) break;
    }
    out = mask;
    return true;
  }
  if (*p == '"') {
    ++p;
    if (strncmp(p, "off\"", 4) == 0) {
      out = 0;
      return true;
    }
    uint8_t b = nameBit(p);
    if (!b) return false;
    out = static_cast<uint8_t>((b << 1) - 1);
    return true;
  }
  return false;
}

inline int levelsJson(char *out, size_t cap, uint8_t mask) {
  int n = snprintf(out, cap, "[");
  for (uint8_t i = LV_ERROR; i <= LV_DEBUG; ++i) {
    if (!(mask & bitOf(static_cast<Level>(i)))) continue;
    n += snprintf(out + n, cap - n, "%s\"%s\"", n > 1 ? "," : "", levelName(static_cast<Level>(i)));
  }
  n += snprintf(out + n, cap - n, "]");
  return n;
}

// ── MQTT packet encoding ─────────────────────────────────────
inline size_t putLen(uint8_t *p, size_t v) {
  size_t n = 0;
  do {
    uint8_t b = v % 128;
    v /= 128;
    if (v) b |= 0x80;
    p[n++] = b;
  } while (v);
  return n;
}
inline size_t putStr(uint8_t *p, const char *s, size_t l) {
  p[0] = static_cast<uint8_t>(l >> 8);
  p[1] = static_cast<uint8_t>(l & 0xFF);
  memcpy(p + 2, s, l);
  return l + 2;
}
inline uint16_t newId() {
  uint16_t id = S().nextId++;
  if (S().nextId == 0) S().nextId = 1;
  return id;
}

inline bool send(const uint8_t *p, size_t n) {
  if (!S().wsUp) return false;
  return ws().sendBIN(p, n);
}

// PUBLISH. qos 0 or 1. Returns false if not sent. *idOut = packet id (qos 1).
inline bool publish(const char *topic, const char *payload, size_t plen, uint8_t qos, bool retain,
                    uint16_t *idOut = nullptr) {
  uint8_t *tx = S().tx;
  const size_t tl = strlen(topic);
  const size_t rem = 2 + tl + (qos ? 2 : 0) + plen;
  if (rem + 5 > sizeof(S().tx)) return false;
  size_t n = 0;
  tx[n++] = 0x30 | (qos << 1) | (retain ? 1 : 0);
  n += putLen(tx + n, rem);
  n += putStr(tx + n, topic, tl);
  if (qos) {
    uint16_t id = newId();
    tx[n++] = id >> 8;
    tx[n++] = id & 0xFF;
    if (idOut) *idOut = id;
  }
  memcpy(tx + n, payload, plen);
  n += plen;
  return send(tx, n);
}

inline void publishStatus() {
  char r[48], s[48], json[200];
  levelsJson(r, sizeof r, S().remote);
  levelsJson(s, sizeof s, S().serial);
  int n = snprintf(json, sizeof json,
                   "{\"state\":\"online\",\"fw\":\"%s\",\"remote\":%s,\"serial\":%s,\"caps\":[\"restart\"]}",
                   S().fw, r, s);
  publish(S().tStatus, json, n, 1, true);
}

inline void sendConnect() {
  static const char kWill[] = "{\"state\":\"offline\"}";
  uint8_t *tx = S().tx;
  const size_t cl = strlen(S().clientId), wl = strlen(S().tStatus), ml = sizeof(kWill) - 1,
               ul = strlen(S().user), pl = strlen(S().pass);
  const size_t rem = 10 + (2 + cl) + (2 + wl) + (2 + ml) + (2 + ul) + (2 + pl);
  size_t n = 0;
  tx[n++] = 0x10;
  n += putLen(tx + n, rem);
  n += putStr(tx + n, "MQTT", 4);
  tx[n++] = 0x04; // protocol level 3.1.1
  // username | password | will retain | will QoS 1 | will flag | clean session
  tx[n++] = 0x80 | 0x40 | 0x20 | 0x08 | 0x04 | 0x02;
  tx[n++] = kKeepaliveSec >> 8;
  tx[n++] = kKeepaliveSec & 0xFF;
  n += putStr(tx + n, S().clientId, cl);
  n += putStr(tx + n, S().tStatus, wl);
  n += putStr(tx + n, kWill, ml);
  n += putStr(tx + n, S().user, ul);
  n += putStr(tx + n, S().pass, pl);
  S().connectSent = send(tx, n);
  S().connectAt = millis();
}

// Clean close: MQTT DISCONNECT, then WebSocket close. Without the DISCONNECT
// the broker sees the socket vanish mid-session, logs "WebSocket is not open"
// when it next writes, and publishes the offline LWT.
inline void closeClean() {
  if (S().wsUp && S().connected) {
    static const uint8_t kDisc[2] = {0xE0, 0x00};
    send(kDisc, sizeof kDisc);
    ws().loop(); // push the frame out before closing
    delay(20);
  }
  ws().disconnect();
}

inline void sendSubscribe() {
  uint8_t *tx = S().tx;
  const size_t a = strlen(S().tCmd), b = strlen(S().tCtl);
  const size_t rem = 2 + (2 + a + 1) + (2 + b + 1);
  size_t n = 0;
  tx[n++] = 0x82;
  n += putLen(tx + n, rem);
  uint16_t id = newId();
  tx[n++] = id >> 8;
  tx[n++] = id & 0xFF;
  n += putStr(tx + n, S().tCmd, a);
  tx[n++] = 1;
  n += putStr(tx + n, S().tCtl, b);
  tx[n++] = 1;
  send(tx, n);
}

// The in-flight batch was acked: drop it from the buffer.
inline void commitInflight() {
  memmove(S().buf, S().buf + S().inflightLen, S().len - S().inflightLen);
  S().len -= S().inflightLen;
  S().lines = S().lines > S().inflightLines ? S().lines - S().inflightLines : 0;
  if (S().len == 0) S().lines = 0;
  else S().firstAt = millis();
  S().sent += S().inflightLines;
  S().inflightLen = 0;
  S().inflightLines = 0;
}

inline void applyCommand(const char *json) {
  uint8_t r = S().remote, s = S().serial;
  const bool hasRemote = jsonLevels(json, "remote", r);
  const bool hasSerial = jsonLevels(json, "serial", s);
  if (!hasRemote && !hasSerial) return;
  const bool changed = (r != S().remote) || (s != S().serial);
  S().remote = r;
  S().serial = s;
  if (changed) saveLevels();
  publishStatus(); // always confirm, so the console shows "synced"
  if (changed) {
    char rl[48], sl[48];
    levelsJson(rl, sizeof rl, r);
    levelsJson(sl, sizeof sl, s);
    LOGI("RLOG", "log levels set from console: remote=%s serial=%s", rl, sl);
  }
}

inline void onControl(const char *ctl) {
  if (!strstr(ctl, "\"action\":\"restart\"")) return;
  S().restartId[0] = '\0';
  const char *id = strstr(ctl, "\"id\":\"");
  if (id) {
    id += 6;
    size_t n = 0;
    while (id[n] && id[n] != '"' && n < sizeof S().restartId - 1) ++n;
    memcpy(S().restartId, id, n);
    S().restartId[n] = '\0';
  }
  S().restartRequested = true; // carried out by MqttLogging::loop()
}

inline void onPublish(uint8_t flags, const uint8_t *body, size_t rem) {
  if (rem < 2) return;
  const size_t tl = (static_cast<size_t>(body[0]) << 8) | body[1];
  const uint8_t qos = (flags >> 1) & 0x03;
  size_t off = 2 + tl;
  if (off > rem) return;
  uint16_t pid = 0;
  if (qos) {
    if (off + 2 > rem) return;
    pid = (static_cast<uint16_t>(body[off]) << 8) | body[off + 1];
    off += 2;
  }
  if (qos == 1) {
    uint8_t ack[4] = {0x40, 0x02, static_cast<uint8_t>(pid >> 8), static_cast<uint8_t>(pid & 0xFF)};
    send(ack, sizeof ack);
  }
  const char *topic = reinterpret_cast<const char *>(body + 2);
  const size_t plen = rem - off;
  if (plen == 0 || plen >= 128) return; // empty = retained message cleared
  char msg[128];
  memcpy(msg, body + off, plen);
  msg[plen] = '\0';
  if (tl == strlen(S().tCtl) && strncmp(topic, S().tCtl, tl) == 0) onControl(msg);
  else if (tl == strlen(S().tCmd) && strncmp(topic, S().tCmd, tl) == 0) applyCommand(msg);
}

inline void onPacket(const uint8_t *p, size_t hdr, size_t rem) {
  const uint8_t type = p[0] & 0xF0;
  const uint8_t *body = p + hdr;
  S().lastHeard = millis();
  switch (type) {
  case 0x20: { // CONNACK
    const uint8_t rc = rem >= 2 ? body[1] : 0xFF;
    S().connectSent = false;
    if (rc == 0) {
      S().connected = true;
      S().everConnected = true;
      S().refused = false;
      S().upSince = millis();
      S().lastPing = millis();
      S().inflightLen = 0; // anything un-acked is sent again
      S().inflightLines = 0;
      S().wsFailures = 0;
      ++S().sessions;
      sendSubscribe();
      publishStatus();
      if (S().planned)
        LOGD("RLOG", "MQTT reconnected after suspend (session #%u, heap=%u maxBlock=%u)",
             static_cast<unsigned>(S().sessions), ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
      else
        LOGI("RLOG", "MQTT connected to %s (session #%u, heap=%u maxBlock=%u)", S().uri,
             static_cast<unsigned>(S().sessions), ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
      S().planned = false;
    } else {
      S().refused = true;
      S().refusedRc = rc;
      LOGE("RLOG",
          "broker refused login rc=%u (4/5 = check MQTT_USERNAME/MQTT_PASSWORD and that MAC %s is registered)",
          rc, S().id);
      ws().disconnect();
    }
    break;
  }
  case 0x40: // PUBACK
    if (rem >= 2 && S().inflightLen) {
      const uint16_t pid = (static_cast<uint16_t>(body[0]) << 8) | body[1];
      if (pid == S().inflightId) commitInflight();
    }
    break;
  case 0x30: // PUBLISH
    onPublish(p[0] & 0x0F, body, rem);
    break;
  default: // SUBACK, PINGRESP, ...
    break;
  }
}

// Feed WebSocket payload bytes; dispatches every complete MQTT packet.
inline void feed(const uint8_t *data, size_t len) {
  while (len) {
    if (S().rxSkip) {
      size_t k = len < S().rxSkip ? len : S().rxSkip;
      S().rxSkip -= k;
      data += k;
      len -= k;
      continue;
    }
    size_t space = sizeof(S().rx) - S().rxLen;
    size_t k = len < space ? len : space;
    memcpy(S().rx + S().rxLen, data, k);
    S().rxLen += k;
    data += k;
    len -= k;

    for (;;) {
      if (S().rxLen < 2) break;
      size_t rem = 0, mul = 1, i = 1;
      bool complete = false;
      for (; i < S().rxLen && i <= 4; ++i) {
        rem += (S().rx[i] & 0x7F) * mul;
        mul *= 128;
        if (!(S().rx[i] & 0x80)) {
          complete = true;
          break;
        }
      }
      if (!complete) {
        if (i > 4) S().rxLen = 0; // malformed length
        break;
      }
      const size_t hdr = i + 1;
      const size_t total = hdr + rem;
      if (total > sizeof(S().rx)) { // oversized: not something we need
        S().rxSkip = total - S().rxLen;
        S().rxLen = 0;
        break;
      }
      if (total > S().rxLen) break;
      onPacket(S().rx, hdr, rem);
      memmove(S().rx, S().rx + total, S().rxLen - total);
      S().rxLen -= total;
    }
  }
}

inline void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
  case WStype_CONNECTED:
    S().wsUp = true;
    S().rxLen = 0;
    S().rxSkip = 0;
    sendConnect();
    break;
  case WStype_DISCONNECTED: {
    const bool wasUp = S().connected;
    const uint32_t upFor = wasUp ? (millis() - S().upSince) / 1000 : 0;
    S().wsUp = false;
    S().connected = false;
    S().connectSent = false;
    S().inflightLen = 0;
    S().inflightLines = 0;
    S().rxLen = 0;
    if (wasUp && S().suspended) {
      LOGD("RLOG", "MQTT closed for suspend after %lus", static_cast<unsigned long>(upFor));
    } else if (wasUp) {
      S().planned = false;
      LOGW("RLOG", "MQTT disconnected after %lus (wifi=%d rssi=%d heap=%u maxBlock=%u)",
          static_cast<unsigned long>(upFor), static_cast<int>(WiFi.status()),
          WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0, ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
    } else if (!S().suspended && WiFi.status() == WL_CONNECTED) {
      // Repeats on every retry: serial only, and only every 5th attempt.
      ++S().wsFailures;
      if ((S().wsFailures % 5) == 1 && (S().serial & bitOf(LV_WARN)))
        Serial.printf("[W][RLOG] broker %s unreachable (attempt %u, heap=%u maxBlock=%u)\n", S().uri,
                      static_cast<unsigned>(S().wsFailures), ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
    }
    break;
  }
  case WStype_BIN:
  case WStype_FRAGMENT_BIN_START:
  case WStype_FRAGMENT:
  case WStype_FRAGMENT_FIN:
    feed(payload, length);
    break;
  default:
    break;
  }
}

// ws[s]://host[:port][/path] -> S().host / port / path / tls.
inline bool parseUri(const char *uri) {
  const char *p = uri;
  if (strncmp(p, "wss://", 6) == 0) {
    S().tls = true;
    p += 6;
  } else if (strncmp(p, "ws://", 5) == 0) {
    S().tls = false;
    p += 5;
  } else {
    return false;
  }
  const char *slash = strchr(p, '/');
  const char *hostEnd = slash ? slash : p + strlen(p);
  const char *colon = static_cast<const char *>(memchr(p, ':', hostEnd - p));
  const char *nameEnd = colon ? colon : hostEnd;
  size_t hl = nameEnd - p;
  if (hl == 0 || hl >= sizeof(S().host)) return false;
  memcpy(S().host, p, hl);
  S().host[hl] = '\0';
  S().port = colon ? static_cast<uint16_t>(atoi(colon + 1)) : (S().tls ? 443 : 80);
  snprintf(S().path, sizeof S().path, "%s", slash ? slash : "/");
  return S().port != 0;
}

inline bool startWs() {
  ws().release(); // begin() would otherwise orphan the old connection object
  if (!parseUri(S().uri)) return false;
  if (S().tls)
    ws().beginSSL(S().host, S().port, S().path, static_cast<const uint8_t *>(nullptr), "mqtt");
  else
    ws().begin(S().host, S().port, S().path, "mqtt");
  return true;
}

// Append one line ("[I][TAG] text\n"); oldest lines go first if it's full.
inline void enqueueLine(const char *line, size_t len) {
  if (len == 0) return;
  while (S().len > 0 && S().len + len + 1 > kBufBytes) {
    char *nl = static_cast<char *>(memchr(S().buf, '\n', S().len));
    size_t cut = nl ? static_cast<size_t>(nl - S().buf) + 1 : S().len;
    if (S().inflightLen) { // the oldest lines are in flight: give up on that batch
      S().inflightLen = 0;
      S().inflightLines = 0;
    }
    memmove(S().buf, S().buf + cut, S().len - cut);
    S().len -= cut;
    if (S().lines) --S().lines;
    ++S().dropped;
    ++S().droppedTotal;
  }
  if (S().len == 0) S().firstAt = millis();
  memcpy(S().buf + S().len, line, len);
  S().len += len;
  S().buf[S().len++] = '\n';
  ++S().lines;
}

// Send the next batch if one is due and nothing is in flight.
inline void sendBatch() {
  if (S().inflightLen || S().len == 0) return;
  const uint32_t now = millis();
  const bool due = S().force || S().len >= kBatchBytes * 3 / 4 || now - S().firstAt >= kFlushMs;
  if (!due) return;

  size_t n = S().len < kBatchBytes ? S().len : kBatchBytes;
  if (n < S().len)
    while (n > 0 && S().buf[n - 1] != '\n') --n; // cut at the last full line
  if (n == 0) return;
  uint32_t lines = 0;
  for (size_t i = 0; i < n; ++i)
    if (S().buf[i] == '\n') ++lines;

  const size_t plen = S().buf[n - 1] == '\n' ? n - 1 : n; // lines are joined by '\n'
  uint16_t pid = 0;
  if (!publish(S().tLog, S().buf, plen, 1, false, &pid)) return;
  S().inflightLen = n;
  S().inflightLines = lines;
  S().inflightId = pid;
  S().inflightAt = now;

  if (S().dropped) {
    char note[72];
    int m = snprintf(note, sizeof note, "[W][RLOG] %u log lines dropped (offline or buffer full)",
                     static_cast<unsigned>(S().dropped));
    if (publish(S().tLog, note, m, 0, false)) S().dropped = 0;
  }
}

} // namespace detail

// ─── Public API ──────────────────────────────────────────────

// Load saved levels (EEPROM must already be begun). Call early in setup().
inline void init() { detail::loadLevels(); }

// Start the MQTT client. Wi-Fi must be connected. Strings are copied.
inline bool begin(const char *uri, const char *username, const char *password, const char *fwVersion) {
  if (S().started) return true;
  String mac = WiFi.macAddress(); // "AA:BB:CC:DD:EE:FF"
  size_t k = 0;
  for (size_t i = 0; i < mac.length() && k < 12; ++i) {
    char c = mac[i];
    if (c == ':') continue;
    S().id[k++] = static_cast<char>(tolower(c));
  }
  S().id[k] = '\0';
  snprintf(S().clientId, sizeof S().clientId, "fm-%s", S().id);
  snprintf(S().tLog, sizeof S().tLog, "fm/%s/log", S().id);
  snprintf(S().tStatus, sizeof S().tStatus, "fm/%s/status", S().id);
  snprintf(S().tCmd, sizeof S().tCmd, "fm/%s/cmd", S().id);
  snprintf(S().tPing, sizeof S().tPing, "fm/%s/ping", S().id);
  snprintf(S().tCtl, sizeof S().tCtl, "fm/%s/ctl", S().id);
  snprintf(S().fw, sizeof S().fw, "%s", fwVersion ? fwVersion : "");
  snprintf(S().user, sizeof S().user, "%s", username ? username : "");
  snprintf(S().pass, sizeof S().pass, "%s", password ? password : "");
  snprintf(S().uri, sizeof S().uri, "%s", uri ? uri : "");

  ws().onEvent(detail::onWsEvent);
  ws().setReconnectInterval(REMOTE_LOG_RETRY_MS);
  if (!detail::startWs()) return false;
  S().started = true;
  S().lastHeard = millis();
  return true;
}

// Switch broker at runtime.
inline void setUri(const char *uri) {
  detail::closeClean();
  snprintf(S().uri, sizeof S().uri, "%s", uri ? uri : "");
  S().wsFailures = 0;
  detail::startWs();
}
inline const char *uri() { return S().uri; }

// Call from loop() (MqttLogging::loop() does it).
inline void loop() {
  if (!S().started || S().suspended) return;
  const uint32_t now = millis();

  // Not connected: the library will try to connect inside ws().loop(). A TLS
  // handshake without a big enough free block crashes or hangs, so wait.
  if (!S().wsUp && S().tls && ESP.getMaxFreeBlockSize() < TLS_MIN_FREE_BLOCK) {
    if (now - S().lastHeapWarn >= 60000) {
      S().lastHeapWarn = now;
      LOGW("RLOG", "not connecting: largest free block %u < %u needed for TLS (heap=%u frag=%u%%)",
          ESP.getMaxFreeBlockSize(), static_cast<unsigned>(TLS_MIN_FREE_BLOCK), ESP.getFreeHeap(),
          ESP.getHeapFragmentation());
    }
    return;
  }

  if (!S().wsUp && ESP.getFreeHeap() < WS_MIN_FREE_HEAP) {
    if (now - S().lastHeapWarn >= 60000) {
      S().lastHeapWarn = now;
      LOGW("RLOG", "not connecting: free heap %u < %u", ESP.getFreeHeap(), static_cast<unsigned>(WS_MIN_FREE_HEAP));
    }
    return;
  }

  if (!S().wsUp) Crumbs::stage("mqtt-connect");
  ws().loop();
  if (!S().wsUp) {
    // Failed attempt: give the ~22 KB TLS object back now instead of holding
    // it for the whole retry interval.
    if (ws().holdsDeadConnection()) ws().release();
    return;
  }

  if (!S().connected) {
    if (!S().connectSent) detail::sendConnect();
    else if (now - S().connectAt >= kConnackMs) {
      LOGW("RLOG", "no CONNACK from broker in %lus - reconnecting",
          static_cast<unsigned long>(kConnackMs / 1000));
      ws().disconnect();
    }
    return;
  }

  if (now - S().lastPing >= kPingMs) {
    S().lastPing = now;
    detail::publish(S().tPing, "1", 1, 1, false);
  }
  if (now - S().lastHeard >= kSilentMs) {
    LOGW("RLOG", "no reply from server for %lus - forcing MQTT reconnect",
        static_cast<unsigned long>((now - S().lastHeard) / 1000));
    S().lastHeard = now;
    ws().disconnect();
    return;
  }
  if (S().inflightLen && now - S().inflightAt >= kAckTimeoutMs) {
    LOGW("RLOG", "log batch not acknowledged in %lus - reconnecting",
        static_cast<unsigned long>(kAckTimeoutMs / 1000));
    ws().disconnect();
    return;
  }
  detail::sendBatch();
}

// Send everything buffered (up to timeoutMs). Use before restart / suspend.
inline bool flush(uint32_t timeoutMs = 3000) {
  if (!S().started || S().suspended) return false;
  S().force = true;
  const uint32_t start = millis();
  bool done = false;
  while (millis() - start < timeoutMs) {
    if (!S().connected) break; // don't burn the time on reconnecting
    loop();
    if (S().len == 0) {
      done = true;
      break;
    }
    ESP.wdtFeed();
    delay(10);
  }
  S().force = false;
  return done;
}

// Close the connection to free its TLS memory for another HTTPS request.
// Lines keep buffering. Undo with resume().
inline void suspend() {
  if (!S().started || S().suspended) return;
  flush(2000);
  S().suspended = true;
  S().planned = true;
  detail::closeClean();
  S().wsUp = false;
  S().connected = false;
  S().connectSent = false;
}
inline void resume() {
  if (!S().suspended) return;
  S().suspended = false;
  S().lastHeard = millis();
}

inline bool connected() { return S().connected; }
inline bool started() { return S().started; }
inline bool suspended() { return S().suspended; }
inline bool everConnected() { return S().everConnected; }
inline bool refused() { return S().refused; }
inline uint8_t remoteLevels() { return S().remote; }
inline uint8_t serialLevels() { return S().serial; }
inline const char *deviceId() { return S().id; }
inline uint32_t sent() { return S().sent; }
inline uint32_t queued() { return S().lines; }
inline uint32_t dropped() { return S().droppedTotal; }
inline uint32_t sessions() { return S().sessions; }

inline bool takeRestartRequest(char *id = nullptr, size_t cap = 0) {
  if (!S().restartRequested) return false;
  S().restartRequested = false;
  if (id && cap) snprintf(id, cap, "%s", S().restartId);
  return true;
}

inline String levelsList(uint8_t mask) {
  String s;
  for (uint8_t i = LV_ERROR; i <= LV_DEBUG; ++i) {
    if (!(mask & bitOf(static_cast<Level>(i)))) continue;
    if (s.length()) s += ',';
    s += levelName(static_cast<Level>(i));
  }
  return s;
}

// Core logger. Prefer the LOGx macros. Allocation-free.
inline void logv(Level level, const char *tag, const char *fmt, va_list ap) {
  const uint8_t bit = bitOf(level);
  if (!bit) return;
  const bool toSerial = (S().serial & bit) != 0;
  const bool toRemote = (S().remote & bit) != 0;
  if (!toSerial && !toRemote) return;

  char line[kMaxLine];
  int head = snprintf(line, sizeof line, "[%c][%s] ", levelLetter(level), tag ? tag : "APP");
  if (head < 0) return;
  if (head >= static_cast<int>(sizeof line)) head = sizeof line - 1;
  // vsnprintf_P reads the format from flash (PSTR) or RAM.
  int body = vsnprintf_P(line + head, sizeof line - head, fmt, ap);
  size_t len = static_cast<size_t>(head) + (body > 0 ? static_cast<size_t>(body) : 0);
  if (len > sizeof line - 1) len = sizeof line - 1;
  for (size_t i = 0; i < len; ++i)
    if (line[i] == '\n' || line[i] == '\r') line[i] = ' ';

  if (toSerial) {
    Serial.write(reinterpret_cast<const uint8_t *>(line), len);
    Serial.write('\n');
  }
  if (toRemote) detail::enqueueLine(line, len);
}

inline void log(Level level, const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  logv(level, tag, fmt, ap);
  va_end(ap);
}

} // namespace RemoteLog

// ─── FirmwareManager backend URLs (shared with ota_update_logic.h) ──
namespace FmServer {

inline String primaryBase() {
  String b = FM_PRIMARY_BASE_URL;
  b.trim();
  while (b.endsWith("/")) b.remove(b.length() - 1);
  return b;
}

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

inline String brokerFor(const String &base) {
  if (base.startsWith("https://")) return "wss://" + base.substring(8) + "/mqtt";
  if (base.startsWith("http://")) return "ws://" + base.substring(7) + "/mqtt";
  return "";
}

} // namespace FmServer

// ─── Diag — deliberate restarts, boot report, health line ────
namespace Diag {

// The ONLY way this firmware restarts on purpose: the reason is stored in
// RTC memory and logged again after the reboot, so the console shows it.
inline void restart(const char *reason) {
  Crumbs::note(reason);
  LOGW("SYS", "restarting: %s (up %lus, heap=%u)", reason, static_cast<unsigned long>(millis() / 1000),
       ESP.getFreeHeap());
  RemoteLog::flush(3000);
  RemoteLog::detail::closeClean();
  Serial.flush();
  delay(200);
  ESP.restart();
}

inline const char *resetName(uint32_t r) {
  switch (r) {
  case REASON_DEFAULT_RST: return "power-on";
  case REASON_WDT_RST: return "hardware watchdog";
  case REASON_EXCEPTION_RST: return "exception (crash)";
  case REASON_SOFT_WDT_RST: return "software watchdog";
  case REASON_SOFT_RESTART: return "software restart";
  case REASON_DEEP_SLEEP_AWAKE: return "deep-sleep wake";
  case REASON_EXT_SYS_RST: return "external reset (RST pin)";
  default: return "unknown";
  }
}

// Explain WHY the previous session ended. Call once after logging is up.
inline void reportBoot() {
  const rst_info *ri = ESP.getResetInfoPtr();
  const uint32_t reason = ri ? ri->reason : 0xFF;
  const Crumbs::Record &p = Crumbs::Prev();
  const bool valid = Crumbs::prevValid();

  LOGI("BOOT", "reset reason: %s (%u), boot #%u", resetName(reason), static_cast<unsigned>(reason),
       static_cast<unsigned>(Crumbs::R().bootCount));

  switch (reason) {
  case REASON_DEFAULT_RST:
    LOGW("BOOT", "previous session ended by POWER LOSS or brownout (no crash record). "
                 "Check the 5V supply, USB cable and JSN-SR04T current draw");
    break;
  case REASON_EXCEPTION_RST:
    LOGE("BOOT", "CRASH exccause=%u epc1=0x%08x epc2=0x%08x epc3=0x%08x excvaddr=0x%08x depc=0x%08x",
         static_cast<unsigned>(ri->exccause), static_cast<unsigned>(ri->epc1), static_cast<unsigned>(ri->epc2),
         static_cast<unsigned>(ri->epc3), static_cast<unsigned>(ri->excvaddr), static_cast<unsigned>(ri->depc));
    break;
  case REASON_WDT_RST:
  case REASON_SOFT_WDT_RST:
    LOGE("BOOT", "WATCHDOG reset: code blocked for too long (epc1=0x%08x)", static_cast<unsigned>(ri->epc1));
    break;
  case REASON_SOFT_RESTART:
    if (valid && p.note[0]) LOGI("BOOT", "deliberate restart: %s", p.note);
    else LOGW("BOOT", "software restart without a recorded reason (library or OTA)");
    break;
  default:
    break;
  }

  if (valid) {
    const bool bad = reason == REASON_EXCEPTION_RST || reason == REASON_WDT_RST || reason == REASON_SOFT_WDT_RST;
    char line[180];
    snprintf(line, sizeof line,
             "previous session: up %lus, last stage '%s', heap=%u minHeap=%u maxBlock=%u frag=%u%%",
             static_cast<unsigned long>(p.uptimeSec), p.stage, static_cast<unsigned>(p.freeHeap),
             static_cast<unsigned>(p.minFreeHeap), static_cast<unsigned>(p.maxBlock), static_cast<unsigned>(p.frag));
    if (bad) LOGE("BOOT", "%s", line);
    else LOGI("BOOT", "%s", line);
    if (bad && p.minFreeHeap < 8000) LOGE("BOOT", "heap was nearly exhausted before the reset - likely out of memory");
  }
}

// Periodic health line + RTC heap sample. Call from loop().
inline void loop() {
  static uint32_t lastSample = 0, lastHealth = 0;
  const uint32_t now = millis();
  if (now - lastSample >= 5000) {
    lastSample = now;
    Crumbs::sample();
  }
  if (now - lastHealth >= HEALTH_LOG_INTERVAL) {
    lastHealth = now;
    const uint32_t heap = ESP.getFreeHeap();
    LOGI("SYS", "up=%lus heap=%u minHeap=%u maxBlock=%u frag=%u%% rssi=%d mqtt=%d sessions=%u queued=%u dropped=%u",
         static_cast<unsigned long>(now / 1000), heap, static_cast<unsigned>(Crumbs::R().minFreeHeap),
         ESP.getMaxFreeBlockSize(), ESP.getHeapFragmentation(), WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
         static_cast<int>(RemoteLog::connected()), static_cast<unsigned>(RemoteLog::sessions()),
         static_cast<unsigned>(RemoteLog::queued()), static_cast<unsigned>(RemoteLog::dropped()));
    if (heap < 10000) LOGW("SYS", "low free heap: %u bytes", heap);
  }
}

} // namespace Diag

// ─── MqttLogging — starts RemoteLog, broker failover, remote restart ──
namespace MqttLogging {

struct Brokers {
  String uri[2];
  int count = 0;
};

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
  bool (*restartGuard)() = nullptr;
  const char *fw = FW_VERSION;
  int index = 0;
  uint32_t switchedAt = 0;
  uint32_t checkedAt = 0;
  uint32_t offlineSince = 0;
  bool offline = false;
};
inline State &St() {
  static State s;
  return s;
}

inline void setFirmwareVersion(const char *fw) { St().fw = fw; }
inline void setBeforeRestart(void (*fn)()) { St().beforeRestart = fn; }
// false = defer the offline-watchdog restart (device busy).
inline void setRestartGuard(bool (*fn)()) { St().restartGuard = fn; }

inline void serviceRestart() {
  char id[24];
  if (!RemoteLog::takeRestartRequest(id, sizeof id)) return;
  LOGW("SYS", "restart requested from FirmwareManager console (request %s)", id[0] ? id : "?");
  if (St().beforeRestart) St().beforeRestart();
  Diag::restart("remote restart from FirmwareManager console");
}

// Last resort: MQTT offline for 10 min while Wi-Fi is up.
inline void superviseOffline(uint32_t now) {
#if REMOTE_LOG_WATCHDOG_RESTART
  if (RemoteLog::connected() || RemoteLog::suspended() || WiFi.status() != WL_CONNECTED) {
    St().offline = false;
    return;
  }
  if (!St().offline) {
    St().offline = true;
    St().offlineSince = now;
    return;
  }
  if (now - St().offlineSince >= 600000UL && RemoteLog::everConnected() && !RemoteLog::refused() &&
      (!St().restartGuard || St().restartGuard())) {
    char note[80];
    snprintf(note, sizeof note, "MQTT offline %lus with Wi-Fi up (heap=%u maxBlock=%u)",
             static_cast<unsigned long>((now - St().offlineSince) / 1000), ESP.getFreeHeap(),
             ESP.getMaxFreeBlockSize());
    Diag::restart(note);
  }
#else
  (void)now;
#endif
}

// Call every loop().
inline void loop() {
  RemoteLog::loop();
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
      LOGE("RLOG", "remote logging failed to start (bad broker URI %s)", b.uri[0].c_str());
    }
    St().index = 0;
    St().switchedAt = now;
    return;
  }

  superviseOffline(now);
  // Time spent suspended (OTA) or connected to the primary must not
  // count towards the "primary unreachable" timer.
  if (RemoteLog::suspended() || (RemoteLog::connected() && St().index == 0)) St().switchedAt = now;
  if (b.count < 2 || RemoteLog::suspended()) return;

  if (RemoteLog::connected()) {
    if (St().index == 1 && now - St().switchedAt >= REMOTE_LOG_PREFER_RETRY_MS) {
      LOGI("RLOG", "retrying primary broker %s", b.uri[0].c_str());
      RemoteLog::flush(2000);
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

} // namespace MqttLogging

#endif // CUSTOM_MQTT_LOGGING_H
