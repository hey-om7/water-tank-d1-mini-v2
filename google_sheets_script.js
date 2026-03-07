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
        "Fill Rate (%/min)",
        "Device Name"
      ]);

      // Format header row
      var headerRange = sheet.getRange(1, 1, 1, 6);
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
      data.fillRate || 0,
      data.deviceName || "WaterTank"
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
 * doGet — Returns the last 288 data rows (24h at 5-min intervals) as JSON.
 * 
 * The dashboard browser fetches this to populate charts with historical data
 * that persists across ESP8266 reboots.
 * 
 * Response format:
 * {
 *   "data": [
 *     { "t": 1741234567, "l": 72.5 },
 *     ...
 *   ]
 * }
 * 
 * Where:
 *   t = Unix epoch seconds (timestamp)
 *   l = Water level percentage
 */
function doGet(e) {
  try {
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
    var lastRow = sheet.getLastRow();

    // No data (only header or empty)
    if (lastRow <= 1) {
      return ContentService
        .createTextOutput(JSON.stringify({ status: "ok", data: [] }))
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

    return ContentService
      .createTextOutput(JSON.stringify({ status: "ok", data: result }))
      .setMimeType(ContentService.MimeType.JSON);

  } catch (error) {
    return ContentService
      .createTextOutput(JSON.stringify({ status: "error", message: error.toString(), data: [] }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}
