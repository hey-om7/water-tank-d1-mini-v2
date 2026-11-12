/*
 * custom_mqtt_logging.h — Remote logging to the FirmwareManager console
 * =======================================================================
 * ESP8266 (Wemos/LOLIN D1 mini v2) MQTT 3.1.1 client for the broker built
 * into FirmwareManager. The transport (FmLink: TCP, TLS, WebSocket) is the
 * one FirmwareManager generates for ESP8266 (tank-monitor-device-model-1-v1/),
 * so no extra library is needed.
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
 * stores it in EEPROM and confirms on fm/<mac>/status.
 *
 * Wire protocol (see tank-monitor-device-model-1-v1/DEVICE_PROTOCOL.md):
 *   fm/<mac>/log     device -> server  batched lines, QoS 1
 *   fm/<mac>/status  device -> server  retained {"state","fw","remote","serial","caps"}; LWT offline
 *   fm/<mac>/cmd     server -> device  retained {"remote":[...],"serial":[...]}
 *   fm/<mac>/ping    device -> server  liveness "1" every 20 s, QoS 1
 *   fm/<mac>/ctl     server -> device  one-shot {"action":"restart","id":"..."}
 * <mac> = Wi-Fi STA MAC, 12 lowercase hex digits. Client id "fm-<mac>".
 *
 * ESP8266 notes:
 *  - Single-threaded: call MqttLogging::loop() from loop(). A (re)connect
 *    blocks for up to ~10 s (TCP + TLS + WebSocket + CONNACK); it is retried
 *    every REMOTE_LOG_RETRY_MS.
 *  - A wss:// session holds ~25 KB of heap for TLS. Before any other HTTPS
 *    request (OTA check) call RemoteLog::suspend(); RemoteLog::resume()
 *    reconnects. Two TLS sessions at once do not fit in ESP8266 RAM.
 *  - Lines logged while offline are kept in a RAM buffer and sent later.
 *    A batch is only removed from the buffer once the server acks it.
 */

#ifndef CUSTOM_MQTT_LOGGING_H
#define CUSTOM_MQTT_LOGGING_H

#include "../config/globals.h"
#include "../diag/crumbs.h"
#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <memory>
#include <stdarg.h>

// ----------------------------------------------------------------------------
// FmLink - one byte stream to the broker: TCP, TLS, WebSocket or both.
// (from the FirmwareManager ESP8266 starter)
// ----------------------------------------------------------------------------
namespace FmLink {

struct Url {
  bool tls = false;
  bool ws = false;
  String host;
  uint16_t port = 0;
  String path = "/";
};

/** ws:// wss:// mqtt:// mqtts:// [host][:port][/path] */
inline bool parse(const String &uri, Url &u) {
  int skip;
  if (uri.startsWith("wss://")) { u.tls = true; u.ws = true; skip = 6; u.port = 443; }
  else if (uri.startsWith("ws://")) { u.ws = true; skip = 5; u.port = 80; }
  else if (uri.startsWith("mqtts://")) { u.tls = true; skip = 8; u.port = 8883; }
  else if (uri.startsWith("mqtt://")) { skip = 7; u.port = 1883; }
  else return false;
  String rest = uri.substring(skip);
  const int slash = rest.indexOf('/');
  u.path = slash >= 0 ? rest.substring(slash) : String("/");
  if (slash >= 0) rest = rest.substring(0, slash);
  const int colon = rest.indexOf(':');
  if (colon >= 0) {
    u.port = static_cast<uint16_t>(rest.substring(colon + 1).toInt());
    rest = rest.substring(0, colon);
  }
  u.host = rest;
  return u.host.length() > 0 && u.port > 0;
}

/** BearSSL client: pinned fingerprint if set, else encrypted-only. */
inline void setupTls(BearSSL::WiFiClientSecure &c, const String &host, uint16_t port) {
  if (strlen(FM_TLS_FINGERPRINT)) c.setFingerprint(FM_TLS_FINGERPRINT);
  else c.setInsecure();
  // Small buffers if the server supports max-fragment-length, else a full
  // 16 KB receive buffer (TLS records can be that big).
  if (c.probeMaxFragmentLength(host.c_str(), port, 1024)) c.setBufferSizes(1024, 1024);
  else c.setBufferSizes(16384, 1024);
}

inline String base64(const uint8_t *in, size_t n) {
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += i + 1 < n ? tbl[(v >> 6) & 63] : '=';
    out += i + 2 < n ? tbl[v & 63] : '=';
  }
  return out;
}

