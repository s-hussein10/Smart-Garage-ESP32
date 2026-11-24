/**
 * Smart Garage Pro — Google Sheets Live Logger (Apps Script)
 * Includes: Trucks Yard (Max 6), Ground Floor (Max 10), First Floor (Max 10),
 * DHT11 (Temp + Humidity), MQ Smoke, and Flame Sensors (GF & FF).
 */

function doPost(e) {
  try {
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();

    // Auto-create header row if sheet is empty
    if (sheet.getLastRow() === 0) {
      sheet.appendRow([
        "Timestamp",
        "Action",
        "Truck_ID",
        "RFID_UID",
        "Driver_Name",
        "Vehicle_Type",
        "Trucks_Yard (Max 6)",
        "Ground_Floor (Max 10)",
        "First_Floor (Max 10)",
        "Total_Inside (Max 26)",
        "GF_Temp_C",
        "GF_Hum_Pct",
        "GF_Smoke_Pct",
        "GF_Flame",
        "FF_Temp_C",
        "FF_Hum_Pct",
        "FF_Smoke_Pct",
        "FF_Flame",
        "Safety_Status"
      ]);
      sheet.getRange(1, 1, 1, 19).setFontWeight("bold").setBackground("#1F4E79").setFontColor("#FFFFFF");
    }

    var data = JSON.parse(e.postData.contents);

    sheet.appendRow([
      data.timestamp || new Date().toLocaleString(),
      data.action || "",
      data.truck_id || "CAR",
      data.rfid || "",
      data.driver || "",
      data.type || "",
      data.f1_cars !== undefined ? data.f1_cars : "",
      data.f2_cars !== undefined ? data.f2_cars : "",
      data.f3_cars !== undefined ? data.f3_cars : "",
      data.total_inside !== undefined ? data.total_inside : "",
      data.f2_temp !== undefined ? data.f2_temp : "",
      data.f2_hum !== undefined ? data.f2_hum : "",
      data.f2_smoke !== undefined ? data.f2_smoke : "",
      data.f2_flame ? "FIRE" : "SAFE",
      data.f3_temp !== undefined ? data.f3_temp : "",
      data.f3_hum !== undefined ? data.f3_hum : "",
      data.f3_smoke !== undefined ? data.f3_smoke : "",
      data.f3_flame ? "FIRE" : "SAFE",
      data.safety_status || "SAFE"
    ]);

    return ContentService
      .createTextOutput(JSON.stringify({ status: "OK" }))
      .setMimeType(ContentService.MimeType.JSON);
  } catch (err) {
    return ContentService
      .createTextOutput(JSON.stringify({ status: "ERROR", message: err.toString() }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}
