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
 * FILLING DETECTION LOGIC:
 * - "Is Filling" is set to "Yes" whenever the water level increased by ≥ 2%
 *   compared to the previous row, regardless of what the device reported.
 * - "Last Filled" session is detected purely from level rises of ≥ 2%,
 *   ignoring the "Is Filling" column entirely.
 *
 * NOISE WINDOW (06:30 – 08:30):
 * - During this window the sensor produces random spikes.
 * - A rise is only accepted if it is gradual: < 5% per 5-minute interval.
 * - Any rise ≥ 5% inside this window is treated as a spike:
 *     • the stored level is clamped to the previous row's value, and
 *     • "Is Filling" is forced to "No".
 * - Outside this window the logic is completely unchanged.
 */

// Minimum level rise (%) between two consecutive rows to count as filling
var FILL_RISE_THRESHOLD = 2.0;

// Noise window: 06:30 – 09:00 local time (hours as decimals)
var NOISE_WINDOW_START = 6.5;   // 06:30
var NOISE_WINDOW_END   = 9.0;   // 09:00

// Inside the noise window, a rise is only accepted if it is gradual.
// We compare the incoming value against the MEDIAN of the last
// NOISE_LOOKBACK clean readings (not just the immediately previous row).
// This prevents a partially-passed spike from raising the baseline and
// letting the next spike sneak through.
var NOISE_MAX_GRADUAL_RISE = 5.0;  // % above the rolling median
var NOISE_LOOKBACK         = 5;    // how many recent clean values to median

/**
 * Returns true if the given Date falls inside the noise window (06:30–09:00).
 * Uses the spreadsheet's local timezone automatically (Apps Script Date is local).
 */
function isInNoiseWindow(date) {
  var h = date.getHours() + date.getMinutes() / 60;
  return h >= NOISE_WINDOW_START && h < NOISE_WINDOW_END;
}

/**
 * Compute the median of a numeric array.
 */
function median(arr) {
  if (arr.length === 0) return 0;
  var sorted = arr.slice().sort(function(a, b) { return a - b; });
  var mid = Math.floor(sorted.length / 2);
  return sorted.length % 2 !== 0
    ? sorted[mid]
    : (sorted[mid - 1] + sorted[mid]) / 2;
}

/**
 * Returns true if a level value should be treated as a spike and discarded.
 *
 * @param {Date}   date       - timestamp of the incoming reading
 * @param {number} value      - the incoming level value
 * @param {number[]} recentClean - array of the last N accepted clean values
 *                               (most recent last). Used as the stable baseline.
 *
 * Outside the noise window always returns false.
 * Inside the noise window, fires when value exceeds
 * median(recentClean) + NOISE_MAX_GRADUAL_RISE.
 */
function isSpikeReading(date, value, recentClean) {
  if (!isInNoiseWindow(date)) return false;
  if (recentClean.length === 0) return false;
  var baseline = median(recentClean);
  return (value - baseline) >= NOISE_MAX_GRADUAL_RISE;
}

