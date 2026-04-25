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
 */

// Minimum level rise (%) between two consecutive rows to count as filling
var FILL_RISE_THRESHOLD = 2.0;

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
    // Read the previous row's level (column B) to compare.
    var isFillingValue = "No";
    var lastRow = sheet.getLastRow();
    if (lastRow >= 2) {
      var prevLevel = parseFloat(sheet.getRange(lastRow, 2).getValue()) || 0;
      if (currentLevel - prevLevel >= FILL_RISE_THRESHOLD) {
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
      if (epochSec > 0) {
        result.push({ t: epochSec, l: lvl });
      }
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

    // Build a flat array of { epochSec, level } for easier processing
    var pts = [];
    for (var p = 0; p < scanValues.length; p++) {
      var pTs = scanValues[p][0];
      var pEpoch = (pTs instanceof Date) ? Math.floor(pTs.getTime() / 1000) : Math.floor(pTs);
      var pLvl = parseFloat(scanValues[p][1]) || 0;
      if (pEpoch > 0) {
        pts.push({ t: pEpoch, l: pLvl });
      }
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
    var usageRows = Math.min(2500, lastRow - 1);
    var usageStart = Math.max(2, lastRow - usageRows + 1);
    var usageRange = sheet.getRange(usageStart, 1, lastRow - usageStart + 1, 2);
    var usageValues = usageRange.getValues();

    var dailyUsage = {};
    var lastLvl = null;
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

      var dObj = new Date(uEpoch * 1000);
      var y = dObj.getFullYear();
      var m = ('0' + (dObj.getMonth() + 1)).slice(-2);
      var d = ('0' + dObj.getDate()).slice(-2);
      var dayStr = y + '-' + m + '-' + d;

      if (!dailyUsage[dayStr]) {
        dailyUsage[dayStr] = { drop: 0, ts: uEpoch };
      }

      if (lastLvl !== null) {
        var drop = lastLvl - uLvl;
        // Only count drops (consumption), ignore rises (filling) and sensor glitches > 30%
        if (drop > 0 && drop <= 30) {
          dailyUsage[dayStr].drop += drop;
        }
      }
      lastLvl = uLvl;
    }

    var usage7d = Object.keys(dailyUsage).sort().slice(-7).map(function (k) {
      return { day: k, drop: Math.round(dailyUsage[k].drop), t: dailyUsage[k].ts };
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
