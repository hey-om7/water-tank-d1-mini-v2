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
 * SPIKE FILTER (all day, every reading):
 * - Every incoming reading is compared against the rolling median of the
 *   last SPIKE_LOOKBACK accepted clean values.
 * - If the new value deviates from that median by more than SPIKE_MAX_DELTA
 *   in either direction (sudden jump up OR sudden drop), it is clamped to
 *   the median — it is NOT stored as-is.
 * - This runs 24 hours a day, not just during a specific time window.
 * - Gradual real changes (tank filling or draining slowly) pass through
 *   because each small step updates the median incrementally.
 *
 * FILLING DETECTION:
 * - "Is Filling" = "Yes" when the accepted (post-filter) level rose ≥ 2%
 *   from the previous row.
 *
 * CALIBRATION RESET:
 * - GET ?action=resetCalibration clears the stored baseline so the filter
 *   re-seeds itself from the next few real readings.
 *   Use the "Reset Calibration" button in the dashboard config panel.
 */

// ── Spike filter constants ────────────────────────────────────────────────────
// Max deviation (%) from the rolling median that is accepted as a real change.
// A single reading that jumps more than this in either direction is clamped.
// Set high enough that a real fast fill (tank filling in ~30 min) still passes:
//   30% fill over 6 readings = ~5%/reading → threshold of 10 gives headroom.
var SPIKE_MAX_DELTA = 10.0;

// How many recent accepted values to include in the rolling median baseline.
var SPIKE_LOOKBACK = 6;

// Minimum level rise (%) between two consecutive rows to count as filling.
var FILL_RISE_THRESHOLD = 2.0;

// Name of the Apps Script property used to persist the baseline across requests.
var BASELINE_PROP = 'spikeBaseline';

// ── Helpers ───────────────────────────────────────────────────────────────────

function calcMedian(arr) {
  if (arr.length === 0) return null;
  var sorted = arr.slice().sort(function(a, b) { return a - b; });
  var mid = Math.floor(sorted.length / 2);
  return sorted.length % 2 !== 0
    ? sorted[mid]
    : (sorted[mid - 1] + sorted[mid]) / 2;
}

/**
 * Load the persisted rolling baseline from Script Properties.
 * Returns an array of up to SPIKE_LOOKBACK numbers, or [] if not set.
 */
function loadBaseline() {
  var raw = PropertiesService.getScriptProperties().getProperty(BASELINE_PROP);
  if (!raw) return [];
  try {
    var arr = JSON.parse(raw);
    return Array.isArray(arr) ? arr : [];
  } catch (e) {
    return [];
  }
}

/**
 * Persist the rolling baseline to Script Properties.
 */
function saveBaseline(arr) {
  PropertiesService.getScriptProperties().setProperty(
    BASELINE_PROP, JSON.stringify(arr.slice(-SPIKE_LOOKBACK))
  );
}

/**
 * Given a candidate level value and the current rolling baseline array,
 * returns the accepted (possibly clamped) value and the updated baseline.
 *
 * Logic:
 *  - If baseline is empty (first readings after reset), accept as-is and seed.
 *  - If |value - median(baseline)| > SPIKE_MAX_DELTA → spike: clamp to median.
 *  - Otherwise accept the value and push it into the baseline.
 */
