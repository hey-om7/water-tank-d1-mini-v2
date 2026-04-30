/*
 * sensor_logic.h — Ultrasonic Sensor & Fill Detection
 * =====================================================
 * JSN-SR04T ultrasonic distance sensor with multi-stage filtration:
 *
 * Pipeline (firmware side):
 *  Every 60s : 3-sample burst → average → spike reject → store in 5-slot buffer
 *  Every 5min: compute MEDIAN of the 5 buffer slots → publish to dashboard & Sheets
 *
 * The median eliminates any remaining outlier in the 5-minute window before
 * the value is sent to Google Sheets for server-side EMA + hysteresis filtering.
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

// ─── Initialize Fill Detection History ───────────────────────
inline void initFillDetection() {
  for (int i = 0; i < FILL_WINDOW; i++)
    levelHistory[i] = -1;
  printLog("[SENSOR] Fill detection initialized");
}

// ─── Initialize Filter State ────────────────────────────────
inline void initFilter() {
  for (int i = 0; i < FILTER_BUFFER_SIZE; i++)
    filterBuffer[i] = 0;
  filterBufIdx = 0;
  filterBufCount = 0;
  lastValidDistance = -1;
  currentRawDistance = 0;
  spikeCount = 0;
  printLog("[SENSOR] Filter initialized");
}

// ─── Read Raw Distance (3-sample average over ~1 second) ─────
// Takes 3 rapid samples spaced ~333ms apart (totalling ~1 second),
// discards any out-of-range readings, and returns the average of
// the valid ones. This averages out sub-second sensor noise before
// the reading enters the spike-rejection and EMA pipeline.
inline float readRawDistance() {
  const int BURST_SAMPLES = 3;
  float samples[BURST_SAMPLES];
  int validCount = 0;

  for (int i = 0; i < BURST_SAMPLES; i++) {
    // Trigger pulse
    digitalWrite(TRIGGER_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIGGER_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIGGER_PIN, LOW);

    // Read echo (50ms timeout)
    long duration = pulseIn(ECHO_PIN, HIGH, 50000);

    if (duration > 0) {
      float dist = (duration * 0.0343) / 2.0;
      if (dist > 2 && dist < 500) {
        samples[validCount++] = dist;
      }
    }

    // Space samples ~333ms apart so 3 samples span ~1 second
    if (i < BURST_SAMPLES - 1) delay(333);
  }

  if (validCount == 0) return -1;

  float sum = 0;
  for (int i = 0; i < validCount; i++) sum += samples[i];
  return sum / validCount;
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
    spikeCount++;
    if (spikeCount > 3) {
      // Accept spike after 3 consecutive rejections to unfreeze
      printLog("[SENSOR] Spike accepted (consecutive): %.1f cm\n", rawDist);
      lastValidDistance = rawDist;
      spikeCount = 0;
      return rawDist;
    }
    // Spike detected — reject and return last valid
    printLog("[SENSOR] Spike rejected: %.1f cm (diff=%.1f from %.1f)\n",
                  rawDist, diff, lastValidDistance);
    return -1; // Signal bad reading
  }

  // Good reading
  lastValidDistance = rawDist;
  spikeCount = 0;
  return rawDist;
}

// ─── Internal Fast Read (called every 2s) ────────────────────
// Reads sensor, filters through spike rejection, stores in buffer for SMA.
inline void internalSensorRead() {
  float raw = readRawDistance();
  
  if (raw > 0) {
    currentRawDistance = raw;
  }
  
  float cleaned = rejectSpike(raw);

  if (cleaned > 0) {
    filterBuffer[filterBufIdx] = cleaned;
    filterBufIdx = (filterBufIdx + 1) % FILTER_BUFFER_SIZE;
    if (filterBufCount < FILTER_BUFFER_SIZE)
      filterBufCount++;
  }

  printLog("[SENSOR] Raw:%.1f Cleaned:%.1f SMA-BufIdx:%d\n", raw, cleaned,
                filterBufIdx);
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

// ─── Publish Stable Reading (called every 5 min) ────────────
// Computes the MEDIAN of the 5-slot filter buffer and publishes it.
// Median is more robust than average — one bad reading in the window
// cannot shift the output, it is simply the middle value when sorted.
inline void publishStableReading() {
  if (filterBufCount == 0) {
    printLog("[SENSOR] No valid data to publish");
    return;
  }

  // Copy valid entries into a temporary array for sorting
  float tmp[FILTER_BUFFER_SIZE];
  int n = filterBufCount;
  for (int i = 0; i < n; i++) {
    tmp[i] = filterBuffer[i];
  }

  // Insertion sort (tiny array — no overhead)
  for (int i = 1; i < n; i++) {
    float key = tmp[i];
    int j = i - 1;
    while (j >= 0 && tmp[j] > key) {
      tmp[j + 1] = tmp[j];
      j--;
    }
    tmp[j + 1] = key;
  }

  // Pick the middle value (median)
  float medianDist = tmp[n / 2];

  currentDistance = medianDist;
  currentLevel    = calculateLevel(currentDistance);
  currentLiters   = calculateLiters(currentDistance);

  printLog("[SENSOR] ─── Published (median): %.1f cm | %.1f%% | %.1f L "
           "(from %d samples) ───\n",
           currentDistance, currentLevel, currentLiters, n);
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
