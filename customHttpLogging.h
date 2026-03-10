#ifndef HTTPLOGGING_H
#define HTTPLOGGING_H

#include "globals.h"
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <stdarg.h>

bool REMOTE_LOGGING = false;

// -------------------
// HTTP logging function
// -------------------
int loggHttp(const String &value) {
  HTTPClient http;

  http.setTimeout(2000); // 2-second timeout (recommended)

  WiFiClient client;
  http.begin(client, baseLoggingUrl);
  http.addHeader("Content-Type", "text/plain");

  int httpCode = http.POST(value);

  if (httpCode == 200) {
    Serial.println("Successfully logged: " + value);
  } else {
    Serial.println("Error logging: " + String(httpCode));
  }

  http.end();
  return httpCode;
}

// -------------------
// Unified print function (String version)
// -------------------
inline void printLogStr(const String &msg) {
  if (REMOTE_LOGGING) {
    int r = loggHttp(msg);
    (void)r; // suppress unused variable warning
  } else {
    Serial.print(msg);
  }
}

// -------------------
// Unified print function (printf style version)
// -------------------
inline void printLog(const char *format, ...) {
  char loc_buf[64];
  char *temp = loc_buf;
  va_list arg;
  va_list copy;
  va_start(arg, format);
  va_copy(copy, arg);
  int len = vsnprintf(temp, sizeof(loc_buf), format, copy);
  va_end(copy);
  if (len < 0) {
    va_end(arg);
    return;
  }
  if (len >= sizeof(loc_buf)) {
    temp = (char *)malloc(len + 1);
    if (temp == NULL) {
      va_end(arg);
      return;
    }
    vsnprintf(temp, len + 1, format, arg);
  }
  va_end(arg);

  String msg = String(temp);
  if (!msg.endsWith("\n")) {
    msg += "\n";
  }

  printLogStr(msg);

  if (temp != loc_buf) {
    free(temp);
  }
}

inline void printLogForce(const String &msg) {
  int r = loggHttp(msg);
  (void)r; // suppress unused variable warning
}

#endif // HTTPLOGGING_H