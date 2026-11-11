/*
 * sensor_logic.h — Ultrasonic Sensor
 * ===================================
 * JSN-SR04T ultrasonic distance sensor. Pipeline, every 30 s:
 *   3-sample burst median -> spike rejection (distance)
 *   -> level %  -> velocity clamp -> deadzone -> EMA (level smoothing)
 * The level stages are the ones the old Google Sheets script ran
 * server-side; they now run on the device.
 * Fill detection samples the smoothed level every 5 min.
 */

#ifndef SENSOR_LOGIC_H
#define SENSOR_LOGIC_H

#include "custom_mqtt_logging.h"
#include "globals.h"

// ─── Initialize Sensor Pins ──────────────────────────────────
inline void initSensor() {
  pinMode(TRIGGER_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIGGER_PIN, LOW);
  LOGI("SENSOR", "pins initialized (trig=D1 echo=D2)");
}

// ─── Read Raw Distance (3-sample burst) ──────────────────────
inline float readRawDistance() {
  const int BURST_SAMPLES = 3;
  float samples[BURST_SAMPLES];
  int validCount = 0;
  int timeouts = 0, outOfRange = 0;

  for (int i = 0; i < BURST_SAMPLES; i++) {
    digitalWrite(TRIGGER_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIGGER_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIGGER_PIN, LOW);

    long duration = pulseIn(ECHO_PIN, HIGH, 30000);

    if (duration > 0) {
      float dist = (duration * 0.0343) / 2.0;
      if (dist > 2 && dist < 500) {
        samples[validCount++] = dist;
      } else {
        outOfRange++;
        LOGD("SENSOR", "sample %d out of range: %.1fcm", i, dist);
      }
    } else {
      timeouts++;
    }

    if (i < BURST_SAMPLES - 1) delay(333);
  }

  if (timeouts || outOfRange)
    LOGD("SENSOR", "burst: %d valid, %d echo timeouts, %d out of range", validCount, timeouts, outOfRange);

  if (validCount == 0) {
    LOGW("SENSOR", "no valid echo in burst (%d timeouts, %d out of range) - check sensor wiring/power",
         timeouts, outOfRange);
    return -1;
  }
  if (validCount == 1) return samples[0];

  // Sort for median
  for (int i = 1; i < validCount; i++) {
    float key = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > key) {
      samples[j + 1] = samples[j];
      j--;
    }
    samples[j + 1] = key;
  }

  if (validCount % 2 == 1) {
    return samples[validCount / 2];
  }
  return (samples[validCount / 2 - 1] + samples[validCount / 2]) / 2.0f;
}

// ─── Spike Rejection ─────────────────────────────────────────
inline float rejectSpike(float rawDist) {
  if (rawDist < 0) return -1;

  if (lastValidDistance < 0) {
    lastValidDistance = rawDist;
    return rawDist;
  }

  float diff = abs(rawDist - lastValidDistance);
  if (diff > SPIKE_THRESHOLD) {
    spikeCount++;
    if (spikeCount > 3) {
      LOGI("SENSOR", "accepting new baseline %.1fcm after %d consecutive spikes (was %.1fcm)", rawDist,
           spikeCount, lastValidDistance);
      lastValidDistance = rawDist;
      spikeCount = 0;
      return rawDist;
    }
    LOGD("SENSOR", "spike rejected: %.1fcm vs last %.1fcm (#%d)", rawDist, lastValidDistance, spikeCount);
    return -1; // Reject
  }

  lastValidDistance = rawDist;
  spikeCount = 0;
  return rawDist;
}

// ─── Calculate Water Level Percentage ────────────────────────
inline float calculateLevel(float distance) {
  if (distance <= 0) return 0;
  if (config.tankEmpty <= config.tankFull) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      LOGW("SENSOR", "bad calibration: tankEmpty (%u) must be > tankFull (%u)", config.tankEmpty,
           config.tankFull);
    }
    return 0;
  }

  float level = 100.0 * (config.tankEmpty - distance) /
                (config.tankEmpty - config.tankFull);

  if (level < 0) level = 0;
  if (level > 100) level = 100;
  return level;
}

// ─── Level smoothing: velocity clamp -> deadzone -> EMA ──────
inline float &smoothedLevelRef() {
  static float v = -1; // -1 = no baseline yet
  return v;
}

