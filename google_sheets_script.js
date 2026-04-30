/**
 * Google Apps Script for Water Tank Monitoring System
 *
 * SETUP INSTRUCTIONS:
 * 1. Create a new Google Sheet
 * 2. Go to Extensions > Apps Script
 * 3. Paste this entire script into the editor
 * 4. Click Deploy > New Deployment
 * 5. Select type: "Web app"
 * 6. Set "Execute as" to "Me"
 * 7. Set "Who has access" to "Anyone"
 * 8. Click Deploy and copy the URL
 * 9. Paste the URL into your Water Tank device configuration panel
 *
 * IMPORTANT: After any code changes, you MUST create a New Version:
 *   Deploy > Manage deployments > Edit (pencil) > Version: New version > Deploy
 *
 * SERVER-SIDE FILTRATION PIPELINE (applied to every incoming reading):
 *
 *  Stage 1 — Velocity Clamp (3.0 %/interval)
 *    Hard-blocks any single-interval change that is physically impossible.
 *    Both rises and drops are clamped symmetrically.
 *
 *  Stage 2 — Fine Hysteresis Deadzone (0.5 %)
 *    If the clamped change is smaller than 0.5 %, the stored value is kept
 *    completely flat. Eliminates micro-jitter on an idle tank.
 *
 *  Stage 3 — Temporal Validation (0.5 % – 1.5 % zone)
 *    Changes in the 0.5–1.5 % band are held as "pending" for one interval.
 *    They are only committed if the NEXT reading also moves in the same
 *    direction, confirming the change is real and not a surface ripple.
 *    Changes > 1.5 % (after the velocity clamp) are committed immediately.
 *
 *  Stage 4 — EMA Smoothing (α = 0.4)
 *    The committed value is blended into the historical trend:
 *      smoothed = 0.4 × committed + 0.6 × previous
 *    Produces a smooth, organic graph curve.
 *
 * FILLING DETECTION:
 *  "Is Filling" = "Yes" only when the EMA-smoothed level has risen by ≥ 3 %
 *  across at least 2 consecutive 5-minute windows (monotonically rising).
 *
 * CALIBRATION RESET:
 *  GET ?action=resetCalibration deletes ALL ScriptProperties so the filter
 *  re-homes to the raw sensor value on the very next check-in, bypassing
 *  all stages until the baseline is re-established.
 */

// ── Pipeline constants ────────────────────────────────────────────────────────

var VELOCITY_CLAMP        = 3.0;  // Max change (%) per 5-min interval (rise or drop)
var HYSTERESIS_DEADZONE   = 0.5;  // Changes smaller than this are ignored (kept flat)
var TEMPORAL_LOWER        = 0.5;  // Bottom of the temporal validation band (%)
var TEMPORAL_UPPER        = 1.5;  // Top of the temporal validation band (%)
var EMA_ALPHA             = 0.4;  // EMA blend factor (0 = no update, 1 = raw value)

// ── Filling detection constants ───────────────────────────────────────────────

var FILL_MIN_RISE_TOTAL   = 3.0;  // Total rise (%) required to declare filling
var FILL_MIN_CONSECUTIVE  = 2;    // Consecutive rising windows required
var FILL_RISE_THRESHOLD   = 2.0;  // Min rise per row for fill-session scan in doGet

// ── Script Properties keys ────────────────────────────────────────────────────

var PROP_LAST_LEVEL   = 'lastAcceptedLevel';  // EMA-smoothed level persisted between posts
var PROP_PENDING      = 'pendingChange';       // Pending temporal-validation value
var PROP_RISE_BUFFER  = 'riseBuffer';          // Recent levels for filling detection

// ── Helpers ───────────────────────────────────────────────────────────────────

function loadProp(key, defaultVal) {
  var raw = PropertiesService.getScriptProperties().getProperty(key);
  if (raw === null) return defaultVal;
  try { return JSON.parse(raw); } catch (e) { return defaultVal; }
}

function saveProp(key, val) {
  PropertiesService.getScriptProperties().setProperty(key, JSON.stringify(val));
}

function calcMedian(arr) {
  if (!arr || arr.length === 0) return null;
  var s = arr.slice().sort(function(a, b) { return a - b; });
  var m = Math.floor(s.length / 2);
  return s.length % 2 !== 0 ? s[m] : (s[m - 1] + s[m]) / 2;
}

/**
 * Run the full 4-stage pipeline on a raw incoming level value.
 *
 * @param {number}      rawLevel   - incoming level from device (already a
 *                                   firmware-side median of 5 readings)
 * @param {number|null} lastLevel  - previous EMA-smoothed stored level
 * @param {object|null} pending    - { value, direction } or null
 *
 * @returns {{ committed: number, newPending: object|null }}
 *   committed  — the value to store and EMA-blend (null = keep lastLevel flat)
 *   newPending — updated pending state to persist
 */
