#ifndef HTTPLOGGING_H
#define HTTPLOGGING_H

#include "globals.h"
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <stdarg.h>
#include <string.h>

// Remote logging is OFF by default. Enabling it will POST every log
// line to `baseLoggingUrl`, which is expensive and slows the device.
bool REMOTE_LOGGING = false;

// ---------------------------------------------------------------
// Remote HTTP logger (only used when REMOTE_LOGGING is true).
// Heap-safe: takes a raw C string, does NOT create a String copy.
// ---------------------------------------------------------------
inline int loggHttp(const char *value) {
  if (WiFi.status() != WL_CONNECTED) return -1;

  WiFiClient client;
  HTTPClient http;

  http.setTimeout(2000);      // 2-second timeout
  http.setReuse(false);       // no keep-alive; free socket promptly

  if (!http.begin(client, baseLoggingUrl)) return -1;
  http.addHeader("Content-Type", "text/plain");

  int httpCode = http.POST((uint8_t *)value, strlen(value));
  http.end();
  client.stop();

  return httpCode;
}

// ---------------------------------------------------------------
// Unified log function (printf-style).
//
// IMPORTANT: This function is called ~80+ times across the codebase
// and can fire dozens of times per minute. On ESP8266 the heap is
// only ~50 KB, so ANY per-call heap allocation here fragments the
// heap over hours of runtime, eventually preventing BearSSL and the
// web server from allocating their large contiguous buffers.
//
// This implementation is intentionally allocation-free on the hot
// path: it formats into a stack buffer, truncates long lines,
// writes directly to Serial (no String), and never calls malloc().
// ---------------------------------------------------------------
inline void printLog(const char *format, ...) {
  char buf[160];  // ~2 short log lines; enough for any real message

  va_list arg;
  va_start(arg, format);
  int len = vsnprintf(buf, sizeof(buf), format, arg);
  va_end(arg);

  if (len < 0) return;

  // Truncate cleanly if it overflowed the stack buffer
  int usableLen = (len >= (int)sizeof(buf)) ? (int)sizeof(buf) - 1 : len;

  if (REMOTE_LOGGING) {
    // Rare path — remote logging is off by default.
    buf[usableLen] = '\0';
    loggHttp(buf);
    return;
  }

  Serial.write((const uint8_t *)buf, usableLen);
  if (usableLen == 0 || buf[usableLen - 1] != '\n') {
    Serial.write('\n');
  }
}

// ---------------------------------------------------------------
// Force-push a message to the remote logger regardless of the
// REMOTE_LOGGING flag. Rarely used.
// ---------------------------------------------------------------
inline void printLogForce(const char *msg) {
  loggHttp(msg);
}

#endif // HTTPLOGGING_H
