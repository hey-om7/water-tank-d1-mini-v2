#pragma once
// ---------------------------------------------------------------------------
//  tank_sensor.h - JSN-SR04T ultrasonic level sensor.
//
//  Pipeline, every SENSOR_INTERVAL:
//    3-sample burst median -> spike rejection (distance, cm)
//    -> level % inside [upper gap .. depth - lower gap]
//    -> velocity clamp -> deadzone -> EMA (light smoothing)
//  Fill detection samples the smoothed level every FILL_SAMPLE_INTERVAL.
//
//    TankSensor::begin()   pins (+ optional sensor power switch)
//    TankSensor::loop()    reads when due; cheap otherwise
//    TankSensor::reading() latest values for the web page / API
//    TankSensor::recalibrate()  call after the gaps change
//  HEADER-ONLY.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include "../config/globals.h"
#include "../config/settings.h"
#include "../diag/crumbs.h"
#include "../logging/custom_mqtt_logging.h"

namespace TankSensor {

struct Reading {
  float distance = -1;   // cm, last accepted (spike-filtered) distance; -1 = none yet
  float rawLevel = -1;   // %, before smoothing
  float level = 0;       // %, smoothed (what the UI shows)
  bool valid = false;    // at least one good reading
  bool filling = false;
  float fillRate = 0;    // %/min while filling
  uint32_t readAt = 0;   // millis() of the last attempt
  uint8_t failStreak = 0;
};

inline Reading& reading() {
  static Reading r;
  return r;
}

namespace detail {

struct Filter {
  float lastValid = -1;   // spike filter baseline (cm)
  int spikeCount = 0;
  float smoothed = -1;    // EMA state (%); -1 = no baseline
  float history[FILL_WINDOW];
  int histIdx = 0;
  bool histInit = false;
  uint32_t lastFillSample = 0;
  bool firstFillSample = true;
};
inline Filter& filt() {
  static Filter f;
  return f;
}

inline void sensorPower(bool on) {
#if SENSOR_POWER_PIN >= 0
  digitalWrite(SENSOR_POWER_PIN, on ? SENSOR_POWER_ON_LEVEL : !SENSOR_POWER_ON_LEVEL);
  if (on) delay(SENSOR_WARMUP_MS);
#else
  (void)on;
#endif
}

/** Median of a 3-shot burst, in cm. -1 when no shot returned an echo. */
inline float readRawDistance() {
  const int kShots = 3;
  float s[kShots];
  int n = 0, timeouts = 0, outOfRange = 0;

  sensorPower(true);
  for (int i = 0; i < kShots; i++) {
    digitalWrite(TRIGGER_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIGGER_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIGGER_PIN, LOW);
    const unsigned long us = pulseIn(ECHO_PIN, HIGH, 30000);
    if (us > 0) {
      const float d = us * 0.0343f / 2.0f;
      if (d > 2 && d < 500) s[n++] = d;
      else outOfRange++;
    } else {
      timeouts++;
    }
    if (i < kShots - 1) delay(100);  // let echoes die out (sleeps under light sleep)
  }
  sensorPower(false);

  if (timeouts || outOfRange)
    LOGD("SENSOR", "burst: %d valid, %d timeouts, %d out of range", n, timeouts, outOfRange);
  if (n == 0) return -1;
  // insertion sort, then median
  for (int i = 1; i < n; i++) {
    float k = s[i];
    int j = i - 1;
    while (j >= 0 && s[j] > k) { s[j + 1] = s[j]; j--; }
    s[j + 1] = k;
  }
  return (n % 2) ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2.0f;
}

/** Drops single wild readings; a new level that persists for >3 reads is accepted. */
inline float rejectSpike(float d) {
  Filter& f = filt();
  if (d < 0) return -1;
  if (f.lastValid < 0) { f.lastValid = d; return d; }
  if (fabsf(d - f.lastValid) > SPIKE_THRESHOLD) {
    if (++f.spikeCount > 3) {
      LOGI("SENSOR", "new baseline %.1fcm after %d consecutive jumps (was %.1fcm)", d, f.spikeCount, f.lastValid);
      f.lastValid = d;
      f.spikeCount = 0;
      return d;
    }
    LOGD("SENSOR", "spike rejected: %.1fcm vs %.1fcm (#%d)", d, f.lastValid, f.spikeCount);
    return -1;
  }
  f.lastValid = d;
  f.spikeCount = 0;
  return d;
}

}  // namespace detail

/** Distance (cm) -> 0..100 % inside the gaps. */
inline float levelFor(float distance) {
  if (distance < 0 || !Settings::valid()) return 0;
  const float full = Settings::fullDistance();
  const float empty = Settings::emptyDistance();
  float pct = 100.0f * (empty - distance) / (empty - full);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

namespace detail {

inline float smooth(float raw) {
  float& last = filt().smoothed;
  if (last < 0) { last = raw; return last; }
  float x = raw;
  if (x - last > LEVEL_VELOCITY_CLAMP) x = last + LEVEL_VELOCITY_CLAMP;
  else if (last - x > LEVEL_VELOCITY_CLAMP) x = last - LEVEL_VELOCITY_CLAMP;
  if (x != raw) LOGW("SENSOR", "level jump %.1f%% -> %.1f%% clamped to %.1f%%", last, raw, x);
  if (fabsf(x - last) < LEVEL_DEADZONE) return last;
  float next = LEVEL_EMA_ALPHA * x + (1.0f - LEVEL_EMA_ALPHA) * last;
  next = roundf(next * 10.0f) / 10.0f;
  // Let the display actually reach the ends instead of creeping toward them.
  if (raw >= 99.5f && next > 99.0f) next = 100;
  if (raw <= 0.5f && next < 1.0f) next = 0;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  last = next;
  return next;
}

inline void updateFill() {
  Filter& f = filt();
  Reading& r = reading();
  if (!f.histInit) {
    for (int i = 0; i < FILL_WINDOW; i++) f.history[i] = -1;
    f.histInit = true;
  }
  f.history[f.histIdx] = r.level;
  f.histIdx = (f.histIdx + 1) % FILL_WINDOW;
  int filled = 0;
  for (int i = 0; i < FILL_WINDOW; i++) if (f.history[i] >= 0) filled++;
  if (filled < FILL_WINDOW / 2) return;
  float oldest = -1;
  for (int k = 0; k < FILL_WINDOW && oldest < 0; k++) oldest = f.history[(f.histIdx + k) % FILL_WINDOW];
  if (oldest < 0) return;
  const float diff = r.level - oldest;
  const float minutes = (filled - 1) * (FILL_SAMPLE_INTERVAL / 60000.0f);
  const bool was = r.filling;
  r.filling = diff > FILL_THRESHOLD && minutes > 0;
  r.fillRate = r.filling ? diff / minutes : 0;
  if (r.filling != was)
    LOGI("FILL", "%s (rise %.1f%% over %.0f min)", r.filling ? "tank FILLING" : "filling stopped", diff, minutes);
}

}  // namespace detail

/** One full measurement (blocks ~250 ms). */
inline void measure() {
  Reading& r = reading();
  Crumbs::stage("sensor");
  ESP.wdtFeed();
  const float raw = detail::readRawDistance();
  const float cleaned = detail::rejectSpike(raw);
  r.readAt = millis();
  if (cleaned > 0) {
    r.distance = cleaned;
    r.rawLevel = levelFor(cleaned);
    r.level = detail::smooth(r.rawLevel);
    r.valid = true;
  }
  if (raw < 0) {
    if (++r.failStreak == 10) LOGE("SENSOR", "no echo for 10 reads in a row - check sensor wiring/power");
    if (r.failStreak == 0) r.failStreak = 255;  // saturate
  } else {
    if (r.failStreak >= 10) LOGI("SENSOR", "sensor recovered after %u failed reads", r.failStreak);
    r.failStreak = 0;
  }
  LOGD("SENSOR", "raw=%.1fcm cleaned=%.1fcm rawLevel=%.1f%% level=%.1f%%", raw, cleaned, r.rawLevel, r.level);

  detail::Filter& f = detail::filt();
  if (r.valid && (f.firstFillSample || millis() - f.lastFillSample >= FILL_SAMPLE_INTERVAL)) {
    f.firstFillSample = false;
    f.lastFillSample = millis();
    detail::updateFill();
  }
  Crumbs::stage("loop");
}

inline void begin() {
#if SENSOR_POWER_PIN >= 0
  pinMode(SENSOR_POWER_PIN, OUTPUT);
  detail::sensorPower(false);
#endif
  pinMode(TRIGGER_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIGGER_PIN, LOW);
  LOGI("SENSOR", "JSN-SR04T on trig=D1 echo=D2%s", SENSOR_POWER_PIN >= 0 ? " (switched supply)" : "");
}

inline void loop() {
  static uint32_t last = 0;
  static bool first = true;
  if (first || millis() - last >= SENSOR_INTERVAL) {
    first = false;
    last = millis();
    measure();
  }
}

/** Gaps changed: re-home the smoothing so the new scale shows immediately. */
inline void recalibrate() {
  Reading& r = reading();
  detail::filt().smoothed = -1;
  if (r.distance > 0) {
    r.rawLevel = levelFor(r.distance);
    r.level = detail::smooth(r.rawLevel);
  }
  LOGI("SENSOR", "recalibrated: level=%.1f%%", r.level);
}

}  // namespace TankSensor
