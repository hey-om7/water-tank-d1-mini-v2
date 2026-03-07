/*
 * sensor_logic.h — Ultrasonic Sensor & Fill Detection
 * =====================================================
 * JSN-SR04T ultrasonic distance sensor with robust 3-stage filtering:
 *  1. Median filter (7 samples) — removes spike outliers
 *  2. Spike rejection — ignores readings >5cm from last valid
 *  3. EMA smoothing — gradual transitions, no jumps
 *
 * Fast internal reads every 2s → published stable value every 30s.
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

// ─── Initialize Filter State ────────────────────────────────
inline void initFilter() {
  for (int i = 0; i < FILTER_BUFFER_SIZE; i++)
    filterBuffer[i] = 0;
  filterBufIdx = 0;
  filterBufCount = 0;
  lastValidDistance = -1;
  emaDistance = -1;
  Serial.println("[SENSOR] Filter initialized");
}

// ─── Bubble Sort (for median) ────────────────────────────────
inline void sortArray(float arr[], int n) {
  for (int i = 0; i < n - 1; i++) {
    for (int j = 0; j < n - i - 1; j++) {
      if (arr[j] > arr[j + 1]) {
        float tmp = arr[j];
        arr[j] = arr[j + 1];
        arr[j + 1] = tmp;
      }
    }
  }
}

// ─── Read Raw Distance (Single Burst, Median of 7) ──────────
// Takes 7 samples, sorts them, returns the median.
// This removes extreme spikes at the hardware level.
inline float readRawDistance() {
  float samples[SENSOR_SAMPLES];
  int validCount = 0;

  for (int i = 0; i < SENSOR_SAMPLES; i++) {
    // Trigger pulse
    digitalWrite(TRIGGER_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIGGER_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIGGER_PIN, LOW);

    // Read echo
    long duration = pulseIn(ECHO_PIN, HIGH, 50000); // 50ms timeout

    if (duration > 0) {
      float dist = (duration * 0.0343) / 2.0; // Speed of sound = 343 m/s
      if (dist > 2 && dist < 500) {           // Valid range: 2-500cm
        samples[validCount++] = dist;
      }
    }
    delay(30); // JSN-SR04T needs ~30ms between readings
  }

  if (validCount == 0) {
    return -1; // No valid readings
  }

  // Sort and return median
  sortArray(samples, validCount);
  return samples[validCount / 2];
}

// ─── Stage 2: Spike Rejection ────────────────────────────────
// Reject readings that jump >SPIKE_THRESHOLD from last valid.
inline float rejectSpike(float rawDist) {
  if (rawDist < 0)
    return -1; // Invalid reading

  // First valid reading ever — accept it
  if (lastValidDistance < 0) {
    lastValidDistance = rawDist;
    return rawDist;
  }

  float diff = abs(rawDist - lastValidDistance);
  if (diff > SPIKE_THRESHOLD) {
    // Spike detected — reject and return last valid
    Serial.printf("[SENSOR] Spike rejected: %.1f cm (diff=%.1f from %.1f)\n",
                  rawDist, diff, lastValidDistance);
    return -1; // Signal bad reading
  }

  // Good reading
  lastValidDistance = rawDist;
  return rawDist;
}

// ─── Stage 3: EMA Smoothing ─────────────────────────────────
// Exponential Moving Average: smoothed = α·new + (1-α)·old
inline float applyEMA(float distance) {
  if (distance < 0)
    return emaDistance; // Keep previous on bad reading

  if (emaDistance < 0) {
    emaDistance = distance; // First reading
    return distance;
  }

  emaDistance = EMA_ALPHA * distance + (1.0 - EMA_ALPHA) * emaDistance;
  return emaDistance;
}

// ─── Internal Fast Read (called every 2s) ────────────────────
// Reads sensor, filters through spike rejection, stores in buffer.
inline void internalSensorRead() {
  float raw = readRawDistance();
  float cleaned = rejectSpike(raw);
  float smoothed = applyEMA(cleaned);

  if (smoothed > 0) {
    filterBuffer[filterBufIdx] = smoothed;
    filterBufIdx = (filterBufIdx + 1) % FILTER_BUFFER_SIZE;
    if (filterBufCount < FILTER_BUFFER_SIZE)
      filterBufCount++;
  }

  Serial.printf("[SENSOR] Raw:%.1f Cleaned:%.1f EMA:%.1f\n", raw, cleaned,
                smoothed);
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

// ─── Calculate Water Volume in Liters ────────────────────────
// Uses tank girth (circumference) to compute cylindrical volume.
// radius = girth / (2π), area = πr², volume = area × waterHeight
// liters = volume / 1000, total = liters × tankCount
inline float calculateLiters(float distance) {
  if (config.tankGirth == 0 || config.tankGirth == 0xFFFF)
    return 0; // Not configured

  float waterHeight = (float)config.tankEmpty - distance;
  if (waterHeight < 0)
    waterHeight = 0;

  float radius = (float)config.tankGirth / (2.0 * 3.14159265);
  float area = 3.14159265 * radius * radius; // cm²
  float volumeCm3 = area * waterHeight;      // cm³
  float liters = volumeCm3 / 1000.0;

  uint8_t count = config.tankCount;
  if (count == 0)
    count = 1;

  return liters * count;
}

// ─── Publish Stable Reading (called every 30s) ──────────────
// Averages the filter buffer to produce the final stable value.
inline void publishStableReading() {
  if (filterBufCount == 0) {
    Serial.println("[SENSOR] No valid data to publish");
    return;
  }

  // Average all values in the buffer
  float sum = 0;
  for (int i = 0; i < filterBufCount; i++) {
    sum += filterBuffer[i];
  }

  currentDistance = sum / filterBufCount;
  currentLevel = calculateLevel(currentDistance);
  currentLiters = calculateLiters(currentDistance);

  Serial.printf("[SENSOR] ─── Published: %.1f cm | %.1f%% | %.1f L (from %d "
                "samples) ───\n",
                currentDistance, currentLevel, currentLiters, filterBufCount);
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
