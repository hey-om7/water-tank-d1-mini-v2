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
 * The firmware side already applies a heavy filter chain before every
 * POST (median-of-3 per 3-sample burst → 5 cm spike rejection →
 * median-of-5 over five 60-second slots). Incoming values are therefore
 * already very stable; this server pipeline is intentionally light and
 * only defends against catastrophic glitches while producing a smooth
 * curve for the chart.
 *
 *  Stage 1 — Velocity Clamp (20.0 %/interval)
 *    Hard-blocks any single-interval change larger than 20 %. This is a
 *    safety net for catastrophic sensor glitches only — the firmware
 *    cannot physically produce a real >~14 % delta per 5-min publish
 *    given its own rejectSpike + median window.
 *
 *  Stage 2 — Fine Hysteresis Deadzone (0.5 %)
 *    If the clamped change is smaller than 0.5 %, the stored value is
 *    kept completely flat. Eliminates micro-jitter on an idle tank.
 *
 *  Stage 3 — EMA Smoothing (α = 0.7)
 *    The committed value is blended into the historical trend:
 *      smoothed = 0.7 × committed + 0.3 × previous
 *    Light smoothing — firmware median-of-5 is the primary smoother,
 *    this stage just keeps the chart curve visually continuous.
 *
 * FILLING DETECTION:
 *  "Is Filling" uses the firmware's own `data.filling` flag directly.
 *  The device runs a 10-window (50 min) monotonic-rise fill detector
 *  which sees the true 5-min publish values with full context, and is
 *  the single source of truth. The GAS side does not recompute it.
 *
 * CALIBRATION RESET:
 *  GET ?action=resetCalibration deletes ALL ScriptProperties so the
 *  filter re-homes to the raw sensor value on the very next check-in,
 *  bypassing all stages until the baseline is re-established.
 */

// ── Pipeline constants ────────────────────────────────────────────────────────

var VELOCITY_CLAMP        = 20.0; // Safety net only — max change (%) per 5-min interval.
                                  // Firmware cannot physically produce a real delta
                                  // larger than ~14 %/publish, so this only catches
                                  // catastrophic glitches without limiting real dynamics.
var HYSTERESIS_DEADZONE   = 0.5;  // Changes smaller than this are ignored (kept flat)
var EMA_ALPHA             = 0.7;  // Light EMA — firmware median-of-5 is the primary smoother

// ── Fill-session scan (used by doGet only) ───────────────────────────────────

var FILL_RISE_THRESHOLD   = 2.0;  // Min per-row rise to belong to a fill session

// ── Script Properties keys ────────────────────────────────────────────────────

var PROP_LAST_LEVEL   = 'lastAcceptedLevel';  // EMA-smoothed level persisted between posts

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
 * Run the 2-stage pipeline on a raw incoming level value.
 *
 * The firmware already provides a median-of-5 stable reading, so this
 * server pipeline is intentionally light:
 *   Stage 1 — velocity clamp (catastrophic-glitch safety net)
 *   Stage 2 — hysteresis deadzone (idle-tank micro-jitter suppression)
 * EMA smoothing is applied by applyEMA() afterwards, outside this fn.
 *
 * @param {number}      rawLevel   - incoming level from device (already
 *                                   a firmware-side median of 5 readings)
 * @param {number|null} lastLevel  - previous EMA-smoothed stored level
 *
 * @returns {number|null}
 *   committed value to EMA-blend, or null to keep lastLevel flat
 */
function runPipeline(rawLevel, lastLevel) {

  // First reading after reset — accept raw directly, skip all stages
  if (lastLevel === null) return rawLevel;

  // ── Stage 1: Velocity Clamp ───────────────────────────────
  var delta = rawLevel - lastLevel;
  var clamped;
  if (delta > VELOCITY_CLAMP) {
    clamped = lastLevel + VELOCITY_CLAMP;
  } else if (delta < -VELOCITY_CLAMP) {
    clamped = lastLevel - VELOCITY_CLAMP;
  } else {
    clamped = rawLevel;
  }

  var absDelta = Math.abs(clamped - lastLevel);

  // ── Stage 2: Hysteresis Deadzone ─────────────────────────
  if (absDelta < HYSTERESIS_DEADZONE) {
    // Change too small — keep flat
    return null;
  }

  // Commit directly. Firmware's median-of-5 window already provides the
  // temporal validation this pipeline used to add server-side.
  return clamped;
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
 * Apply the pipeline to a historical level value for doGet chart cleaning.
 * Uses a simplified version (velocity clamp + EMA only — no deadzone or
 * commit logic; historical values are already committed).
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

    // Load persisted state (only the EMA-smoothed level; the temporal
    // pending state and rise buffer are no longer used — see header docs.)
    var lastLevel = loadProp(PROP_LAST_LEVEL, null);

    // ── Run 2-stage pipeline + EMA ────────────────────────────
    var committed = runPipeline(rawLevel, lastLevel);

    var currentLevel;
    if (committed !== null) {
      currentLevel = applyEMA(committed, lastLevel);
    } else {
      // Deadzone — keep previous level flat
      currentLevel = (lastLevel !== null) ? lastLevel : rawLevel;
    }

    // Round to 1 decimal to prevent floating-point drift
    currentLevel = Math.round(currentLevel * 10) / 10;

    // Persist updated state
    saveProp(PROP_LAST_LEVEL, currentLevel);

    // ── Filling flag: firmware is the single source of truth ──
    // The device runs its own 10-window (50 min) monotonic-rise
    // detector on the true 5-min publish stream. We store that flag
    // directly so "Is Filling" and "Fill Rate" columns are consistent.
    var isFillingValue = (data.filling === true) ? 'Yes' : 'No';

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