function runPipeline(rawLevel, lastLevel, pending) {

  // ── Stage 1: Velocity Clamp ───────────────────────────────
  var clamped;
  if (lastLevel === null) {
    // First reading after reset — accept raw directly, skip all stages
    return { committed: rawLevel, newPending: null };
  }

  var delta = rawLevel - lastLevel;
  if (delta > VELOCITY_CLAMP) {
    clamped = lastLevel + VELOCITY_CLAMP;
  } else if (delta < -VELOCITY_CLAMP) {
    clamped = lastLevel - VELOCITY_CLAMP;
  } else {
    clamped = rawLevel;
  }

  var clampedDelta = clamped - lastLevel;
  var absDelta = Math.abs(clampedDelta);

  // ── Stage 2: Hysteresis Deadzone ─────────────────────────
  if (absDelta < HYSTERESIS_DEADZONE) {
    // Change too small — keep flat, clear any pending
    return { committed: null, newPending: null };
  }

  // ── Stage 3: Temporal Validation ─────────────────────────
  if (absDelta >= TEMPORAL_LOWER && absDelta < TEMPORAL_UPPER) {
    // In the validation band — needs confirmation from next window
    var direction = clampedDelta > 0 ? 1 : -1;

    if (pending !== null && pending.direction === direction) {
      // Same direction confirmed — commit the pending value
      var confirmed = pending.value;
      return { committed: confirmed, newPending: null };
    } else {
      // First time seeing this change — hold as pending
      return { committed: null, newPending: { value: clamped, direction: direction } };
    }
  }

  // ── Change > TEMPORAL_UPPER: commit immediately ───────────
  // (already passed velocity clamp, so this is a real significant change)
  return { committed: clamped, newPending: null };
}

/**
 * Apply EMA blend.
 * @param {number} committed  - new committed value
 * @param {number} lastLevel  - previous EMA level
 * @returns {number} smoothed value
 */
function applyEMA(committed, lastLevel) {
  if (lastLevel === null) return committed;
  return EMA_ALPHA * committed + (1 - EMA_ALPHA) * lastLevel;
}

/**
 * Determine whether the current level constitutes "Is Filling".
 * Requires FILL_MIN_CONSECUTIVE consecutive rising windows with
 * a total rise of at least FILL_MIN_RISE_TOTAL.
 */
function isFillingNow(currentLevel, riseBuffer) {
  if (riseBuffer.length < FILL_MIN_CONSECUTIVE) return false;

  var window = riseBuffer.concat([currentLevel]);
  var start = window.length - FILL_MIN_CONSECUTIVE - 1;
  if (start < 0) start = 0;
  var slice = window.slice(start);

  for (var i = 1; i < slice.length; i++) {
    if (slice[i] <= slice[i - 1]) return false;
  }

  return (slice[slice.length - 1] - slice[0]) >= FILL_MIN_RISE_TOTAL;
}

/**
 * Apply the pipeline to a historical level value for doGet chart cleaning.
 * Uses a simplified version (velocity clamp + EMA only — no temporal state).
 */
function applyReadFilter(rawValue, lastAccepted) {
  if (lastAccepted === null) return rawValue;

  var delta = rawValue - lastAccepted;
  var clamped;
  if (delta > VELOCITY_CLAMP) {
    clamped = lastAccepted + VELOCITY_CLAMP;
  } else if (delta < -VELOCITY_CLAMP) {
    clamped = lastAccepted - VELOCITY_CLAMP;
  } else {
    clamped = rawValue;
  }

  // Apply EMA
  return EMA_ALPHA * clamped + (1 - EMA_ALPHA) * lastAccepted;
}

// ── doPost ────────────────────────────────────────────────────────────────────