function applySpikeFiler(value, baseline) {
  if (baseline.length < 2) {
    // Not enough history yet — accept and seed
    var newBaseline = baseline.concat([value]);
    if (newBaseline.length > SPIKE_LOOKBACK) newBaseline.shift();
    return { accepted: value, baseline: newBaseline };
  }

  var med = calcMedian(baseline);
  var delta = Math.abs(value - med);

  if (delta > SPIKE_MAX_DELTA) {
    // Spike detected — clamp to median, do NOT update baseline with bad value
    return { accepted: med, baseline: baseline.slice() };
  }

  // Clean value — accept and update baseline
  var newBaseline = baseline.concat([value]);
  if (newBaseline.length > SPIKE_LOOKBACK) newBaseline.shift();
  return { accepted: value, baseline: newBaseline };
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

    // ── All-day spike filter ──────────────────────────────────
    var baseline = loadBaseline();
    var filtered = applySpikeFiler(rawLevel, baseline);
    var currentLevel = filtered.accepted;
    saveBaseline(filtered.baseline);

    // ── Is Filling from accepted level rise ───────────────────
    var isFillingValue = 'No';
    var lastRow = sheet.getLastRow();
    if (lastRow >= 2) {
      var prevLevel = parseFloat(sheet.getRange(lastRow, 2).getValue()) || 0;
      if (currentLevel - prevLevel >= FILL_RISE_THRESHOLD) {
        isFillingValue = 'Yes';
      }
    }

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
 *   GET ?action=resetCalibration → clear the persisted spike-filter baseline
 */
function doGet(e) {
  try {
    // ── Calibration reset ─────────────────────────────────────
    if (e && e.parameter && e.parameter.action === 'resetCalibration') {
      PropertiesService.getScriptProperties().deleteProperty(BASELINE_PROP);
      return ContentService
        .createTextOutput(JSON.stringify({ status: 'ok', message: 'Calibration baseline reset.' }))
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
    // Re-apply the spike filter over the stored values so that any rows
    // written before this script version (or during a baseline gap) are
    // also cleaned before being sent to the dashboard.
    var maxRows = 288;
    var startRow = Math.max(2, lastRow - maxRows + 1);
    var numRows = lastRow - startRow + 1;
    var range = sheet.getRange(startRow, 1, numRows, 2);
    var values = range.getValues();

    var result = [];
    var resultBaseline = [];
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
      var rf = applySpikeFiler(lvl, resultBaseline);
      lvl = rf.accepted;
      resultBaseline = rf.baseline;

      result.push({ t: epochSec, l: lvl });
    }

    // ── Last fill session (level-rise based, spike-filtered) ──
    var lastFilled = null;
    var scanRows = Math.min(1000, lastRow - 1);
    var scanStart = Math.max(2, lastRow - scanRows + 1);
    var scanRange = sheet.getRange(scanStart, 1, lastRow - scanStart + 1, 2);
    var scanValues = scanRange.getValues();

    var pts = [];
    var ptsBaseline = [];
    for (var p = 0; p < scanValues.length; p++) {
      var pTs = scanValues[p][0];
      var pEpoch = (pTs instanceof Date) ? Math.floor(pTs.getTime() / 1000) : Math.floor(pTs);
      var pLvl = parseFloat(scanValues[p][1]) || 0;
      if (pEpoch <= 0) continue;

      var pf = applySpikeFiler(pLvl, ptsBaseline);
      pLvl = pf.accepted;
      ptsBaseline = pf.baseline;

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

      var fromLevel = pts[sessionStartIdx - 1].l;
      var toLevel   = pts[sessionEndIdx].l;
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

    // ── 7-day usage (peak-to-trough per day, spike-filtered) ──
    var usageRows = Math.min(2500, lastRow - 1);
    var usageStart = Math.max(2, lastRow - usageRows + 1);
    var usageRange = sheet.getRange(usageStart, 1, lastRow - usageStart + 1, 2);
    var usageValues = usageRange.getValues();

    var dailyReadings = {};
    var uPrevLvl = null;
    var uBaseline = [];

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

      // Spike filter
      var uf = applySpikeFiler(uLvl, uBaseline);
      uLvl = uf.accepted;
      uBaseline = uf.baseline;

      // Also reject single-interval drop > 30% (sensor glitch)
      if (uPrevLvl !== null && (uPrevLvl - uLvl) > 30) {
        uLvl = uPrevLvl;
      }
      uPrevLvl = uLvl;

      var uDate = new Date(uEpoch * 1000);
      var y = uDate.getFullYear();
      var mo = ('0' + (uDate.getMonth() + 1)).slice(-2);
      var dy = ('0' + uDate.getDate()).slice(-2);
      var dayStr = y + '-' + mo + '-' + dy;

      if (!dailyReadings[dayStr]) {
        dailyReadings[dayStr] = { readings: [], ts: uEpoch };
      }
      dailyReadings[dayStr].readings.push(uLvl);
    }

    // Per-day: peak − trough-after-peak, capped at 100%
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