class Link {
public:
  static constexpr size_t kMaxOut = REMOTE_LOG_BATCH_BYTES + 160; // one MQTT packet

  /** Blocking connect (+ TLS + WebSocket handshake). */
  bool open(const Url &u, uint32_t timeoutMs) {
    close();
    ws_ = u.ws;
    if (u.tls) {
      secure_.reset(new (std::nothrow) BearSSL::WiFiClientSecure());
      if (!secure_) {
        error_ = "out of memory for TLS";
        return false;
      }
      setupTls(*secure_, u.host, u.port);
      c_ = secure_.get();
    } else {
      c_ = &plain_;
    }
    c_->setTimeout(timeoutMs);
    if (!c_->connect(u.host.c_str(), u.port)) {
      error_ = u.tls ? "TLS connect failed" : "TCP connect failed";
      close();
      return false;
    }
    c_->setNoDelay(true);
    if (ws_ && !handshake(u, timeoutMs)) {
      close();
      return false;
    }
    error_ = "";
    return true;
  }

  void close() {
    if (c_) c_->stop();
    c_ = nullptr;
    secure_.reset(); // frees the TLS buffers
    rxHead_ = rxLen_ = 0;
  }

  bool connected() { return c_ && c_->connected(); }
  const char *error() const { return error_; }

  /** MQTT bytes ready to read (decodes WebSocket frames as they arrive). */
  int available() {
    if (!c_) return 0;
    if (!ws_) return c_->available();
    while (rxHead_ == rxLen_ && c_ && c_->available() >= 2) {
      if (!pumpFrame()) return 0;
    }
    return static_cast<int>(rxLen_ - rxHead_);
  }

  /** One MQTT byte, waiting up to timeoutMs for it. -1 on timeout / drop. */
  int read(uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    while (available() <= 0) {
      if (!connected() || millis() - t0 >= timeoutMs) return -1;
      delay(1);
    }
    if (!ws_) return c_->read();
    return rx_[rxHead_++];
  }

  /** Send one complete MQTT packet. */
  bool write(const uint8_t *p, size_t n) {
    if (!c_ || n > kMaxOut) return false;
    if (!ws_) return c_->write(p, n) == n;
    // Client -> server frames are masked (RFC 6455 5.3). One binary frame per packet.
    static uint8_t frame[kMaxOut + 8];
    size_t h = 0;
    frame[h++] = 0x82;
    if (n < 126) {
      frame[h++] = 0x80 | static_cast<uint8_t>(n);
    } else {
      frame[h++] = 0x80 | 126;
      frame[h++] = static_cast<uint8_t>(n >> 8);
      frame[h++] = static_cast<uint8_t>(n);
    }
    const uint32_t key = ESP.random();
    uint8_t mask[4] = {static_cast<uint8_t>(key), static_cast<uint8_t>(key >> 8), static_cast<uint8_t>(key >> 16),
                       static_cast<uint8_t>(key >> 24)};
    memcpy(frame + h, mask, 4);
    h += 4;
    for (size_t i = 0; i < n; ++i) frame[h + i] = p[i] ^ mask[i & 3];
    return c_->write(frame, h + n) == h + n;
  }

private:
  bool readExact(uint8_t *out, size_t n, uint32_t timeoutMs = 3000) {
    const uint32_t t0 = millis();
    size_t got = 0;
    while (got < n) {
      const int a = c_->available();
      if (a > 0) {
        const int r = c_->read(out + got, n - got);
        if (r > 0) got += static_cast<size_t>(r);
        continue;
      }
      if (!c_->connected() || millis() - t0 >= timeoutMs) return false;
      delay(1);
    }
    return true;
  }

