/*
 * sensor_logic.h — Ultrasonic Sensor & Fill Detection
 * =====================================================
 * JSN-SR04T ultrasonic distance sensor reading with multi-sample
 * averaging, distance-to-level conversion, and fill event detection.
 */

#ifndef SENSOR_LOGIC_H
#define SENSOR_LOGIC_H

#include "globals.h"

// ─── Initialize Sensor Pins ──────────────────────────────────
inline void initSensor() {
  pinMode(TRIGGER_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIGGER_PIN, LOW);
  Serial.println("[SENSOR] Pins initialized");
}

// ─── Initialize Fill Detection History ───────────────────────
inline void initFillDetection() {
  for (int i = 0; i < FILL_WINDOW; i++)
    levelHistory[i] = -1;
  Serial.println("[SENSOR] Fill detection initialized");
}

// ─── Read Distance (Multi-Sample Average) ────────────────────
inline float readDistance() {
  float total = 0;
  int valid = 0;

  for (int i = 0; i < SENSOR_SAMPLES; i++) {
    // Trigger pulse
    digitalWrite(TRIGGER_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIGGER_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIGGER_PIN, LOW);

    // Read echo
    long duration = pulseIn(ECHO_PIN, HIGH, 50000); // 50ms timeout (~8.5m)

    if (duration > 0) {
      float dist = (duration * 0.0343) / 2.0; // Speed of sound = 343 m/s
      if (dist > 2 && dist < 500) {           // Valid range: 2-500cm
        total += dist;
        valid++;
      }
    }
    delay(30); // JSN-SR04T needs ~30ms between readings
  }

  return valid > 0 ? total / valid
                   : currentDistance; // Keep last reading if no valid
}

// ─── Calculate Water Level Percentage ────────────────────────
inline float calculateLevel(float distance) {
  if (config.tankEmpty <= config.tankFull)
    return 0;

  float level = 100.0 * (config.tankEmpty - distance) /
                (config.tankEmpty - config.tankFull);

  // Clamp 0-100
  if (level < 0)
    level = 0;
  if (level > 100)
    level = 100;
  return level;
}

// ─── Update Fill Detection ───────────────────────────────────
inline void updateFillDetection() {
  // Store in circular buffer
  levelHistory[levelHistIdx] = currentLevel;
  levelHistIdx = (levelHistIdx + 1) % FILL_WINDOW;

  // Need at least half the window filled
  int filled = 0;
  for (int i = 0; i < FILL_WINDOW; i++) {
    if (levelHistory[i] >= 0)
      filled++;
  }
  if (filled < FILL_WINDOW / 2)
    return;

  // Find oldest valid entry
  int oldest = levelHistIdx; // Points to oldest entry in circular buffer
  float oldestLevel = levelHistory[oldest];
  if (oldestLevel < 0)
    return;

  float diff = currentLevel - oldestLevel;
  float windowMinutes = (FILL_WINDOW * SENSOR_INTERVAL) / 60000.0;

  if (diff > FILL_THRESHOLD) {
    isFilling = true;
    fillRate = diff / windowMinutes; // %/min
    if (fillRate > 0.01) {
      fillEta = (100.0 - currentLevel) / fillRate;
    } else {
      fillEta = 0;
    }
  } else {
    isFilling = false;
    fillRate = 0;
    fillEta = 0;
  }
}

#endif // SENSOR_LOGIC_H
