/*
 * ======================================================================================
 *  BOARD 1      : ESP32 MASTER (Gates, RFID, 20x4 LCD, MQTT Node-RED & Local Excel Server)
 *  PROJECT NAME : Smart 3-Floor Industrial Garage & ASRS Control System (IoT)
 *  ARCHITECTURE : Dual-Core Offline-First + UART Master-Slave + HTTP Excel Endpoint
 * ======================================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ======================================================================================
// 1. Network, MQTT Broker & Local WebServer Configuration
// ======================================================================================
const char* ssid        = "FLOOR 2"; // Wi-Fi Network SSID 
const char* password    = "05003964";      // Wi-Fi Network Password
const char* mqtt_server = "192.168.1.53";  // Laptop IPv4 Address (Node-RED MQTT Broker)
const int   mqtt_port   = 1883;

WiFiClient espClient;
PubSubClient client(espClient);
WebServer excelServer(80); // Local HTTP Server on Port 80 for Excel Web Query

// Network state flags (Core 0 <-> Core 1)
volatile bool wifiConnected           = false;
volatile bool mqttConnected           = false;
volatile bool nodeRedVerified         = false;
volatile bool pendingWifiLcdPopup     = false;
volatile bool pendingSlaveCmd         = false;
bool          wifiPreviouslyConnected = false;
bool          mqttPreviouslyConnected = false;

// FreeRTOS synchronization primitives for Core 0 <-> Core 1 safety
SemaphoreHandle_t stateMutex     = NULL;
QueueHandle_t     gateEventQueue = NULL;
String            wifiIpForLcd   = "";

void enqueueGateEvent(const String& json) {
  if (gateEventQueue == NULL) return;
  char buf[192];
  json.toCharArray(buf, sizeof(buf));
  xQueueSend(gateEventQueue, buf, 0);
}

// ======================================================================================
// 2. UART2 Configuration (Link to Slave ESP32: RX2 = GPIO 16 | TX2 = GPIO 17)
// ======================================================================================
#define UART_RX_PIN 16
#define UART_TX_PIN 17
String uartRxBuffer = "";
unsigned long lastSlavePacketTime = 0;
bool slaveConnected = false;

// ======================================================================================
// 3. 20x4 I2C LCD Configuration (SDA -> GPIO 21 | SCL -> GPIO 22)
// ======================================================================================
LiquidCrystal_I2C lcd(0x27, 20, 4);

bool lcdEventActive = false;
unsigned long lcdEventTimer = 0;
const unsigned long LCD_EVENT_DURATION = 3000;

// ======================================================================================
// 4. Master Hardware Pinout Configuration
// ======================================================================================
// --- A) RFID Reader (RC522 via Hardware VSPI: SCK=18, MISO=19, MOSI=23) ---
#define SS_PIN           5
#define RST_PIN          -1
MFRC522 rfid(SS_PIN, RST_PIN);

// --- B) Gate Servo Motors ---
// NOTE: GPIO 12 is an ESP32 strapping pin (MTDI). Ensure the exit servo signal does not pull it HIGH at boot
// (if boot fails, move SERVO_EXIT_PIN to GPIO 25 or GPIO 26).
#define SERVO_ENTRY_PIN  13
#define SERVO_EXIT_PIN   12
Servo entryServo;
Servo exitServo;

// --- C) Gate Security & Access IR Sensors (Active LOW) ---
#define IR_INTRUSION_PIN 14
#define IR_EXIT_GATE_PIN 27

// --- D) Master Alarm & System Status LED Outputs ---
// NOTE: GPIO 2 must remain LOW during firmware flashing.
#define MASTER_ALARM_PIN      2
#define SYSTEM_STATUS_LED_PIN 25  // Stays ON continuously while the system is running

// ======================================================================================
// 5. System State Variables (Synchronized with Slave ESP32 via UART)
// ======================================================================================
bool entryGateOpen = false;
bool exitGateOpen  = false;
unsigned long entryGateTimer    = 0;
unsigned long exitGateTimer     = 0;
unsigned long lastLcdUpdateTime = 0;

// Garage Capacity Constants
#define TRUCKS_MAX       6
#define GROUND_FLOOR_MAX 10
#define FIRST_FLOOR_MAX  10
#define GARAGE_TOTAL_MAX (TRUCKS_MAX + GROUND_FLOOR_MAX + FIRST_FLOOR_MAX) // 26

// Registered Trucks Fleet (Edit UIDs to match your physical RFID cards)
struct TruckRecord {
  const char* truckId;
  const char* uid;
  const char* driver;
  bool isInside;
};

TruckRecord truckFleet[TRUCKS_MAX] = {
  { "TRK-01", "A1B2C3D4", "Ahmed Ali",      false },
  { "TRK-02", "B2C3D4E5", "Mahmoud Hassan", false },
  { "TRK-03", "C3D4E5F6", "Omar Khaled",    false },
  { "TRK-04", "D4E5F6A1", "Youssef Tarek",  false },
  { "TRK-05", "E5F6A1B2", "Karim Adel",     false },
  { "TRK-06", "F6A1B2C3", "Mostafa Samir",  false }
};

int pendingTruckIndex = -1; // Set when Truck RFID opens Entry Gate; confirmed on Main Gate IR pass

// Real-time data (Trucks Yard on Master | Ground & First Floor from Slave via UART)
volatile int   f1_cars = 0; // Trucks Yard (Max 6 - Master RFID + Main Gate IR)
volatile int   f2_cars = 0; // Ground Floor (Max 10 - Slave 2-IR)
volatile int   f3_cars = 0; // First Floor (Max 10 - Slave 2-IR)
volatile float t2 = 26.5, t3 = 28.0;
volatile int   h2 = 50,   h3 = 50;
volatile int   s2 = 0,    s3 = 0;
volatile bool  f2_flame = false, f3_flame = false;
volatile bool  f2_danger = false, f3_danger = false;

// Access statistics tracked by Master
volatile unsigned long totalEntered = 0;
volatile unsigned long totalExited  = 0;

// Last vehicle info for Excel logging (protected by stateMutex)
String lastDriverName  = "None";
String lastVehicleType = "None";
String lastCardUID     = "None";
String lastTruckId     = "None";

int findTruckByUID(const String& uid) {
  for (int i = 0; i < TRUCKS_MAX; i++) {
    if (uid.equalsIgnoreCase(truckFleet[i].uid)) return i;
  }
  return -1;
}

String getActiveTrucksList() {
  String list = "";
  for (int i = 0; i < TRUCKS_MAX; i++) {
    if (truckFleet[i].isInside) {
      if (list.length() > 0) list += ", ";
      list += truckFleet[i].truckId;
    }
  }
  return list.length() > 0 ? list : "None";
}

// Edge detection & Debounce for Gate IRs
bool last_intrusion = HIGH;
bool last_exit_ir   = HIGH;
unsigned long lastDebounceIntrusion = 0;
unsigned long lastDebounceExit      = 0;
const unsigned long DEBOUNCE_DELAY  = 250;

// Manual override & security flags
volatile bool manual_master_alarm = false;
volatile bool manual_f2_emerg     = false;
volatile bool manual_f3_emerg     = false;
volatile bool intrusion_active    = false;

// ======================================================================================
// 6. Helper Functions for LCD & String Parsing
// ======================================================================================
String pad20(String str) {
  while (str.length() < 20) str += " ";
  return str.substring(0, 20);
}

void showLcdPopup(String line0, String line1, String line2, String line3) {
  lcdEventActive = true;
  lcdEventTimer  = millis();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(line0.substring(0, 20));
  lcd.setCursor(0, 1); lcd.print(line1.substring(0, 20));
  lcd.setCursor(0, 2); lcd.print(line2.substring(0, 20));
  lcd.setCursor(0, 3); lcd.print(line3.substring(0, 20));
}

// Helper to extract values from key=value pairs received over UART
String getValue(String data, String key) {
  int keyIndex = data.indexOf(key + "=");
  if (keyIndex == -1) return "";
  int startIndex = keyIndex + key.length() + 1;
  int endIndex = data.indexOf(",", startIndex);
  if (endIndex == -1) endIndex = data.length();
  return data.substring(startIndex, endIndex);
}

// ======================================================================================
// 7. Local HTTP WebServer Handlers for Microsoft Excel (Data -> From Web)
// ======================================================================================
void getLastDriverSnapshot(String& driverOut, String& uidOut, String& truckIdOut) {
  if (stateMutex && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    driverOut  = lastDriverName;
    uidOut     = lastCardUID;
    truckIdOut = lastTruckId;
    xSemaphoreGive(stateMutex);
  } else {
    driverOut  = "None";
    uidOut     = "None";
    truckIdOut = "None";
  }
}

void setupExcelWebServer() {
  // 1) Main HTML Table Endpoint (Excel Power Query loves HTML Tables!)
  // URL: http://<ESP_IP>/
  excelServer.on("/", HTTP_GET, []() {
    String gateStr  = (entryGateOpen || exitGateOpen) ? "OPEN" : "CLOSED";
    String emergStr = (f2_danger || f3_danger || intrusion_active || manual_master_alarm) ? "EMERGENCY" : "SAFE";
    int totalCars   = f1_cars + f2_cars + f3_cars;
    String drv, uid, trkId;
    getLastDriverSnapshot(drv, uid, trkId);

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Smart Garage Live Data</title></head><body>";
    html += "<h2>Smart Industrial Garage - Live Local Feed</h2>";
    html += "<table border='1' id='GarageTable'>";
    html += "<tr><th>Facility</th><th>Total_Vehicles</th><th>Free_Slots</th><th>Total_Entered</th><th>Total_Exited</th><th>Trucks_Yard</th><th>Yard_Max</th><th>Active_Trucks</th><th>Ground_Floor_Cars</th><th>GF_Max</th><th>GF_Temp_C</th><th>GF_Hum_Pct</th><th>GF_Smoke_Pct</th><th>GF_Flame</th><th>First_Floor_Cars</th><th>FF_Max</th><th>FF_Temp_C</th><th>FF_Hum_Pct</th><th>FF_Smoke_Pct</th><th>FF_Flame</th><th>Gate_State</th><th>Safety_Status</th><th>Last_Truck_ID</th><th>Last_Driver</th><th>Last_UID</th></tr>";
    html += "<tr>";
    html += "<td>Smart_Garage</td>";
    html += "<td>" + String(totalCars) + "</td>";
    html += "<td>" + String(GARAGE_TOTAL_MAX - totalCars) + "</td>";
    html += "<td>" + String(totalEntered) + "</td>";
    html += "<td>" + String(totalExited) + "</td>";
    html += "<td>" + String(f1_cars) + "</td><td>" + String(TRUCKS_MAX) + "</td>";
    html += "<td>" + getActiveTrucksList() + "</td>";
    html += "<td>" + String(f2_cars) + "</td><td>" + String(GROUND_FLOOR_MAX) + "</td>";
    html += "<td>" + String(t2, 1) + "</td><td>" + String(h2) + "</td><td>" + String(s2) + "</td><td>" + String(f2_flame ? "FIRE" : "SAFE") + "</td>";
    html += "<td>" + String(f3_cars) + "</td><td>" + String(FIRST_FLOOR_MAX) + "</td>";
    html += "<td>" + String(t3, 1) + "</td><td>" + String(h3) + "</td><td>" + String(s3) + "</td><td>" + String(f3_flame ? "FIRE" : "SAFE") + "</td>";
    html += "<td>" + gateStr + "</td>";
    html += "<td>" + emergStr + "</td>";
    html += "<td>" + trkId + "</td>";
    html += "<td>" + drv + "</td>";
    html += "<td>" + uid + "</td>";
    html += "</tr></table></body></html>";

    excelServer.sendHeader("Access-Control-Allow-Origin", "*");
    excelServer.send(200, "text/html", html);
  });

  // 2) Direct CSV Endpoint for Excel
  // URL: http://<ESP_IP>/excel.csv
  excelServer.on("/excel.csv", HTTP_GET, []() {
    String gateStr  = (entryGateOpen || exitGateOpen) ? "OPEN" : "CLOSED";
    String emergStr = (f2_danger || f3_danger || intrusion_active || manual_master_alarm) ? "EMERGENCY" : "SAFE";
    int totalCars   = f1_cars + f2_cars + f3_cars;
    String drv, uid, trkId;
    getLastDriverSnapshot(drv, uid, trkId);

    String csv = "Facility,Total_Vehicles,Free_Slots,Total_Entered,Total_Exited,Trucks_Yard,Active_Trucks,Ground_Floor_Cars,GF_Temp_C,GF_Hum_Pct,GF_Smoke_Pct,GF_Flame,First_Floor_Cars,FF_Temp_C,FF_Hum_Pct,FF_Smoke_Pct,FF_Flame,Gate_State,Safety_Status,Last_Truck_ID,Last_Driver,Last_UID\n";
    csv += "Smart_Garage," + String(totalCars) + "," + String(GARAGE_TOTAL_MAX - totalCars) + "," + String(totalEntered) + "," + String(totalExited) + "," + String(f1_cars) + ",\"" + getActiveTrucksList() + "\"," + String(f2_cars) + "," + String(t2, 1) + "," + String(h2) + "," + String(s2) + "," + String(f2_flame ? "FIRE" : "SAFE") + ",";
    csv += String(f3_cars) + "," + String(t3, 1) + "," + String(h3) + "," + String(s3) + "," + String(f3_flame ? "FIRE" : "SAFE") + "," + gateStr + "," + emergStr + "," + trkId + "," + drv + "," + uid + "\n";

    excelServer.sendHeader("Access-Control-Allow-Origin", "*");
    excelServer.send(200, "text/csv", csv);
  });

  // 3) Direct JSON Endpoint (For Python / Node-RED / Web APIs)
  // URL: http://<ESP_IP>/data.json
  excelServer.on("/data.json", HTTP_GET, []() {
    String gateStr  = (entryGateOpen || exitGateOpen) ? "OPEN" : "CLOSED";
    String emergStr = (f2_danger || f3_danger || intrusion_active || manual_master_alarm) ? "EMERGENCY" : "SAFE";
    int totalCars   = f1_cars + f2_cars + f3_cars;
    String drv, uid, trkId;
    getLastDriverSnapshot(drv, uid, trkId);

    String json = "{";
    json += "\"facility\":\"Smart_Garage\",";
    json += "\"total_inside\":" + String(totalCars) + ",\"total_free\":" + String(GARAGE_TOTAL_MAX - totalCars) + ",\"total_max\":" + String(GARAGE_TOTAL_MAX) + ",\"total_entered\":" + String(totalEntered) + ",\"total_exited\":" + String(totalExited) + ",";
    json += "\"f1_cars\":" + String(f1_cars) + ",\"f1_free\":" + String(TRUCKS_MAX - f1_cars) + ",\"f1_max\":" + String(TRUCKS_MAX) + ",\"active_trucks\":\"" + getActiveTrucksList() + "\",";
    json += "\"f2_cars\":" + String(f2_cars) + ",\"f2_free\":" + String(GROUND_FLOOR_MAX - f2_cars) + ",\"f2_temp\":" + String(t2, 1) + ",\"f2_hum\":" + String(h2) + ",\"f2_smoke\":" + String(s2) + ",\"f2_flame\":" + String(f2_flame ? 1 : 0) + ",";
    json += "\"f3_cars\":" + String(f3_cars) + ",\"f3_free\":" + String(FIRST_FLOOR_MAX - f3_cars) + ",\"f3_temp\":" + String(t3, 1) + ",\"f3_hum\":" + String(h3) + ",\"f3_smoke\":" + String(s3) + ",\"f3_flame\":" + String(f3_flame ? 1 : 0) + ",";
    json += "\"gate\":\"" + gateStr + "\",\"status\":\"" + emergStr + "\",";
    json += "\"slave_connected\":" + String(slaveConnected ? "true" : "false") + ",";
    json += "\"last_truck_id\":\"" + trkId + "\",\"last_driver\":\"" + drv + "\",\"last_uid\":\"" + uid + "\"";
    json += "}";

    excelServer.sendHeader("Access-Control-Allow-Origin", "*");
    excelServer.send(200, "application/json", json);
  });

  excelServer.begin();
}

// ======================================================================================
// 8. UART Communication Handler (Receive Telemetry from Slave & Send Commands)
// ======================================================================================
// Send manual override commands to Slave ESP32 via UART2 (always called on Core 1)
void sendCommandToSlave() {
  String cmd = "CMD:F2=" + String(manual_f2_emerg ? 1 : 0) + ",F3=" + String(manual_f3_emerg ? 1 : 0) + "\n";
  Serial2.print(cmd);
}

void handleUART() {
  // Flush any pending command from MQTT callback (Core 0 -> Core 1)
  if (pendingSlaveCmd) {
    pendingSlaveCmd = false;
    sendCommandToSlave();
  }

  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      uartRxBuffer.trim();
      // Expected format from Slave: F2=0,F3=0,T2=26.5,H2=50,S2=0,FL2=0,D2=0,T3=28.0,H3=50,S3=0,FL3=0,D3=0
      // (Trucks Yard f1_cars is owned by Master via RFID + Main Gate IR)
      if (uartRxBuffer.startsWith("F2=") || uartRxBuffer.startsWith("F1=")) {
        f2_cars   = constrain(getValue(uartRxBuffer, "F2").toInt(), 0, GROUND_FLOOR_MAX);
        f3_cars   = constrain(getValue(uartRxBuffer, "F3").toInt(), 0, FIRST_FLOOR_MAX);
        t2        = getValue(uartRxBuffer, "T2").toFloat();
        String h2s = getValue(uartRxBuffer, "H2");
        if (h2s.length() > 0) h2 = constrain(h2s.toInt(), 0, 100);
        s2        = constrain(getValue(uartRxBuffer, "S2").toInt(), 0, 100);
        f2_flame  = (getValue(uartRxBuffer, "FL2").toInt() == 1);
        f2_danger = (getValue(uartRxBuffer, "D2").toInt() == 1) || f2_flame;
        t3        = getValue(uartRxBuffer, "T3").toFloat();
        String h3s = getValue(uartRxBuffer, "H3");
        if (h3s.length() > 0) h3 = constrain(h3s.toInt(), 0, 100);
        s3        = constrain(getValue(uartRxBuffer, "S3").toInt(), 0, 100);
        f3_flame  = (getValue(uartRxBuffer, "FL3").toInt() == 1);
        f3_danger = (getValue(uartRxBuffer, "D3").toInt() == 1) || f3_flame;

        lastSlavePacketTime = millis();
        if (!slaveConnected) {
          slaveConnected = true;
          Serial.println("[UART] >>> Slave ESP32 Connected & Syncing Floors Data! <<<");
          // Re-sync manual fan overrides in case Slave rebooted
          sendCommandToSlave();
        }
      }
      uartRxBuffer = "";
    } else if (uartRxBuffer.length() < 200) {
      uartRxBuffer += c;
    } else {
      // Guard against malformed / noisy UART stream without newline
      uartRxBuffer = "";
    }
  }

  // Detect if Slave ESP32 is disconnected (> 3 seconds without packet)
  if (slaveConnected && (millis() - lastSlavePacketTime > 3000)) {
    slaveConnected = false;
    Serial.println("[UART WARNING] Slave ESP32 connection lost! Check TX2/RX2 wiring.");
  }
}

// ======================================================================================
// 9. Real-Time 20x4 LCD Dashboard Updater (Flicker-Free)
// ======================================================================================
void updateLCD() {
  unsigned long now = millis();

  // Safely show Wi-Fi connected popup triggered from Core 0
  if (pendingWifiLcdPopup) {
    pendingWifiLcdPopup = false;
    String ipCopy = "";
    if (stateMutex && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      ipCopy = wifiIpForLcd;
      xSemaphoreGive(stateMutex);
    }
    showLcdPopup("  WIFI CONNECTED!   ", "Excel IP Address:   ", ipCopy, "http://" + ipCopy);
    return;
  }

  if (lcdEventActive) {
    if (now - lcdEventTimer >= LCD_EVENT_DURATION) {
      lcdEventActive = false;
      lcd.clear();
    } else {
      return;
    }
  }

  if (now - lastLcdUpdateTime < 500) return;
  lastLcdUpdateTime = now;

  // PRIORITY 1: Emergency Hazard or Security Breach Alert Screen
  if (f2_danger || f3_danger || intrusion_active || manual_master_alarm) {
    lcd.setCursor(0, 0); lcd.print(pad20("!! EMERGENCY ALERT !"));
    
    if (intrusion_active) {
      lcd.setCursor(0, 1); lcd.print(pad20("SECURITY BREACH!    "));
      lcd.setCursor(0, 2); lcd.print(pad20("Unauthorized Entry  "));
      lcd.setCursor(0, 3); lcd.print(pad20("Main Gate Locked!   "));
    } else {
      String hazardFloor = "";
      if (f2_danger) hazardFloor += "GF ";
      if (f3_danger) hazardFloor += "1F ";
      if (hazardFloor == "") hazardFloor = "MANUAL";

      lcd.setCursor(0, 1); lcd.print(pad20("HAZARD ZONE: " + hazardFloor));
      if (f2_flame || f3_flame) {
        lcd.setCursor(0, 2); lcd.print(pad20("!! FLAME DETECTED !!"));
      } else {
        lcd.setCursor(0, 2); lcd.print(pad20("T:" + String((int)max(t2, t3)) + "C GAS:" + String(max(s2, s3)) + "%"));
      }
      lcd.setCursor(0, 3); lcd.print(pad20("FANS & ALARM: ACTIVE"));
    }
    return;
  }

  // PRIORITY 2: Normal Real-Time Telemetry & Garage Occupancy Screen
  String netStatus = mqttConnected ? "ONLINE" : (wifiConnected ? "LOCAL" : "OFFLINE");
  String gateState = (entryGateOpen || exitGateOpen) ? "OPEN" : "CLSD";
  lcd.setCursor(0, 0);
  lcd.print(pad20("SYS:" + netStatus + " GT:" + gateState));

  lcd.setCursor(0, 1);
  lcd.print(pad20("Yard(Trk):" + String(f1_cars) + "/6 [OPEN]"));

  lcd.setCursor(0, 2);
  lcd.print(pad20("G-Flr:" + String(f2_cars) + "/10  T:" + String((int)t2) + "C"));

  lcd.setCursor(0, 3);
  lcd.print(pad20("1-Flr:" + String(f3_cars) + "/10  T:" + String((int)t3) + "C"));
}

// ======================================================================================
// 10. MQTT Callback: Receives Commands from Node-RED & Forwards to Slave via UART
// ======================================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  String topicStr = String(topic);

  if (!nodeRedVerified) {
    nodeRedVerified = true;
    Serial.println("[NODE-RED] >>> VERIFIED! Active 2-Way Link Established <<<");
  }

  if (topicStr == "garage/control/nodered_ack") return;

  if (topicStr == "garage/control/master_alarm") {
    manual_master_alarm = (msg == "1" || msg == "true" || msg == "ON");
    if (!manual_master_alarm) intrusion_active = false;
    Serial.printf("[NODE-RED CMD] Master Alarm set to: %s\n", manual_master_alarm ? "ON" : "OFF");
  } 
  else if (topicStr == "garage/control/f2_emergency") {
    manual_f2_emerg = (msg == "1" || msg == "true" || msg == "ON");
    pendingSlaveCmd = true;
    Serial.printf("[NODE-RED CMD -> UART SLAVE] Ground Floor Fan set to: %s\n", manual_f2_emerg ? "ON" : "OFF");
  } 
  else if (topicStr == "garage/control/f3_emergency") {
    manual_f3_emerg = (msg == "1" || msg == "true" || msg == "ON");
    pendingSlaveCmd = true;
    Serial.printf("[NODE-RED CMD -> UART SLAVE] First Floor Fan set to: %s\n", manual_f3_emerg ? "ON" : "OFF");
  }
}

// ======================================================================================
// 11. Vehicle Entry & RFID Access Control
// ======================================================================================
void confirmTruckEntry(int idx) {
  if (idx < 0 || idx >= TRUCKS_MAX) return;
  if (!truckFleet[idx].isInside && f1_cars < TRUCKS_MAX) {
    truckFleet[idx].isInside = true;
    f1_cars++;
    totalEntered++;

    if (stateMutex && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      lastTruckId     = truckFleet[idx].truckId;
      lastCardUID     = truckFleet[idx].uid;
      lastDriverName  = truckFleet[idx].driver;
      lastVehicleType = "Truck (Trucks Yard)";
      xSemaphoreGive(stateMutex);
    }

    Serial.printf("[TRUCK CONFIRMED IN] %s (%s) -> Yard: %d/%d\n", truckFleet[idx].truckId, truckFleet[idx].driver, f1_cars, TRUCKS_MAX);
    showLcdPopup("  TRUCK CHECKED IN  ", String(truckFleet[idx].truckId) + " " + String(truckFleet[idx].driver), "Trucks Yard: " + String(f1_cars) + "/6", "Logged to Sheets!   ");

    String evt = "{\"action\":\"TRUCK_ENTER\",\"truck_id\":\"" + String(truckFleet[idx].truckId) + "\",\"rfid\":\"" + String(truckFleet[idx].uid) + "\",\"driver\":\"" + String(truckFleet[idx].driver) + "\",\"type\":\"Truck (Trucks Yard)\",\"f1_cars\":" + String(f1_cars) + ",\"total_entered\":" + String(totalEntered) + ",\"total_exited\":" + String(totalExited) + "}";
    enqueueGateEvent(evt);
  }
}

void processTruckExit(int idx) {
  if (idx < 0 || idx >= TRUCKS_MAX) return;
  if (truckFleet[idx].isInside) {
    truckFleet[idx].isInside = false;
    if (f1_cars > 0) f1_cars--;
    totalExited++;

    exitServo.write(90);
    exitGateOpen  = true;
    exitGateTimer = millis();

    if (stateMutex && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      lastTruckId     = truckFleet[idx].truckId;
      lastCardUID     = truckFleet[idx].uid;
      lastDriverName  = truckFleet[idx].driver;
      lastVehicleType = "Truck Exit";
      xSemaphoreGive(stateMutex);
    }

    Serial.printf("[TRUCK CHECKED OUT] %s (%s) -> Yard: %d/%d\n", truckFleet[idx].truckId, truckFleet[idx].driver, f1_cars, TRUCKS_MAX);
    showLcdPopup("  TRUCK EXITING!    ", String(truckFleet[idx].truckId) + " " + String(truckFleet[idx].driver), "Trucks Yard: " + String(f1_cars) + "/6", "Exit Gate Open...   ");

    String evt = "{\"action\":\"TRUCK_EXIT\",\"truck_id\":\"" + String(truckFleet[idx].truckId) + "\",\"rfid\":\"" + String(truckFleet[idx].uid) + "\",\"driver\":\"" + String(truckFleet[idx].driver) + "\",\"type\":\"Truck (Trucks Yard)\",\"f1_cars\":" + String(f1_cars) + ",\"total_entered\":" + String(totalEntered) + ",\"total_exited\":" + String(totalExited) + "}";
    enqueueGateEvent(evt);
  }
}

void processVehicleEntry(String tagID, String driverName, String vehicleType, int truckIdx = -1, bool immediateTruckConfirm = false) {
  bool isTruck = (truckIdx != -1) || (vehicleType.indexOf("Truck") != -1);

  if (f1_cars >= TRUCKS_MAX && f2_cars >= GROUND_FLOOR_MAX && f3_cars >= FIRST_FLOOR_MAX) {
    Serial.println("[GATE] Garage is COMPLETELY FULL! Entry denied.");
    showLcdPopup("   ACCESS DENIED!   ", "  GARAGE IS FULL!   ", "Yd:6/6 GF:10 1F:10  ", "Please wait outside ");
    return;
  }

  // Handle registered Truck (RFID + Main Gate IR)
  if (truckIdx != -1) {
    // If this truck is ALREADY inside, tapping its RFID checks it OUT!
    if (truckFleet[truckIdx].isInside) {
      processTruckExit(truckIdx);
      return;
    }
    if (f1_cars >= TRUCKS_MAX) {
      Serial.println("[GATE] Trucks Yard is FULL (6/6)! Entry denied.");
      showLcdPopup("   ACCESS DENIED!   ", " TRUCKS YARD FULL!  ", "Trucks Yard: 6/6    ", "Please wait outside ");
      return;
    }

    entryServo.write(90);
    entryGateOpen    = true;
    entryGateTimer   = millis();
    intrusion_active = false;

    if (immediateTruckConfirm) {
      pendingTruckIndex = -1;
      confirmTruckEntry(truckIdx);
    } else {
      // Wait for Main Gate IR (IR_INTRUSION_PIN) to confirm truck physically passed through!
      pendingTruckIndex = truckIdx;
      Serial.printf("[TRUCK RFID OK] %s (%s) - Waiting for Main Gate IR pass...\n", truckFleet[truckIdx].truckId, truckFleet[truckIdx].driver);
      showLcdPopup("  TRUCK AUTHORIZED  ", String(truckFleet[truckIdx].truckId) + " " + String(truckFleet[truckIdx].driver), "Gate Opening...     ", "Pass Main Gate IR   ");
    }
    return;
  }

  // Handle Passenger Cars (Ground Floor / First Floor)
  if (!isTruck && f2_cars >= GROUND_FLOOR_MAX && f3_cars >= FIRST_FLOOR_MAX) {
    Serial.println("[GATE] Ground & First Floors (Cars) are FULL! Entry denied.");
    showLcdPopup("   ACCESS DENIED!   ", "  CAR FLOORS FULL!  ", "GF:10/10  1F:10/10  ", "Please wait outside ");
    return;
  }

  entryServo.write(90);
  entryGateOpen    = true;
  entryGateTimer   = millis();
  intrusion_active = false;
  pendingTruckIndex = -1;
  totalEntered++;

  if (stateMutex && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    lastTruckId     = "CAR";
    lastCardUID     = tagID;
    lastDriverName  = driverName;
    lastVehicleType = vehicleType;
    xSemaphoreGive(stateMutex);
  }

  Serial.println("[ENTRY GATE OPEN] Welcome: " + driverName + " (" + tagID + ")");
  showLcdPopup("  ACCESS GRANTED!   ", driverName, vehicleType, "Gate Opening...     ");

  String evt = "{\"action\":\"ENTER\",\"truck_id\":\"CAR\",\"rfid\":\"" + tagID + "\",\"driver\":\"" + driverName + "\",\"type\":\"" + vehicleType + "\",\"f1_cars\":" + String(f1_cars) + ",\"total_entered\":" + String(totalEntered) + ",\"total_exited\":" + String(totalExited) + "}";
  enqueueGateEvent(evt);
}

void triggerExitGate() {
  if (!exitGateOpen) {
    exitServo.write(90);
    exitGateOpen  = true;
    exitGateTimer = millis();
    totalExited++;
    Serial.println("[GATE] Vehicle departing. Exit barrier opened.");
    showLcdPopup("  VEHICLE EXITING   ", "  Goodbye & Drive   ", "      Safely!       ", "Exit Gate Open...   ");

    String evt = "{\"action\":\"EXIT\",\"truck_id\":\"CAR\",\"rfid\":\"AUTO_EXIT\",\"driver\":\"Exiting Vehicle\",\"type\":\"Exit Gate\",\"f1_cars\":" + String(f1_cars) + ",\"total_entered\":" + String(totalEntered) + ",\"total_exited\":" + String(totalExited) + "}";
    enqueueGateEvent(evt);
  }
}

void handleRFID() {
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    if (cmd == '1') processVehicleEntry("55573406", "Eng. Seif Hussein", "Sedan (Ground Flr)", -1, false);
    if (cmd == '2') {
      // Test command: Enter next available Truck (TRK-01..TRK-06)
      for (int i = 0; i < TRUCKS_MAX; i++) {
        if (!truckFleet[i].isInside) {
          processVehicleEntry(truckFleet[i].uid, truckFleet[i].driver, "Truck (Trucks Yard)", i, true);
          break;
        }
      }
    }
    if (cmd == '3') triggerExitGate();
    if (cmd == '4') {
      // Test command: Exit most recent Truck inside
      for (int i = TRUCKS_MAX - 1; i >= 0; i--) {
        if (truckFleet[i].isInside) {
          processTruckExit(i);
          break;
        }
      }
    }
  }

  // Poll RC522 every 100ms so FM17522 (0x82) RF field stays stable and powers the card
  static unsigned long lastRfidPoll = 0;
  if (millis() - lastRfidPoll < 100) return;
  lastRfidPoll = millis();

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String tagID = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) tagID += "0";
    tagID += String(rfid.uid.uidByte[i], HEX);
  }
  tagID.toUpperCase();
  Serial.println("[RFID SCANNED] Card UID = " + tagID);

  // Check if this card belongs to one of the 6 registered Trucks
  int truckIdx = findTruckByUID(tagID);
  if (truckIdx != -1) {
    processVehicleEntry(tagID, truckFleet[truckIdx].driver, "Truck (Trucks Yard)", truckIdx, false);
  } else {
    String driverName  = "Guest Driver";
    String vehicleType = "Passenger Car";
    if (tagID == "55573406" || tagID == "F9E8D7C6") {
      driverName  = "Eng. Seif Hussein";
      vehicleType = "Sedan (Ground Flr)";
    } else if (tagID == "0E647C05") {
      driverName  = "Eng. Eiad";
      vehicleType = "Sedan (First Flr)";
    }
    processVehicleEntry(tagID, driverName, vehicleType, -1, false);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ======================================================================================
// 12. Gate Barriers, Intrusion Detection & Master Alarm
// ======================================================================================
void handleGatesAndAlarm() {
  unsigned long now = millis();

  // Instant Master Alarm control (triggers if Slave reports danger OR local breach)
  bool any_danger = (f2_danger || f3_danger || manual_master_alarm || intrusion_active);
  digitalWrite(MASTER_ALARM_PIN, any_danger ? HIGH : LOW);

  // 1. Auto-close Entry Gate after 4000 ms
  if (entryGateOpen && (now - entryGateTimer >= 4000)) {
    entryServo.write(0);
    entryGateOpen = false;
    pendingTruckIndex = -1; // Clear if truck did not pass IR in time
    Serial.println("[GATE] Entry barrier auto-closed.");
  }

  // 2. Post-entry Main Gate IR check (Confirms Truck Entry OR Detects Intrusion)
  bool currentIntrusion = digitalRead(IR_INTRUSION_PIN);
  if (currentIntrusion == LOW && last_intrusion == HIGH && (now - lastDebounceIntrusion > DEBOUNCE_DELAY)) {
    lastDebounceIntrusion = now;
    if (entryGateOpen) {
      if (pendingTruckIndex != -1) {
        int idx = pendingTruckIndex;
        pendingTruckIndex = -1;
        confirmTruckEntry(idx);
      }
    } else {
      intrusion_active = true;
      Serial.println("[ALARM] SECURITY BREACH! Unauthorized entry detected at Main Gate!");
      enqueueGateEvent("{\"action\":\"INTRUSION\",\"truck_id\":\"NONE\",\"rfid\":\"INTRUSION_ALERT\",\"driver\":\"UNAUTHORIZED\",\"type\":\"SECURITY_BREACH\"}");
    }
  }
  last_intrusion = currentIntrusion;

  // 3. Pre-exit IR check to open Exit Gate
  bool currentExitIR = digitalRead(IR_EXIT_GATE_PIN);
  if (currentExitIR == LOW && last_exit_ir == HIGH && (now - lastDebounceExit > DEBOUNCE_DELAY)) {
    lastDebounceExit = now;
    triggerExitGate();
  }
  last_exit_ir = currentExitIR;

  // 4. Auto-close Exit Gate after 4000 ms
  if (exitGateOpen && (now - exitGateTimer >= 4000)) {
    exitServo.write(0);
    exitGateOpen = false;
    Serial.println("[GATE] Exit barrier auto-closed.");
  }
}

// ======================================================================================
// 13. Core 0 Task: Wi-Fi, Local Excel WebServer & MQTT Node-RED Sync
// ======================================================================================
void networkTask(void * parameter) {
  unsigned long lastMqttRetry     = 0;
  unsigned long lastTelemetrySend = 0;

  espClient.setTimeout(2);
  client.setSocketTimeout(2);

  for (;;) {
    unsigned long now = millis();
    wl_status_t wifiStatus = WiFi.status();

    if (wifiStatus == WL_CONNECTED) {
      wifiConnected = true;
      if (!wifiPreviouslyConnected) {
        wifiPreviouslyConnected = true;
        String ip = WiFi.localIP().toString();
        Serial.println("\n==================================================================");
        Serial.println("[WIFI] >>> Connected Successfully! <<<");
        Serial.println("[EXCEL LINKS FOR MAIN TEAM (Data -> From Web)]:");
        Serial.println("  1) HTML Table Link : http://" + ip + "/");
        Serial.println("  2) CSV Direct Link : http://" + ip + "/excel.csv");
        Serial.println("  3) JSON API Link   : http://" + ip + "/data.json");
        Serial.println("==================================================================\n");
        if (stateMutex && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          wifiIpForLcd = ip;
          xSemaphoreGive(stateMutex);
        }
        pendingWifiLcdPopup = true;
      }

      // Handle incoming Excel HTTP requests continuously without blocking
      excelServer.handleClient();

      // Handle MQTT Node-RED connection
      if (client.connected()) {
        mqttConnected = true;
        client.loop();

        char evtBuf[192];
        while (gateEventQueue != NULL && xQueueReceive(gateEventQueue, evtBuf, 0) == pdTRUE) {
          client.publish("garage/gate/event", evtBuf);
        }

        if (now - lastTelemetrySend >= 2000) {
          lastTelemetrySend = now;
          int totalInside  = f1_cars + f2_cars + f3_cars;
          int totalFree    = GARAGE_TOTAL_MAX - totalInside;
          int occupancyPct = (totalInside * 100) / GARAGE_TOTAL_MAX;
          bool f2FanOn     = (f2_danger || manual_f2_emerg);
          bool f3FanOn     = (f3_danger || manual_f3_emerg);
          bool anyDanger   = (f2_danger || f3_danger || manual_master_alarm || intrusion_active);
          String gateState = (entryGateOpen || exitGateOpen) ? "OPEN" : "CLOSED";

          String payload = "{";
          payload += "\"f1_cars\":" + String(f1_cars) + ",\"f1_free\":" + String(TRUCKS_MAX - f1_cars) + ",\"f1_max\":" + String(TRUCKS_MAX) + ",\"active_trucks\":\"" + getActiveTrucksList() + "\",";
          payload += "\"f2_temp\":" + String(t2, 1) + ",\"f2_hum\":" + String(h2) + ",\"f2_smoke\":" + String(s2) + ",\"f2_flame\":" + String(f2_flame ? 1 : 0) + ",\"f2_cars\":" + String(f2_cars) + ",\"f2_free\":" + String(GROUND_FLOOR_MAX - f2_cars) + ",\"f2_danger\":" + String(f2_danger ? 1 : 0) + ",\"f2_fan\":" + String(f2FanOn ? 1 : 0) + ",";
          payload += "\"f3_temp\":" + String(t3, 1) + ",\"f3_hum\":" + String(h3) + ",\"f3_smoke\":" + String(s3) + ",\"f3_flame\":" + String(f3_flame ? 1 : 0) + ",\"f3_cars\":" + String(f3_cars) + ",\"f3_free\":" + String(FIRST_FLOOR_MAX - f3_cars) + ",\"f3_danger\":" + String(f3_danger ? 1 : 0) + ",\"f3_fan\":" + String(f3FanOn ? 1 : 0) + ",";
          payload += "\"total_inside\":" + String(totalInside) + ",\"total_free\":" + String(totalFree) + ",\"total_max\":" + String(GARAGE_TOTAL_MAX) + ",\"occupancy_pct\":" + String(occupancyPct) + ",\"total_entered\":" + String(totalEntered) + ",\"total_exited\":" + String(totalExited) + ",";
          payload += "\"gate_status\":\"" + gateState + "\",\"intrusion\":" + String(intrusion_active ? 1 : 0) + ",\"master_alarm\":" + String(anyDanger ? 1 : 0) + ",";
          payload += "\"slave_connected\":" + String(slaveConnected ? 1 : 0) + ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi\":" + String(WiFi.RSSI());
          payload += "}";

          client.publish("garage/telemetry", payload.c_str());
        }
      } 
      else {
        mqttConnected = false;
        nodeRedVerified = false;

        if (now - lastMqttRetry > 5000) {
          lastMqttRetry = now;
          if (client.connect("ESP32_SmartGarage_Master")) {
            mqttConnected = true;
            Serial.println("[MQTT] Connected to Node-RED Broker!");
            client.subscribe("garage/control/#");
            String bootMsg = "{\"status\":\"ESP32_ONLINE\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
            client.publish("garage/system/status", bootMsg.c_str(), true);
          }
        }
      }
    } 
    else {
      wifiConnected = false;
      mqttConnected = false;
      if (wifiPreviouslyConnected) {
        wifiPreviouslyConnected = false;
        Serial.println("[WIFI] Disconnected! Running in Offline Mode.");
      }
      if (wifiStatus == WL_DISCONNECTED || wifiStatus == WL_CONNECTION_LOST || wifiStatus == WL_NO_SSID_AVAIL) {
        WiFi.begin(ssid, password);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
      }
    }

    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

// ======================================================================================
// 14. Master System Initialization (Setup)
// ======================================================================================
void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(" SMART GARAGE MASTER");
  lcd.setCursor(0, 1); lcd.print("  Eng. Seif Hussein ");
  lcd.setCursor(0, 2); lcd.print(" Dual-ESP32 UART Sys");
  lcd.setCursor(0, 3); lcd.print(" Booting Hardware...");
  delay(1200);

  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();
  delay(50);
  rfid.PCD_WriteRegister(rfid.TxControlReg, 0x83);
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);
  byte rfidVer = rfid.PCD_ReadRegister(rfid.VersionReg);
  byte txCtrl  = rfid.PCD_ReadRegister(rfid.TxControlReg);
  if (rfidVer == 0x00 || rfidVer == 0xFF) {
    Serial.printf("[RFID ERROR] RC522 NOT DETECTED (Version=0x%02X)! Check 3.3V, GND, RST->3.3V, and SPI pins (5,18,19,23).\n", rfidVer);
  } else {
    Serial.printf("[RFID OK] RC522 Connected! Firmware=0x%02X | AntennaReg=0x%02X (%s)\n", rfidVer, txCtrl, (txCtrl & 0x03) == 0x03 ? "ON" : "OFF");
  }

  entryServo.attach(SERVO_ENTRY_PIN);
  exitServo.attach(SERVO_EXIT_PIN);
  entryServo.write(0); 
  exitServo.write(0);

  pinMode(IR_INTRUSION_PIN, INPUT_PULLUP);
  pinMode(IR_EXIT_GATE_PIN, INPUT_PULLUP);
  pinMode(MASTER_ALARM_PIN, OUTPUT);
  digitalWrite(MASTER_ALARM_PIN, LOW);

  pinMode(SYSTEM_STATUS_LED_PIN, OUTPUT);
  digitalWrite(SYSTEM_STATUS_LED_PIN, HIGH); // Always ON while system is running

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  stateMutex     = xSemaphoreCreateMutex();
  gateEventQueue = xQueueCreate(8, 192);

  setupExcelWebServer();
  client.setBufferSize(768);
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  lcd.clear();

  xTaskCreatePinnedToCore(networkTask, "NetworkTask", 8192, NULL, 1, NULL, 0);
}

// ======================================================================================
// 15. Master Real-Time Loop (Core 1)
// ======================================================================================
void loop() {
  handleUART();          // 1. Receive live floor counts & temperatures from Slave ESP32
  handleRFID();          // 2. Poll RFID reader & Serial test commands ('1', '2')
  handleGatesAndAlarm(); // 3. Control Entry/Exit servos, intrusion sensor, and Master Alarm
  updateLCD();           // 4. Update 20x4 LCD screen
}