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
 */

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

    // Append data row
    sheet.appendRow([
      new Date(),
      data.level || 0,
      data.distance || 0,
      data.filling ? "Yes" : "No",
      data.fillRate || 0
    ]);

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
 * plus the timestamp of the last time the tank was full (>=95%).
 * 
 * Response format:
 * {
 *   "data": [
 *     { "t": 1741234567, "l": 72.5 },
 *     ...
 *   ],
 *   "lastFull": 1741200000   // Unix epoch of last full event, or null
 * }
 */
function doGet(e) {
  try {
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
    var lastRow = sheet.getLastRow();

    // No data (only header or empty)
    if (lastRow <= 1) {
      return ContentService
        .createTextOutput(JSON.stringify({ status: "ok", data: [], lastFull: null }))
        .setMimeType(ContentService.MimeType.JSON);
    }

    // Get last 288 rows (24h of 5-min intervals), skip header row
    var maxRows = 288;
    var startRow = Math.max(2, lastRow - maxRows + 1); // Row 2 = first data row
    var numRows = lastRow - startRow + 1;

    // Read columns A (Timestamp) and B (Water Level)
    var range = sheet.getRange(startRow, 1, numRows, 2);
    var values = range.getValues();

    var result = [];
    for (var i = 0; i < values.length; i++) {
      var timestamp = values[i][0];
      var level = values[i][1];

      // Convert Date to Unix epoch seconds
      var epochSec = 0;
      if (timestamp instanceof Date) {
        epochSec = Math.floor(timestamp.getTime() / 1000);
      } else if (typeof timestamp === 'number') {
        epochSec = Math.floor(timestamp);
      }

      // Parse level as float
      var lvl = parseFloat(level) || 0;

      if (epochSec > 0) {
        result.push({ t: epochSec, l: lvl });
      }
    }

    // ─── Find Last Fill Session ───────────────────────────────
    // Scan backwards through last 1000 rows to find the most recent
    // fill session. Returns: { startTime, endTime, fromLevel, toLevel, durationMin }
    var lastFilled = null;
    var scanRows = Math.min(1000, lastRow - 1);
    var scanStart = Math.max(2, lastRow - scanRows + 1);
    // Read columns A (Timestamp), B (Level), D (Is Filling)
    var scanRange = sheet.getRange(scanStart, 1, lastRow - scanStart + 1, 4);
    var scanValues = scanRange.getValues();

    // Step 1: Find the last row where filling = "Yes" (end of most recent session)
    var endIdx = -1;
    for (var j = scanValues.length - 1; j >= 0; j--) {
      var filling = String(scanValues[j][3]).trim().toLowerCase();
      if (filling === "yes") {
        endIdx = j;
        break;
      }
    }

    if (endIdx >= 0) {
      // Step 2: Walk backwards from endIdx to find the start of this fill session
      var startIdx = endIdx;
      for (var k = endIdx - 1; k >= 0; k--) {
        var f = String(scanValues[k][3]).trim().toLowerCase();
        if (f === "yes") {
          startIdx = k;
        } else {
          break; // First non-filling row = session started after this
        }
      }

      // Extract timestamps and levels
      var endTs = scanValues[endIdx][0];
      var startTs = scanValues[startIdx][0];
      var fromLevel = parseFloat(scanValues[startIdx][1]) || 0;
      var toLevel = parseFloat(scanValues[endIdx][1]) || 0;

      var endEpoch = (endTs instanceof Date) ? Math.floor(endTs.getTime() / 1000) : Math.floor(endTs);
      var startEpoch = (startTs instanceof Date) ? Math.floor(startTs.getTime() / 1000) : Math.floor(startTs);
      var durationMin = Math.round((endEpoch - startEpoch) / 60);

      // If the row just before the fill session exists, use its level as the true "from"
      if (startIdx > 0) {
        var preLevel = parseFloat(scanValues[startIdx - 1][1]) || 0;
        fromLevel = preLevel;
      }

      lastFilled = {
        startTime: startEpoch,
        endTime: endEpoch,
        fromLevel: Math.round(fromLevel),
        toLevel: Math.round(toLevel),
        durationMin: Math.max(1, durationMin) // At least 1 min
      };
    }

    return ContentService
      .createTextOutput(JSON.stringify({ status: "ok", data: result, lastFilled: lastFilled }))
      .setMimeType(ContentService.MimeType.JSON);

  } catch (error) {
    return ContentService
      .createTextOutput(JSON.stringify({ status: "error", message: error.toString(), data: [], lastFilled: null }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}