function doPost(e) {
  try {
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
    var data = JSON.parse(e.postData.contents);

    // Add header row if sheet is empty
    if (sheet.getLastRow() === 0) {
      sheet.appendRow([
        "Timestamp",
        "Water Level (%)",
        "Distance (cm)",
        "Is Filling",
        "Fill Rate (%/min)"
      ]);

      // Format header row
      var headerRange = sheet.getRange(1, 1, 1, 5);
      headerRange.setFontWeight("bold");
      headerRange.setBackground("#4285f4");
      headerRange.setFontColor("#ffffff");
    }

    var currentLevel = Number(data.level) || 0;

    // ── Determine "Is Filling" from level rise, not from device flag ──
    var isFillingValue = "No";
    var lastRow = sheet.getLastRow();
    if (lastRow >= 2) {
      var prevLevel = parseFloat(sheet.getRange(lastRow, 2).getValue()) || 0;
      var rise = currentLevel - prevLevel;

      // ── Noise-window spike filter (rolling-median baseline) ────────
      // Read the last NOISE_LOOKBACK clean values from the sheet to build
      // a stable baseline. Comparing against a median of recent values
      // prevents a partially-passed spike from raising the baseline and
      // letting the next spike sneak through.
      var now = new Date();
      if (isInNoiseWindow(now)) {
        var lookbackCount = Math.min(NOISE_LOOKBACK, lastRow - 1);
        var lookbackStart = lastRow - lookbackCount + 1;
        var recentRange = sheet.getRange(lookbackStart, 2, lookbackCount, 1);
        var recentVals = recentRange.getValues();
        var recentClean = [];
        for (var ri = 0; ri < recentVals.length; ri++) {
          var rv = parseFloat(recentVals[ri][0]);
          if (!isNaN(rv)) recentClean.push(rv);
        }

        if (isSpikeReading(now, currentLevel, recentClean)) {
          // Clamp to the median of recent clean values — more stable than
          // just using the immediately previous row.
          currentLevel = median(recentClean);
          rise = currentLevel - prevLevel;
        }
      }

      if (rise >= FILL_RISE_THRESHOLD) {
        isFillingValue = "Yes";
      }
    }

    // Append data row
    sheet.appendRow([
      new Date(),
      currentLevel,
      Number(data.distance) || 0,
      isFillingValue,
      Number(data.fillRate) || 0
    ]);

    // Force numeric format to correctly display data
    var newRow = sheet.getLastRow();
    sheet.getRange(newRow, 2).setNumberFormat("0.0");   // Water Level (%)
    sheet.getRange(newRow, 3).setNumberFormat("0.0");   // Distance (cm)
    sheet.getRange(newRow, 5).setNumberFormat("0.00");  // Fill Rate (%/min)

    return ContentService
      .createTextOutput(JSON.stringify({ status: "ok" }))
      .setMimeType(ContentService.MimeType.JSON);

  } catch (error) {
    return ContentService
      .createTextOutput(JSON.stringify({ status: "error", message: error.toString() }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}

/**
 * doGet — Returns the last 288 data rows (24h at 5-min intervals) as JSON,
 * plus the most recent fill session detected from level rises.
 *
 * Fill session detection:
 *   A "fill session" is a consecutive run of rows where each row's level
 *   is ≥ FILL_RISE_THRESHOLD higher than the previous row.
 *   The "Is Filling" column is NOT used — only the level values matter.
 *
 * Response format:
 * {
 *   "data": [ { "t": 1741234567, "l": 72.5 }, ... ],
 *   "lastFilled": {
 *     "startTime": 1741200000,
 *     "endTime":   1741201800,
 *     "fromLevel": 40,
 *     "toLevel":   75,
 *     "durationMin": 30,
 *     "refillData": [ { "t": ..., "l": ... }, ... ]
 *   },
 *   "usage7d": [ { "day": "2025-04-26", "drop": 12, "t": 1745625600 }, ... ]
 * }
 */
function doGet(e) {
  try {
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
    var lastRow = sheet.getLastRow();

    // No data (only header or empty)
    if (lastRow <= 1) {
      return ContentService
        .createTextOutput(JSON.stringify({ status: "ok", data: [], lastFilled: null, usage7d: [] }))
        .setMimeType(ContentService.MimeType.JSON);
    }

    // ── Last 288 rows for the 24-h trend chart ────────────────
    var maxRows = 288;
    var startRow = Math.max(2, lastRow - maxRows + 1);
    var numRows = lastRow - startRow + 1;

    var range = sheet.getRange(startRow, 1, numRows, 2);
    var values = range.getValues();

    var result = [];
    var resultRecentClean = []; // rolling baseline — only populated near/in noise window
    for (var i = 0; i < values.length; i++) {
      var timestamp = values[i][0];
      var level = values[i][1];

      var epochSec = 0;
      if (timestamp instanceof Date) {
        epochSec = Math.floor(timestamp.getTime() / 1000);
      } else if (typeof timestamp === 'number') {
        epochSec = Math.floor(timestamp);
      }

      var lvl = parseFloat(level) || 0;
      if (epochSec <= 0) continue;

      var rDate = new Date(epochSec * 1000);
      var rH = rDate.getHours() + rDate.getMinutes() / 60;

      // Keep the rolling baseline fresh: only include readings from the
      // 30-minute pre-window buffer or inside the window itself.
      // Outside that range, clear it so stale afternoon/evening values
      // don't pollute tomorrow's baseline.
      var rInOrNearWindow = (rH >= (NOISE_WINDOW_START - 0.5) && rH < NOISE_WINDOW_END);
      if (!rInOrNearWindow) {
        resultRecentClean = []; // reset — not near the noise window
      }

      // Apply noise-window spike filter (only fires inside window)
      if (resultRecentClean.length > 0) {
        if (isSpikeReading(rDate, lvl, resultRecentClean)) {
          lvl = median(resultRecentClean);
        }
      }

      // Add to rolling baseline only when near/in the window
      if (rInOrNearWindow) {
        resultRecentClean.push(lvl);
        if (resultRecentClean.length > NOISE_LOOKBACK) resultRecentClean.shift();
      }

      result.push({ t: epochSec, l: lvl });
    }

    // ── Find Last Fill Session (level-rise based) ─────────────
    // Scan the last 1000 rows. A fill session is a consecutive run of rows
    // where level[i] - level[i-1] >= FILL_RISE_THRESHOLD.
    // We find the MOST RECENT such session.
    var lastFilled = null;
    var scanRows = Math.min(1000, lastRow - 1);
    var scanStart = Math.max(2, lastRow - scanRows + 1);
    // Read columns A (Timestamp) and B (Level) only — ignore Is Filling column
    var scanRange = sheet.getRange(scanStart, 1, lastRow - scanStart + 1, 2);
    var scanValues = scanRange.getValues();

    // Build a flat array of { epochSec, level } for easier processing.
    // Apply the rolling-median spike filter so historical spike rows
    // don't trigger false fill-session detection.
    var pts = [];
    var ptsRecentClean = []; // rolling baseline — only populated near/in noise window
    for (var p = 0; p < scanValues.length; p++) {
      var pTs = scanValues[p][0];
      var pEpoch = (pTs instanceof Date) ? Math.floor(pTs.getTime() / 1000) : Math.floor(pTs);
      var pLvl = parseFloat(scanValues[p][1]) || 0;
      if (pEpoch <= 0) continue;

      var pDate = new Date(pEpoch * 1000);
      var pH = pDate.getHours() + pDate.getMinutes() / 60;
      var pInOrNearWindow = (pH >= (NOISE_WINDOW_START - 0.5) && pH < NOISE_WINDOW_END);

      if (!pInOrNearWindow) {
        ptsRecentClean = [];
      }

      if (ptsRecentClean.length > 0) {
        if (isSpikeReading(pDate, pLvl, ptsRecentClean)) {
          pLvl = median(ptsRecentClean);
        }
      }

      if (pInOrNearWindow) {
        ptsRecentClean.push(pLvl);
        if (ptsRecentClean.length > NOISE_LOOKBACK) ptsRecentClean.shift();
      }

      pts.push({ t: pEpoch, l: pLvl });
    }

    // Walk backwards through pts to find the most recent fill session end
    // A "fill row" is one where pts[i].l - pts[i-1].l >= FILL_RISE_THRESHOLD
    var sessionEndIdx = -1;
    for (var j = pts.length - 1; j >= 1; j--) {
      if (pts[j].l - pts[j - 1].l >= FILL_RISE_THRESHOLD) {
        sessionEndIdx = j;
        break;
      }
    }

    if (sessionEndIdx >= 1) {
      // Walk backwards from sessionEndIdx to find where this session started
      // (the first consecutive rising row in this run)
      var sessionStartIdx = sessionEndIdx;
      for (var k = sessionEndIdx - 1; k >= 1; k--) {
        if (pts[k].l - pts[k - 1].l >= FILL_RISE_THRESHOLD) {
          sessionStartIdx = k;
        } else {
          break;
        }
      }

      // "from" level = the row just before the first rising row
      var fromLevel = pts[sessionStartIdx - 1].l;
      var toLevel   = pts[sessionEndIdx].l;

      var startEpoch = pts[sessionStartIdx].t;
      var endEpoch   = pts[sessionEndIdx].t;
      var durationMin = Math.max(1, Math.round((endEpoch - startEpoch) / 60));

      // Collect the data points that make up the fill window
      // (include the row before the session for visual context)
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

    // ── Calculate 7-Day Usage ─────────────────────────────────
    // Strategy: for each day, find the highest level reading and the lowest
    // level reading that comes AFTER the highest (i.e. net consumption after
    // the last refill of that day). This is immune to:
    //   • sensor noise / oscillation accumulating via per-interval summing
    //   • refills inflating the drop total
    //   • cross-day lastLvl carry-over errors
    //   • noise-window spikes (filtered below before use)
    //
    // Per-day consumption = peak level − trough level after that peak,
    // capped at 100% (physically impossible to consume more than a full tank).
    var usageRows = Math.min(2500, lastRow - 1);
    var usageStart = Math.max(2, lastRow - usageRows + 1);
    var usageRange = sheet.getRange(usageStart, 1, lastRow - usageStart + 1, 2);
    var usageValues = usageRange.getValues();

    // Build per-day arrays of clean (spike-filtered) level readings
    var dailyReadings = {};   // { "YYYY-MM-DD": { readings: [], ts } }
    var uPrevLvl = null;
    var uRecentClean = [];    // rolling baseline — only populated near/in noise window

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

      var uDate = new Date(uEpoch * 1000);
      var uH = uDate.getHours() + uDate.getMinutes() / 60;
      var uInOrNearWindow = (uH >= (NOISE_WINDOW_START - 0.5) && uH < NOISE_WINDOW_END);

      // Reset the rolling baseline whenever we're outside the noise window
      // so stale values from the rest of the day don't pollute the baseline
      if (!uInOrNearWindow) {
        uRecentClean = [];
      }

      // Apply noise-window spike filter (isSpikeReading returns false outside window)
      if (uRecentClean.length > 0) {
        if (isSpikeReading(uDate, uLvl, uRecentClean)) {
          uLvl = median(uRecentClean);
        }
      }

      // Also reject any single-interval drop > 30% as a sensor glitch
      if (uPrevLvl !== null && (uPrevLvl - uLvl) > 30) {
        uLvl = uPrevLvl;
      }

      uPrevLvl = uLvl;

      // Only maintain the rolling window when near/in the noise window
      if (uInOrNearWindow) {
        uRecentClean.push(uLvl);
        if (uRecentClean.length > NOISE_LOOKBACK) uRecentClean.shift();
      }

      var y = uDate.getFullYear();
      var mo = ('0' + (uDate.getMonth() + 1)).slice(-2);
      var dy = ('0' + uDate.getDate()).slice(-2);
      var dayStr = y + '-' + mo + '-' + dy;

      if (!dailyReadings[dayStr]) {
        dailyReadings[dayStr] = { readings: [], ts: uEpoch };
      }
      dailyReadings[dayStr].readings.push(uLvl);
    }

    // For each day: consumption = max(level) − min(level after max)
    // This captures "how much was used after the last refill today".
    var usage7d = Object.keys(dailyReadings).sort().slice(-7).map(function (k) {
      var readings = dailyReadings[k].readings;
      if (readings.length < 2) {
        return { day: k, drop: 0, t: dailyReadings[k].ts };
      }

      // Find the index of the peak level for the day
      var peakIdx = 0;
      for (var x = 1; x < readings.length; x++) {
        if (readings[x] > readings[peakIdx]) peakIdx = x;
      }

      // Find the minimum level in all readings after the peak
      var troughAfterPeak = readings[peakIdx];
      for (var y = peakIdx + 1; y < readings.length; y++) {
        if (readings[y] < troughAfterPeak) troughAfterPeak = readings[y];
      }

      var consumption = readings[peakIdx] - troughAfterPeak;

      // Hard cap: can't consume more than 100% of a full tank in one day
      consumption = Math.min(100, Math.max(0, consumption));

      return { day: k, drop: Math.round(consumption), t: dailyReadings[k].ts };
    });

    return ContentService
      .createTextOutput(JSON.stringify({ status: "ok", data: result, lastFilled: lastFilled, usage7d: usage7d }))
      .setMimeType(ContentService.MimeType.JSON);

  } catch (error) {
    return ContentService
      .createTextOutput(JSON.stringify({ status: "error", message: error.toString(), data: [], lastFilled: null, usage7d: [] }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}