// Forget the baseline (e.g. after recalibration); the next reading is
// accepted as-is.
inline void resetLevelSmoothing() {
  smoothedLevelRef() = -1;
  if (currentDistance > 0) {
    currentLevel = calculateLevel(currentDistance);
    smoothedLevelRef() = currentLevel;
  }
  LOGI("SENSOR", "level smoothing reset, level=%.1f%%", currentLevel);
}

inline float smoothLevel(float rawLevel) {
  float &last = smoothedLevelRef();
  if (last < 0) { // first reading: accept raw, skip all stages
    last = rawLevel;
    return last;
  }

  // 1. Velocity clamp
  float delta = rawLevel - last;
  float clamped = rawLevel;
  if (delta > LEVEL_VELOCITY_CLAMP) clamped = last + LEVEL_VELOCITY_CLAMP;
  else if (delta < -LEVEL_VELOCITY_CLAMP) clamped = last - LEVEL_VELOCITY_CLAMP;
  if (clamped != rawLevel)
    LOGW("SENSOR", "level jump %.1f%% -> %.1f%% clamped to %.1f%%", last, rawLevel, clamped);

  // 2. Deadzone: keep flat on tiny changes
  if (fabsf(clamped - last) < LEVEL_DEADZONE) return last;

  // 3. EMA
  float next = LEVEL_EMA_ALPHA * clamped + (1.0f - LEVEL_EMA_ALPHA) * last;
  next = roundf(next * 10.0f) / 10.0f; // 1 decimal, no float drift
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  last = next;
  return next;
}

// ─── Fill detection (smoothed level, one sample per 5 min) ───
inline void updateFillDetection() {
  static float history[FILL_WINDOW];
  static int idx = 0;
  static bool initialised = false;
  if (!initialised) {
    for (int i = 0; i < FILL_WINDOW; i++) history[i] = -1;
    initialised = true;
  }

  history[idx] = currentLevel;
  idx = (idx + 1) % FILL_WINDOW;

  int filled = 0;
  for (int i = 0; i < FILL_WINDOW; i++)
    if (history[i] >= 0) filled++;
  if (filled < FILL_WINDOW / 2) return;

  // Oldest valid sample (the slot after the newest, skipping empty ones).
  float oldest = -1;
  for (int k = 0; k < FILL_WINDOW && oldest < 0; k++)
    oldest = history[(idx + k) % FILL_WINDOW];
  if (oldest < 0) return;

  const float diff = currentLevel - oldest;
  const float windowMinutes = (filled - 1) * (FILL_SAMPLE_INTERVAL / 60000.0f);
  const bool was = isFilling;
  if (diff > FILL_THRESHOLD && windowMinutes > 0) {
    isFilling = true;
    fillRate = diff / windowMinutes; // %/min
  } else {
    isFilling = false;
    fillRate = 0;
  }
  if (isFilling != was)
    LOGI("FILL", "%s (rise %.1f%% over %.0f min, rate %.2f%%/min)",
         isFilling ? "tank FILLING started" : "filling stopped", diff, windowMinutes, fillRate);
  else
    LOGD("FILL", "diff=%.1f%% window=%.0fmin filling=%d", diff, windowMinutes, isFilling);
}

// ─── Take Sensor Reading ─────────────────────────────────────
inline void takeSensorReading() {
  static uint8_t failStreak = 0;
  Crumbs::stage("sensor");
  ESP.wdtFeed();
  yield();

  float raw = readRawDistance();
  float cleaned = rejectSpike(raw);

  float rawLevel = -1;
  if (cleaned > 0) {
    currentDistance = cleaned;
    rawLevel = calculateLevel(currentDistance);
    currentLevel = smoothLevel(rawLevel);
  }

  if (raw < 0) {
    failStreak++;
    if (failStreak == 10) LOGE("SENSOR", "no valid reading for %d reads in a row - sensor fault?", failStreak);
  } else {
    if (failStreak >= 10) LOGI("SENSOR", "sensor recovered after %d failed reads", failStreak);
    failStreak = 0;
  }

  ESP.wdtFeed();
  LOGD("SENSOR", "raw=%.1fcm cleaned=%.1fcm rawLevel=%.1f%% level=%.1f%%", raw, cleaned, rawLevel, currentLevel);

  static unsigned long lastFillSample = 0;
  static bool firstSample = true;
  if (firstSample || millis() - lastFillSample >= FILL_SAMPLE_INTERVAL) {
    firstSample = false;
    lastFillSample = millis();
    if (currentDistance > 0) updateFillDetection();
  }
  Crumbs::stage("loop");
}

#endif // SENSOR_LOGIC_H
