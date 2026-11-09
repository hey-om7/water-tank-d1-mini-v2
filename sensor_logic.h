/*
 * sensor_logic.h — Ultrasonic Sensor (Minimal)
 * ===============================================
 * JSN-SR04T ultrasonic distance sensor — simple read + spike reject.
 * Reads every 30s, 3-sample burst median, spike rejection, publish.
 */

#ifndef SENSOR_LOGIC_H
#define SENSOR_LOGIC_H

#include "globals.h"

// ─── Initialize Sensor Pins ──────────────────────────────────
inline void initSensor() {
  pinMode(TRIGGER_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIGGER_PIN, LOW);
  printLog("[SENSOR] Pins initialized");
}

// ─── Read Raw Distance (3-sample burst) ──────────────────────
inline float readRawDistance() {
  const int BURST_SAMPLES = 3;
  float samples[BURST_SAMPLES];
  int validCount = 0;

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
      }
    }

    if (i < BURST_SAMPLES - 1) delay(333);
  }

  if (validCount == 0) return -1;
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
      // Accept after 3 consecutive rejections
      lastValidDistance = rawDist;
      spikeCount = 0;
      return rawDist;
    }
    return -1; // Reject
  }

  lastValidDistance = rawDist;
  spikeCount = 0;
  return rawDist;
}

// ─── Calculate Water Level Percentage ────────────────────────
inline float calculateLevel(float distance) {
  if (distance <= 0) return 0;
  if (config.tankEmpty <= config.tankFull) return 0;

  float level = 100.0 * (config.tankEmpty - distance) /
                (config.tankEmpty - config.tankFull);

  if (level < 0) level = 0;
  if (level > 100) level = 100;
  return level;
}

// ─── Take Sensor Reading and Publish ─────────────────────────
inline void takeSensorReading() {
  ESP.wdtFeed();
  yield();

  float raw = readRawDistance();
  float cleaned = rejectSpike(raw);

  if (cleaned > 0) {
    currentDistance = cleaned;
    currentLevel = calculateLevel(currentDistance);
  }

  ESP.wdtFeed();

  printLog("[SENSOR] Raw:%.1f Cleaned:%.1f Level:%.1f%%",
           raw, cleaned, currentLevel);
}

#endif // SENSOR_LOGIC_H
