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
  currentRawDistance = -1; // sentinel: "no valid reading yet"
  spikeCount = 0;
  printLog("[SENSOR] Filter initialized");
}

// ─── Read Raw Distance (3-sample burst, ~1 second total) ─────
// Takes 3 samples spaced ~333ms apart. Uses yield() between samples
// so the ESP8266 watchdog and WiFi stack are serviced during the burst.
// This prevents the WDT from firing and keeps the web server responsive.
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

    // Read echo — 30ms timeout (JSN-SR04T max range ~5m = ~30ms round-trip)
    // Shorter than 50ms to reduce worst-case blocking time per sample.
    long duration = pulseIn(ECHO_PIN, HIGH, 30000);

    if (duration > 0) {
      float dist = (duration * 0.0343) / 2.0;
      if (dist > 2 && dist < 500) {
        samples[validCount++] = dist;
      }
    }

    // Feed watchdog and service WiFi/mDNS between samples.
    // delay() on ESP8266 calls yield() internally, keeping the WDT happy
    // and allowing server.handleClient() work to be scheduled.
    if (i < BURST_SAMPLES - 1) delay(333);
  }

  if (validCount == 0) return -1;
  if (validCount == 1) return samples[0];

  // MEDIAN of the burst (not mean). This is the same idea as the outer
  // 5-slot median in publishStableReading, applied at the finer 3-sample
  // granularity: one bad echo inside a burst is *ignored entirely* by
  // the median, whereas an arithmetic mean would absorb ~33% of the
  // outlier and drag the raw value in that direction. Combined with the
  // downstream rejectSpike + 5-slot median this makes the pipeline
  // robust to environmental spikes (evaporation, ripples, condensation)
  // without slowing the response to real level changes — for a clean
  // burst with no outlier, mean and median of 3 give the same answer.
  //
  // Simple insertion sort — validCount is at most BURST_SAMPLES (=3).
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
  // Even count (2): true median = mean of the two middles.
  return (samples[validCount / 2 - 1] + samples[validCount / 2]) / 2.0f;
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

// ─── Internal Read (called every 60s) ────────────────────────
// Performs one 3-sample burst, spike-rejects the result, and stores
// it in the 5-slot circular buffer. Feeds the watchdog before the
// burst so the ~1s blocking time doesn't risk a WDT reset.
inline void internalSensorRead() {
  // Feed watchdog before the ~1s blocking burst read
  ESP.wdtFeed();
  yield();

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

  // Feed watchdog after the burst so the next loop iteration starts clean
  ESP.wdtFeed();

  printLog("[SENSOR] Raw:%.1f Cleaned:%.1f BufIdx:%d\n", raw, cleaned,
                filterBufIdx);
}

// ─── Calculate Water Level Percentage ────────────────────────
inline float calculateLevel(float distance) {
  // Guard: no valid reading yet (sensor hasn't produced a value or
  // returned -1). Without this guard, distance==0 would evaluate to
  // 100.0 * tankEmpty / (tankEmpty - tankFull) > 100%, get clamped to
  // 100, and the dashboard would show "TANK FULL" on boot before any
  // real sensor data has arrived. Return 0 for "no data yet" instead.
  if (distance <= 0)
    return 0;

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

  // Same "no valid reading yet" guard as calculateLevel.
  if (distance <= 0)
    return 0;

  float waterHeight = (float)config.tankEmpty - distance;
  if (waterHeight < 0)
    waterHeight = 0;

  // Cap water height at the tank's physical capacity so a distance
  // reading shorter than tankFull (surface splash, brief overshoot,
  // sensor blind-zone artefact near the full mark) cannot inflate
  // the volume beyond what the tank can physically hold.
  if (config.tankEmpty > config.tankFull) {
    float maxHeight = (float)config.tankEmpty - (float)config.tankFull;
    if (waterHeight > maxHeight)
      waterHeight = maxHeight;
  }

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

  // Pick the middle value (median).
  //
  // For odd n the true median is tmp[n/2]. For even n the true median
  // is the average of the two centre values — the previous code always
  // picked tmp[n/2], which is the *upper* middle, biasing the reported
  // distance high (level low) whenever the buffer wasn't fully filled
  // (boot warm-up, or after rejectSpike dropped a sample). This is the
  // proper median for both parities.
  float medianDist;
  if (n % 2 == 1) {
    medianDist = tmp[n / 2];
  } else {
    medianDist = (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0f;
  }

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