function doPost(e) {
  try {
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
    var data = JSON.parse(e.postData.contents);

    // Add header row if sheet is empty
    if (sheet.getLastRow() === 0) {
      sheet.appendRow([
        'Timestamp',
        'Water Level (%)',
        'Distance (cm)',
        'Is Filling',
        'Fill Rate (%/min)'
      ]);
      var headerRange = sheet.getRange(1, 1, 1, 5);
      headerRange.setFontWeight('bold');
      headerRange.setBackground('#4285f4');
      headerRange.setFontColor('#ffffff');
    }

    var rawLevel = Number(data.level) || 0;

    // Load persisted state
    var lastLevel  = loadProp(PROP_LAST_LEVEL,  null);
    var pending    = loadProp(PROP_PENDING,     null);
    var riseBuffer = loadProp(PROP_RISE_BUFFER, []);

    // ── Run 4-stage pipeline ──────────────────────────────────
    var result = runPipeline(rawLevel, lastLevel, pending);

    var currentLevel;
    if (result.committed !== null) {
      // Blend committed value through EMA
      currentLevel = applyEMA(result.committed, lastLevel);
    } else {
      // Deadzone or pending — keep previous level flat
      currentLevel = (lastLevel !== null) ? lastLevel : rawLevel;
    }

    // Round to 1 decimal to prevent floating-point drift
    currentLevel = Math.round(currentLevel * 10) / 10;

    // Persist updated state
    saveProp(PROP_LAST_LEVEL, currentLevel);
    saveProp(PROP_PENDING,    result.newPending);

    // ── Filling detection ─────────────────────────────────────
    var filling = isFillingNow(currentLevel, riseBuffer);
    riseBuffer.push(currentLevel);
    if (riseBuffer.length > FILL_MIN_CONSECUTIVE + 1) riseBuffer.shift();
    saveProp(PROP_RISE_BUFFER, riseBuffer);

    var isFillingValue = filling ? 'Yes' : 'No';

    // Append row
    sheet.appendRow([
      new Date(),
      currentLevel,
      Number(data.distance) || 0,
      isFillingValue,
      Number(data.fillRate) || 0
    ]);

    var newRow = sheet.getLastRow();
    sheet.getRange(newRow, 2).setNumberFormat('0.0');
    sheet.getRange(newRow, 3).setNumberFormat('0.0');
    sheet.getRange(newRow, 5).setNumberFormat('0.00');

    return ContentService
      .createTextOutput(JSON.stringify({ status: 'ok' }))
      .setMimeType(ContentService.MimeType.JSON);

  } catch (error) {
    return ContentService
      .createTextOutput(JSON.stringify({ status: 'error', message: error.toString() }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}

// ── doGet ─────────────────────────────────────────────────────────────────────

/**
 * Handles two actions:
 *   GET (no params)              → return chart data, lastFilled, usage7d
 *   GET ?action=resetCalibration → wipe ALL ScriptProperties so the filter
 *                                  re-homes to raw sensor value immediately
 */
function doGet(e) {
  try {
    // ── Calibration reset ─────────────────────────────────────
    if (e && e.parameter && e.parameter.action === 'resetCalibration') {
      // Delete ALL properties — not just named ones — so no stale state
      // can interfere with the fresh baseline after reset.
      PropertiesService.getScriptProperties().deleteAllProperties();
      return ContentService
        .createTextOutput(JSON.stringify({
          status: 'ok',
          message: 'All calibration state cleared. Filter will re-home to raw sensor value on next check-in.'
        }))
        .setMimeType(ContentService.MimeType.JSON);
    }

    var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
    var lastRow = sheet.getLastRow();

    if (lastRow <= 1) {
      return ContentService
        .createTextOutput(JSON.stringify({ status: 'ok', data: [], lastFilled: null, usage7d: [] }))
        .setMimeType(ContentService.MimeType.JSON);
    }

    // ── 24-h trend chart data ─────────────────────────────────
    // Re-apply velocity clamp + EMA over stored rows so historical spikes
    // that slipped through before this version are cleaned for the chart.
    var maxRows = 288;
    var startRow = Math.max(2, lastRow - maxRows + 1);
    var numRows = lastRow - startRow + 1;
    var range = sheet.getRange(startRow, 1, numRows, 2);
    var values = range.getValues();

    var result = [];
    var resultLast = null;
    for (var i = 0; i < values.length; i++) {
      var timestamp = values[i][0];
      var level = values[i][1];

      var epochSec = 0;
      if (timestamp instanceof Date) {
        epochSec = Math.floor(timestamp.getTime() / 1000);
      } else if (typeof timestamp === 'number') {
        epochSec = Math.floor(timestamp);
      }
      if (epochSec <= 0) continue;

      var lvl = parseFloat(level) || 0;
      lvl = applyReadFilter(lvl, resultLast);
      lvl = Math.round(lvl * 10) / 10;
      resultLast = lvl;

      result.push({ t: epochSec, l: lvl });
    }

    // ── Last fill session (level-rise based) ──────────────────
    var lastFilled = null;
    var scanRows = Math.min(1000, lastRow - 1);
    var scanStart = Math.max(2, lastRow - scanRows + 1);
    var scanRange = sheet.getRange(scanStart, 1, lastRow - scanStart + 1, 2);
    var scanValues = scanRange.getValues();

    var pts = [];
    var ptsLast = null;
    for (var p = 0; p < scanValues.length; p++) {
      var pTs = scanValues[p][0];
      var pEpoch = (pTs instanceof Date) ? Math.floor(pTs.getTime() / 1000) : Math.floor(pTs);
      var pLvl = parseFloat(scanValues[p][1]) || 0;
      if (pEpoch <= 0) continue;

      pLvl = applyReadFilter(pLvl, ptsLast);
      pLvl = Math.round(pLvl * 10) / 10;
      ptsLast = pLvl;

      pts.push({ t: pEpoch, l: pLvl });
    }

    // Walk backwards to find most recent fill session
    var sessionEndIdx = -1;
    for (var j = pts.length - 1; j >= 1; j--) {
      if (pts[j].l - pts[j - 1].l >= FILL_RISE_THRESHOLD) {
        sessionEndIdx = j;
        break;
      }
    }

    if (sessionEndIdx >= 1) {
      var sessionStartIdx = sessionEndIdx;
      for (var k = sessionEndIdx - 1; k >= 1; k--) {
        if (pts[k].l - pts[k - 1].l >= FILL_RISE_THRESHOLD) {
          sessionStartIdx = k;
        } else {
          break;
        }
      }

      var fromLevel  = pts[sessionStartIdx - 1].l;
      var toLevel    = pts[sessionEndIdx].l;
      var startEpoch = pts[sessionStartIdx].t;
      var endEpoch   = pts[sessionEndIdx].t;
      var durationMin = Math.max(1, Math.round((endEpoch - startEpoch) / 60));

      var refillData = [];
      for (var r = sessionStartIdx - 1; r <= sessionEndIdx; r++) {
        refillData.push({ t: pts[r].t, l: pts[r].l });
      }

      lastFilled = {
        startTime:   startEpoch,
        endTime:     endEpoch,
        fromLevel:   Math.round(fromLevel),
        toLevel:     Math.round(toLevel),
        durationMin: durationMin,
        refillData:  refillData
      };
    }

    // ── 7-day usage (peak-to-trough per day) ──────────────────
    var usageRows = Math.min(2500, lastRow - 1);
    var usageStart = Math.max(2, lastRow - usageRows + 1);
    var usageRange = sheet.getRange(usageStart, 1, lastRow - usageStart + 1, 2);
    var usageValues = usageRange.getValues();

    var dailyReadings = {};
    var uPrevLvl = null;

    for (var i = 0; i < usageValues.length; i++) {
      var uTs = usageValues[i][0];
      var uLvl = parseFloat(usageValues[i][1]) || 0;

      var uEpoch = 0;
      if (uTs instanceof Date) {
        uEpoch = Math.floor(uTs.getTime() / 1000);
      } else if (typeof uTs === 'number') {
        uEpoch = Math.floor(uTs);
      } else {
        continue;
      }
      if (uEpoch <= 0) continue;

      uLvl = applyReadFilter(uLvl, uPrevLvl);
      uLvl = Math.round(uLvl * 10) / 10;

      // Extra guard: reject single-interval drop > 30% (catastrophic glitch)
      if (uPrevLvl !== null && (uPrevLvl - uLvl) > 30) {
        uLvl = uPrevLvl;
      }
      uPrevLvl = uLvl;

      var uDate = new Date(uEpoch * 1000);
      var y  = uDate.getFullYear();
      var mo = ('0' + (uDate.getMonth() + 1)).slice(-2);
      var dy = ('0' + uDate.getDate()).slice(-2);
      var dayStr = y + '-' + mo + '-' + dy;

      if (!dailyReadings[dayStr]) {
        dailyReadings[dayStr] = { readings: [], ts: uEpoch };
      }
      dailyReadings[dayStr].readings.push(uLvl);
    }

    // Per-day: peak − trough-after-peak, capped at 100 %
    var usage7d = Object.keys(dailyReadings).sort().slice(-7).map(function(k) {
      var readings = dailyReadings[k].readings;
      if (readings.length < 2) return { day: k, drop: 0, t: dailyReadings[k].ts };

      var peakIdx = 0;
      for (var x = 1; x < readings.length; x++) {
        if (readings[x] > readings[peakIdx]) peakIdx = x;
      }

      var troughAfterPeak = readings[peakIdx];
      for (var y = peakIdx + 1; y < readings.length; y++) {
        if (readings[y] < troughAfterPeak) troughAfterPeak = readings[y];
      }

      var consumption = Math.min(100, Math.max(0, readings[peakIdx] - troughAfterPeak));
      return { day: k, drop: Math.round(consumption), t: dailyReadings[k].ts };
    });

    return ContentService
      .createTextOutput(JSON.stringify({ status: 'ok', data: result, lastFilled: lastFilled, usage7d: usage7d }))
      .setMimeType(ContentService.MimeType.JSON);

  } catch (error) {
    return ContentService
      .createTextOutput(JSON.stringify({ status: 'error', message: error.toString(), data: [], lastFilled: null, usage7d: [] }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}