  /** Read one WebSocket frame. Data goes to rx_; ping is answered; close drops the link. */
  bool pumpFrame() {
    uint8_t hdr[2];
    if (!readExact(hdr, 2)) return fail("WebSocket read timeout");
    const uint8_t op = hdr[0] & 0x0F;
    size_t len = hdr[1] & 0x7F;
    if (len == 126) {
      uint8_t ext[2];
      if (!readExact(ext, 2)) return fail("WebSocket read timeout");
      len = (static_cast<size_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
      return fail("WebSocket frame too large");
    }
    uint8_t mask[4] = {0, 0, 0, 0};
    const bool masked = hdr[1] & 0x80;
    if (masked && !readExact(mask, 4)) return fail("WebSocket read timeout");

    if (op == 0x0 || op == 0x1 || op == 0x2) {
      if (rxHead_ == rxLen_) rxHead_ = rxLen_ = 0;
      if (rxHead_ > 0) { // compact
        memmove(rx_, rx_ + rxHead_, rxLen_ - rxHead_);
        rxLen_ -= rxHead_;
        rxHead_ = 0;
      }
      if (len > sizeof rx_ - rxLen_) return fail("WebSocket frame larger than the receive buffer");
      if (!readExact(rx_ + rxLen_, len)) return fail("WebSocket read timeout");
      if (masked)
        for (size_t i = 0; i < len; ++i) rx_[rxLen_ + i] ^= mask[i & 3];
      rxLen_ += len;
      return true;
    }
    uint8_t ctl[125];
    if (len > sizeof ctl || !readExact(ctl, len)) return fail("bad WebSocket control frame");
    if (masked)
      for (size_t i = 0; i < len; ++i) ctl[i] ^= mask[i & 3];
    if (op == 0x8) return fail("server closed the WebSocket");
    if (op == 0x9) { // ping -> pong with the same payload
      uint8_t pong[2 + 4 + 125];
      const uint32_t key = ESP.random();
      pong[0] = 0x8A;
      pong[1] = 0x80 | static_cast<uint8_t>(len);
      memcpy(pong + 2, &key, 4);
      for (size_t i = 0; i < len; ++i) pong[6 + i] = ctl[i] ^ pong[2 + (i & 3)];
      c_->write(pong, 6 + len);
    }
    return true; // pong / unknown control: ignored
  }

  bool fail(const char *why) {
    error_ = why;
    close();
    return false;
  }

  bool handshake(const Url &u, uint32_t timeoutMs) {
    uint8_t nonce[16];
    for (size_t i = 0; i < sizeof nonce; i += 4) {
      const uint32_t r = ESP.random();
      memcpy(nonce + i, &r, 4);
    }
    const bool defaultPort = (u.tls && u.port == 443) || (!u.tls && u.port == 80);
    String req;
    req.reserve(256);
    req += "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host;
    if (!defaultPort) req += ":" + String(u.port);
    req += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + base64(nonce, sizeof nonce);
    req += "\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: mqtt\r\n\r\n";
    c_->print(req);

    const uint32_t t0 = millis();
    String status;
    bool first = true;
    while (millis() - t0 < timeoutMs) {
      if (!c_->available()) {
        if (!c_->connected()) break;
        delay(5);
        continue;
      }
      String line = c_->readStringUntil('\n');
      line.trim();
      if (first) {
        status = line;
        first = false;
        continue;
      }
      if (!line.length()) { // end of headers
        if (status.indexOf(" 101") > 0) return true;
        static char msg[64];
        snprintf(msg, sizeof msg, "WebSocket refused: %.40s", status.c_str());
        error_ = msg;
        return false;
      }
    }
    error_ = "WebSocket handshake timeout";
    return false;
  }

  WiFiClient plain_;
  std::unique_ptr<BearSSL::WiFiClientSecure> secure_;
  WiFiClient *c_ = nullptr;
  bool ws_ = false;
  uint8_t rx_[320];
  size_t rxHead_ = 0, rxLen_ = 0;
  const char *error_ = "";
};

} // namespace FmLink

namespace RemoteLog {

// Names are prefixed: ESP8266 headers define plain ERROR / DEBUG.
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
static constexpr uint32_t kPingMs = 20000;       // liveness ping while connected
static constexpr uint32_t kSilentMs = 45000;     // nothing heard -> reconnect
static constexpr uint32_t kConnectMs = 10000;    // TCP/TLS/WebSocket/CONNACK budget
static constexpr uint32_t kAckTimeoutMs = 15000; // log batch not acked -> reconnect + resend

struct State {
  FmLink::Link link;
  bool started = false;
  bool suspended = false;
  bool connected = false;   // MQTT session up
  bool everConnected = false;
  bool refused = false;     // broker rejected the login
  uint8_t refusedRc = 0;
  uint32_t refusedAt = 0;
  bool force = false;       // flush(): send now, ignore batching delay
  bool planned = false;     // current reconnect was caused by suspend()
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

