// Google Apps Script for handling ESP32 radar sensor data
// This script receives ON/OFF status from ESP32 and logs it to Google Sheets with timestamp

function doPost(e) {
  try {
    // Get the active spreadsheet (make sure to create one and note the ID)
    const sheet = SpreadsheetApp.getActiveSheet();

    // Get current timestamp
    const timestamp = new Date();

    // Get the status from POST parameters
    const status = e.parameter.status;

    // Validate status
    if (status !== "ON" && status !== "OFF") {
      return ContentService.createTextOutput(
        JSON.stringify({
          result: "error",
          message: "Invalid status. Must be ON or OFF",
        })
      ).setMimeType(ContentService.MimeType.JSON);
    }

    // Get the last row to append new data
    const lastRow = sheet.getLastRow();
    const newRow = lastRow + 1;

    // Add data to the sheet
    // Column A: Timestamp, Column B: Status
    sheet.getRange(newRow, 1).setValue(timestamp);
    sheet.getRange(newRow, 2).setValue(status);

    // Optional: Add formatted timestamp in Column C for better readability
    sheet
      .getRange(newRow, 3)
      .setValue(
        Utilities.formatDate(
          timestamp,
          Session.getScriptTimeZone(),
          "yyyy-MM-dd HH:mm:ss"
        )
      );

    // Log the activity
    console.log(`Status logged: ${status} at ${timestamp}`);

    // Return success response
    return ContentService.createTextOutput(
      JSON.stringify({
        result: "success",
        message: "Status logged successfully",
        timestamp: timestamp,
        status: status,
      })
    ).setMimeType(ContentService.MimeType.JSON);
  } catch (error) {
    console.error("Error processing request:", error);

    return ContentService.createTextOutput(
      JSON.stringify({
        result: "error",
        message: error.toString(),
      })
    ).setMimeType(ContentService.MimeType.JSON);
  }
}

function doGet(e) {
  // Optional: Handle GET requests for testing
  return ContentService.createTextOutput(
    JSON.stringify({
      result: "success",
      message: "Radar sensor logging service is running",
      timestamp: new Date(),
    })
  ).setMimeType(ContentService.MimeType.JSON);
}

// Function to initialize the sheet with headers (run this once manually)
function initializeSheet() {
  const sheet = SpreadsheetApp.getActiveSheet();

  // Set headers
  sheet.getRange(1, 1).setValue("Timestamp");
  sheet.getRange(1, 2).setValue("Status");
  sheet.getRange(1, 3).setValue("Formatted Time");

  // Format header row
  const headerRange = sheet.getRange(1, 1, 1, 3);
  headerRange.setFontWeight("bold");
  headerRange.setBackground("#4285f4");
  headerRange.setFontColor("white");

  // Set column widths
  sheet.setColumnWidth(1, 180);
  sheet.setColumnWidth(2, 80);
  sheet.setColumnWidth(3, 150);

  console.log("Sheet initialized with headers");
}

// Function to get recent status (optional, for monitoring)
function getRecentStatus(limit = 10) {
  const sheet = SpreadsheetApp.getActiveSheet();
  const lastRow = sheet.getLastRow();

  if (lastRow <= 1) {
    return [];
  }

  const startRow = Math.max(2, lastRow - limit + 1);
  const data = sheet
    .getRange(startRow, 1, lastRow - startRow + 1, 3)
    .getValues();

  return data.map((row) => ({
    timestamp: row[0],
    status: row[1],
    formatted: row[2],
  }));
}

// Function to clean old data (optional, run periodically)
function cleanOldData(daysToKeep = 30) {
  const sheet = SpreadsheetApp.getActiveSheet();
  const lastRow = sheet.getLastRow();

  if (lastRow <= 1) {
    return;
  }

  const cutoffDate = new Date();
  cutoffDate.setDate(cutoffDate.getDate() - daysToKeep);

  const data = sheet.getRange(2, 1, lastRow - 1, 1).getValues();

  let deleteCount = 0;
  for (let i = 0; i < data.length; i++) {
    if (data[i][0] < cutoffDate) {
      deleteCount++;
    } else {
      break;
    }
  }

  if (deleteCount > 0) {
    sheet.deleteRows(2, deleteCount);
    console.log(`Deleted ${deleteCount} old records`);
  }
}