  uint32_t lastHeard = 0;
  uint32_t lastPing = 0;
  uint32_t lastAttempt = 0;
  uint32_t upSince = 0;
  uint32_t lastHeapWarn = 0;
  uint32_t failures = 0; // failed connection attempts since the last success
  uint32_t sessions = 0; // successful MQTT sessions since boot

  bool restartRequested = false;
  char restartId[24] = "";

  uint8_t tx[kBatchBytes + 96];
};
static_assert(sizeof(State::tx) <= FmLink::Link::kMaxOut, "tx packet must fit one link write");
inline State &S() {
  static State s;
  return s;
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
// Format strings are placed in flash (PSTR) to save RAM.
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
// "key":"info"            -> threshold (error+warn+info), "off" = none
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

inline bool send(const uint8_t *p, size_t n) { return S().link.write(p, n); }

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
  if (!S().connected) return;
  char r[48], s[48], json[200];
  levelsJson(r, sizeof r, S().remote);
  levelsJson(s, sizeof s, S().serial);
  int n = snprintf(json, sizeof json,
                   "{\"state\":\"online\",\"fw\":\"%s\",\"remote\":%s,\"serial\":%s,\"caps\":[\"restart\"]}",
                   S().fw, r, s);
  publish(S().tStatus, json, n, 1, true);
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
  if (qos) {
    if (off + 2 > rem) return;
    const uint8_t ack[4] = {0x40, 0x02, body[off], body[off + 1]};
    send(ack, sizeof ack);
    off += 2;
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

/** Read one whole packet if one has started arriving. false = none / dropped / oversized. */
inline bool readPacket(uint8_t &type, uint8_t *body, size_t cap, size_t &len) {
  if (S().link.available() <= 0) return false;
  const int t = S().link.read(1000);
  if (t < 0) return false;
  size_t n = 0, mult = 1;
  for (int i = 0; i < 4; ++i) {
    const int b = S().link.read(2000);
    if (b < 0) return false;
    n += static_cast<size_t>(b & 0x7F) * mult;
    mult *= 128;
    if (!(b & 0x80)) break;
  }
  for (size_t i = 0; i < n; ++i) {
    const int b = S().link.read(2000);
    if (b < 0) return false;
    if (i < cap) body[i] = static_cast<uint8_t>(b);
  }
  type = static_cast<uint8_t>(t);
  len = n < cap ? n : cap;
  return n <= cap; // oversized packets are drained and ignored
}

inline void handlePackets() {
  static uint8_t body[256];
  uint8_t type;
  size_t n;
  for (int guard = 0; guard < 8 && S().connected && S().link.available() > 0; ++guard) {
    if (!readPacket(type, body, sizeof body, n)) continue;
    S().lastHeard = millis();
    switch (type & 0xF0) {
    case 0x40: // PUBACK
      if (n >= 2 && S().inflightLen) {
        const uint16_t pid = (static_cast<uint16_t>(body[0]) << 8) | body[1];
        if (pid == S().inflightId) commitInflight();
      }
      break;
    case 0x30: // PUBLISH
      onPublish(type & 0x0F, body, n);
      break;
    default: // SUBACK, PINGRESP: liveness only
      break;
    }
  }
}

/** Link dropped (or we closed it). Un-acked lines are kept and sent again. */
inline void dropSession(const char *why) {
  const bool wasUp = S().connected;
  const uint32_t upFor = wasUp ? (millis() - S().upSince) / 1000 : 0;
  S().connected = false;
  S().inflightLen = 0;
  S().inflightLines = 0;
  S().link.close();
  S().lastAttempt = millis() ? millis() : 1;
  if (wasUp && why)
    LOGW("RLOG", "MQTT disconnected after %lus: %s (wifi=%d rssi=%d heap=%u maxBlock=%u)",
         static_cast<unsigned long>(upFor), why, static_cast<int>(WiFi.status()),
         WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0, ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
}

// Clean close: MQTT DISCONNECT first, so the broker doesn't publish the LWT
// or write into a socket that vanished.
inline void closeClean() {
  if (S().connected && S().link.connected()) {
    static const uint8_t kDisc[2] = {0xE0, 0x00};
    send(kDisc, sizeof kDisc);
    delay(20);
  }
  S().connected = false;
  S().inflightLen = 0;
  S().inflightLines = 0;
  S().link.close();
}

inline void connectFailed(const char *why) {
  S().link.close();
  ++S().failures;
  // First failure of a streak, then every 10th: don't fill the log buffer.
  if ((S().failures % 10) == 1)
    LOGW("RLOG", "broker %s: %s (attempt %u, heap=%u maxBlock=%u)", S().uri, why,
         static_cast<unsigned>(S().failures), ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
}

/** Blocking connect: link + CONNECT/CONNACK + subscriptions + status. */
inline bool connect() {
  FmLink::Url u;
  if (!FmLink::parse(S().uri, u)) {
    connectFailed("bad broker URI");
    return false;
  }
  Crumbs::stage("mqtt-connect");
  if (!S().link.open(u, kConnectMs)) {
    connectFailed(S().link.error());
    Crumbs::stage("loop");
    return false;
  }

  // CONNECT: user + pass + retained QoS 1 will "offline" + clean session.
  static const char kWill[] = "{\"state\":\"offline\"}";
  uint8_t *tx = S().tx;
  const size_t cl = strlen(S().clientId), wl = strlen(S().tStatus), ml = sizeof(kWill) - 1,
               ul = strlen(S().user), pl = strlen(S().pass);
  const size_t rem = 10 + (2 + cl) + (2 + wl) + (2 + ml) + (2 + ul) + (2 + pl);
  size_t n = 0;
  tx[n++] = 0x10;
  n += putLen(tx + n, rem);
  n += putStr(tx + n, "MQTT", 4);
  tx[n++] = 0x04;
  tx[n++] = 0x80 | 0x40 | 0x20 | 0x08 | 0x04 | 0x02;
  tx[n++] = kKeepaliveSec >> 8;
  tx[n++] = kKeepaliveSec & 0xFF;
  n += putStr(tx + n, S().clientId, cl);
  n += putStr(tx + n, S().tStatus, wl);
  n += putStr(tx + n, kWill, ml);
  n += putStr(tx + n, S().user, ul);
  n += putStr(tx + n, S().pass, pl);
  if (!send(tx, n)) {
    connectFailed("CONNECT not sent");
    Crumbs::stage("loop");
    return false;
  }

  // CONNACK (the broker may split it over several WebSocket frames).
  const uint32_t t0 = millis();
  uint8_t type = 0, ack[4];
  size_t an = 0;
  bool got = false;
  while (millis() - t0 < kConnectMs) {
    if (readPacket(type, ack, sizeof ack, an)) {
      got = true;
      break;
    }
    if (!S().link.connected()) break;
    ESP.wdtFeed();
    delay(5);
  }
  Crumbs::stage("loop");
  if (!got || (type & 0xF0) != 0x20 || an < 2) {
    connectFailed(S().link.error()[0] ? S().link.error() : "no CONNACK from broker");
    return false;
  }
  const uint8_t rc = ack[1];
  if (rc != 0) {
    S().link.close();
    S().refused = true;
    S().refusedRc = rc;
    S().refusedAt = millis();
    LOGE("RLOG", "broker refused login rc=%u: %s (MAC %s); retrying in %lus", rc,
         rc == 5 ? "this MAC isn't registered in FirmwareManager -> Devices"
         : rc == 4 ? "check MQTT_USERNAME / MQTT_PASSWORD"
         : rc == 2 ? "client id refused (must be fm-<mac>)" : "login rejected",
         S().id, static_cast<unsigned long>(REMOTE_LOG_REFUSED_BACKOFF_MS / 1000));
    return false;
  }

  // SUBSCRIBE cmd + ctl, QoS 1.
  const size_t a = strlen(S().tCmd), b = strlen(S().tCtl);
  n = 0;
  tx[n++] = 0x82;
  n += putLen(tx + n, 2 + (2 + a + 1) + (2 + b + 1));
  const uint16_t id = newId();
  tx[n++] = id >> 8;
  tx[n++] = id & 0xFF;
  n += putStr(tx + n, S().tCmd, a);
  tx[n++] = 1;
  n += putStr(tx + n, S().tCtl, b);
  tx[n++] = 1;
  send(tx, n);

  S().connected = true;
  S().everConnected = true;
  S().refused = false;
  S().upSince = S().lastHeard = S().lastPing = millis();
  S().failures = 0;
  ++S().sessions;
  publishStatus();
  if (S().planned)
    LOGD("RLOG", "MQTT reconnected after suspend (session #%u, heap=%u maxBlock=%u)",
         static_cast<unsigned>(S().sessions), ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
  else
    LOGI("RLOG", "MQTT connected to %s (session #%u, heap=%u maxBlock=%u)", S().uri,
         static_cast<unsigned>(S().sessions), ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
  S().planned = false;
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
  if (!publish(S().tLog, S().buf, plen, 1, false, &pid)) {
    dropSession("log publish failed");
    return;
  }
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

// Start the MQTT client (connects from loop()). Strings are copied.
inline bool begin(const char *uri, const char *username, const char *password, const char *fwVersion) {
  if (S().started) return true;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(S().id, sizeof S().id, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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
  FmLink::Url u;
  if (!FmLink::parse(S().uri, u)) return false;
  S().started = true;
  S().lastAttempt = 0; // connect on the next loop()
  return true;
}

// Switch broker at runtime.
inline void setUri(const char *uri) {
  detail::closeClean();
  snprintf(S().uri, sizeof S().uri, "%s", uri ? uri : "");
  S().failures = 0;
  S().lastAttempt = 0;
}
inline const char *uri() { return S().uri; }

// Call from loop() (MqttLogging::loop() does it).
inline void loop() {
  if (!S().started || S().suspended) return;
  uint32_t now = millis();

  if (S().connected) {
    if (!S().link.connected()) {
      detail::dropSession(S().link.error()[0] ? S().link.error() : "connection closed by server");
      return;
    }
    detail::handlePackets();
    // handlePackets() stamps lastHeard with a fresh millis(); a stale `now`
    // would make now - lastHeard wrap to ~4294967s and force a reconnect.
    now = millis();
    if (!S().connected) return;
    if (!S().link.connected()) {
      detail::dropSession(S().link.error()[0] ? S().link.error() : "connection closed by server");
      return;
    }
    if (now - S().lastPing >= kPingMs) {
      S().lastPing = now;
      if (!detail::publish(S().tPing, "1", 1, 1, false)) {
        detail::dropSession("ping not sent");
        return;
      }
    }
    if (now - S().lastHeard >= kSilentMs) {
      LOGW("RLOG", "no reply from server for %lus - reconnecting",
           static_cast<unsigned long>((now - S().lastHeard) / 1000));
      detail::closeClean();
      S().lastAttempt = 0;
      return;
    }
    if (S().inflightLen && now - S().inflightAt >= kAckTimeoutMs) {
      LOGW("RLOG", "log batch not acknowledged in %lus - reconnecting",
           static_cast<unsigned long>(kAckTimeoutMs / 1000));
      detail::closeClean();
      S().lastAttempt = 0;
      return;
    }
    detail::sendBatch();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) return;
  // Login refused: wait, or the server locks this IP out (10 failures / 5 min).
  if (S().refused && now - S().refusedAt < REMOTE_LOG_REFUSED_BACKOFF_MS) return;
  if (S().lastAttempt && now - S().lastAttempt < REMOTE_LOG_RETRY_MS) return;

  // A TLS handshake without a big enough free block crashes or hangs.
  const bool tls = strncmp(S().uri, "wss://", 6) == 0 || strncmp(S().uri, "mqtts://", 8) == 0;
  if (tls && ESP.getMaxFreeBlockSize() < TLS_MIN_FREE_BLOCK) {
    if (now - S().lastHeapWarn >= 60000) {
      S().lastHeapWarn = now;
      LOGW("RLOG", "not connecting: largest free block %u < %u needed for TLS (heap=%u frag=%u%%)",
           ESP.getMaxFreeBlockSize(), static_cast<unsigned>(TLS_MIN_FREE_BLOCK), ESP.getFreeHeap(),
           ESP.getHeapFragmentation());
    }
    S().lastAttempt = now ? now : 1;
    return;
  }

  S().lastAttempt = now ? now : 1;
  if (detail::connect()) detail::sendBatch();
  S().lastAttempt = millis() ? millis() : 1; // back-off counts from the end of the attempt
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
  S().planned = S().connected;
  detail::closeClean();
}
inline void resume() {
  if (!S().suspended) return;
  S().suspended = false;
  S().lastAttempt = 0; // reconnect right away
}

inline bool connected() { return S().connected; }
inline bool started() { return S().started; }
inline bool suspended() { return S().suspended; }
inline bool everConnected() { return S().everConnected; }
inline bool refused() { return S().refused; }
// Login refused and waiting out REMOTE_LOG_REFUSED_BACKOFF_MS before retrying.
inline bool backingOff() {
  return S().refused && !S().connected && millis() - S().refusedAt < REMOTE_LOG_REFUSED_BACKOFF_MS;
}
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
