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
const char* ssid        = "Seif's iPhone"; // Wi-Fi Network SSID (أو شبكة التيم الأساسي)
const char* password    = "12345678";      // Wi-Fi Network Password
const char* mqtt_server = "172.20.10.3";   // Laptop IPv4 Address (Node-RED MQTT Broker)
const int   mqtt_port   = 1883;

WiFiClient espClient;
PubSubClient client(espClient);
WebServer excelServer(80); // Local HTTP Server on Port 80 for Excel Web Query

// Network state flags (Core 0)
volatile bool wifiConnected           = false;
volatile bool mqttConnected           = false;
volatile bool nodeRedVerified         = false;
bool          wifiPreviouslyConnected = false;
bool          mqttPreviouslyConnected = false;

// Thread-safe event buffer to pass gate events from Core 1 to Core 0
volatile bool pendingGateEvent     = false;
String        pendingGateEventJson = "";

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
#define SERVO_ENTRY_PIN  13
#define SERVO_EXIT_PIN   12
Servo entryServo;
Servo exitServo;

// --- C) Gate Security & Access IR Sensors (Active LOW) ---
#define IR_INTRUSION_PIN 14
#define IR_EXIT_GATE_PIN 27

// --- D) Master Alarm Output ---
#define MASTER_ALARM_PIN 2

// ======================================================================================
// 5. System State Variables (Synchronized with Slave ESP32 via UART)
// ======================================================================================
bool entryGateOpen = false;
bool exitGateOpen  = false;
unsigned long entryGateTimer    = 0;
unsigned long exitGateTimer     = 0;
unsigned long lastLcdUpdateTime = 0;

// Real-time data received from Slave ESP32
volatile int   f1_cars = 0;
volatile int   f2_cars = 0;
volatile int   f3_cars = 0;
volatile float t2 = 26.5, t3 = 28.0;
volatile int   s2 = 10,   s3 = 10;
volatile bool  f2_danger = false, f3_danger = false;

// Last vehicle info for Excel logging
String lastDriverName  = "None";
String lastVehicleType = "None";
String lastCardUID     = "None";

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
void setupExcelWebServer() {
  // 1) Main HTML Table Endpoint (Excel Power Query loves HTML Tables!)
  // URL: http://<ESP_IP>/
  excelServer.on("/", HTTP_GET, []() {
    String gateStr  = (entryGateOpen || exitGateOpen) ? "OPEN" : "CLOSED";
    String emergStr = (f2_danger || f3_danger || intrusion_active || manual_master_alarm) ? "EMERGENCY" : "SAFE";
    int totalCars   = f1_cars + f2_cars + f3_cars;

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Smart Garage Live Data</title></head><body>";
    html += "<h2>Smart 3-Floor Industrial Garage - Live Local Feed</h2>";
    html += "<table border='1' id='GarageTable'>";
    html += "<tr><th>Facility</th><th>Total_Vehicles</th><th>F1_Trucks</th><th>F1_Max</th><th>F2_Sedans</th><th>F2_Max</th><th>F2_Temp_C</th><th>F2_Smoke_Pct</th><th>F3_Sedans</th><th>F3_Max</th><th>F3_Temp_C</th><th>F3_Smoke_Pct</th><th>Gate_State</th><th>Safety_Status</th><th>Last_Driver</th><th>Last_UID</th></tr>";
    html += "<tr>";
    html += "<td>Smart_Garage</td>";
    html += "<td>" + String(totalCars) + "</td>";
    html += "<td>" + String(f1_cars) + "</td><td>5</td>";
    html += "<td>" + String(f2_cars) + "</td><td>10</td>";
    html += "<td>" + String(t2, 1) + "</td><td>" + String(s2) + "</td>";
    html += "<td>" + String(f3_cars) + "</td><td>10</td>";
    html += "<td>" + String(t3, 1) + "</td><td>" + String(s3) + "</td>";
    html += "<td>" + gateStr + "</td>";
    html += "<td>" + emergStr + "</td>";
    html += "<td>" + lastDriverName + "</td>";
    html += "<td>" + lastCardUID + "</td>";
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

    String csv = "Facility,Total_Vehicles,F1_Trucks,F2_Sedans,F2_Temp_C,F2_Smoke_Pct,F3_Sedans,F3_Temp_C,F3_Smoke_Pct,Gate_State,Safety_Status,Last_Driver,Last_UID\n";
    csv += "Smart_Garage," + String(totalCars) + "," + String(f1_cars) + "," + String(f2_cars) + "," + String(t2, 1) + "," + String(s2) + ",";
    csv += String(f3_cars) + "," + String(t3, 1) + "," + String(s3) + "," + gateStr + "," + emergStr + "," + lastDriverName + "," + lastCardUID + "\n";

    excelServer.sendHeader("Access-Control-Allow-Origin", "*");
    excelServer.send(200, "text/csv", csv);
  });

  // 3) Direct JSON Endpoint (For Python / Node-RED / Web APIs)
  // URL: http://<ESP_IP>/data.json
  excelServer.on("/data.json", HTTP_GET, []() {
    String gateStr  = (entryGateOpen || exitGateOpen) ? "OPEN" : "CLOSED";
    String emergStr = (f2_danger || f3_danger || intrusion_active || manual_master_alarm) ? "EMERGENCY" : "SAFE";

    String json = "{";
    json += "\"facility\":\"Smart_Garage\",";
    json += "\"f1_cars\":" + String(f1_cars) + ",";
    json += "\"f2_cars\":" + String(f2_cars) + ",\"f2_temp\":" + String(t2, 1) + ",\"f2_smoke\":" + String(s2) + ",";
    json += "\"f3_cars\":" + String(f3_cars) + ",\"f3_temp\":" + String(t3, 1) + ",\"f3_smoke\":" + String(s3) + ",";
    json += "\"gate\":\"" + gateStr + "\",\"status\":\"" + emergStr + "\",";
    json += "\"last_driver\":\"" + lastDriverName + "\",\"last_uid\":\"" + lastCardUID + "\"";
    json += "}";

    excelServer.sendHeader("Access-Control-Allow-Origin", "*");
    excelServer.send(200, "application/json", json);
  });

  excelServer.begin();
}

// ======================================================================================
// 8. UART Communication Handler (Receive Telemetry from Slave & Send Commands)
// ======================================================================================
void handleUART() {
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      uartRxBuffer.trim();
      // Expected format: F1=0,F2=0,F3=0,T2=26.5,S2=10,D2=0,T3=28.0,S3=10,D3=0
      if (uartRxBuffer.startsWith("F1=")) {
        f1_cars   = getValue(uartRxBuffer, "F1").toInt();
        f2_cars   = getValue(uartRxBuffer, "F2").toInt();
        f3_cars   = getValue(uartRxBuffer, "F3").toInt();
        t2        = getValue(uartRxBuffer, "T2").toFloat();
        s2        = getValue(uartRxBuffer, "S2").toInt();
        f2_danger = (getValue(uartRxBuffer, "D2").toInt() == 1);
        t3        = getValue(uartRxBuffer, "T3").toFloat();
        s3        = getValue(uartRxBuffer, "S3").toInt();
        f3_danger = (getValue(uartRxBuffer, "D3").toInt() == 1);

        lastSlavePacketTime = millis();
        if (!slaveConnected) {
          slaveConnected = true;
          Serial.println("[UART] >>> Slave ESP32 Connected & Syncing Floors Data! <<<");
        }
      }
      uartRxBuffer = "";
    } else {
      uartRxBuffer += c;
    }
  }

  // Detect if Slave ESP32 is disconnected (> 3 seconds without packet)
  if (slaveConnected && (millis() - lastSlavePacketTime > 3000)) {
    slaveConnected = false;
    Serial.println("[UART WARNING] Slave ESP32 connection lost! Check TX2/RX2 wiring.");
  }
}

// Send manual override commands to Slave ESP32 via UART2
void sendCommandToSlave() {
  String cmd = "CMD:F2=" + String(manual_f2_emerg ? 1 : 0) + ",F3=" + String(manual_f3_emerg ? 1 : 0) + "\n";
  Serial2.print(cmd);
}

// ======================================================================================
// 9. Real-Time 20x4 LCD Dashboard Updater (Flicker-Free)
// ======================================================================================
void updateLCD() {
  unsigned long now = millis();

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
      if (f2_danger) hazardFloor += "F2 ";
      if (f3_danger) hazardFloor += "F3 ";
      if (hazardFloor == "") hazardFloor = "MANUAL";

      lcd.setCursor(0, 1); lcd.print(pad20("HAZARD ZONE: " + hazardFloor));
      lcd.setCursor(0, 2); lcd.print(pad20("T:" + String((int)max(t2, t3)) + "C GAS:" + String(max(s2, s3)) + "%"));
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
  lcd.print(pad20("F1(Trk):" + String(f1_cars) + "/5  [OPEN]"));

  lcd.setCursor(0, 2);
  lcd.print(pad20("F2(Car):" + String(f2_cars) + "/10 T:" + String((int)t2) + "C"));

  lcd.setCursor(0, 3);
  lcd.print(pad20("F3(Car):" + String(f3_cars) + "/10 T:" + String((int)t3) + "C"));
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
    manual_master_alarm = (msg == "1");
    if (!manual_master_alarm) intrusion_active = false;
    Serial.printf("[NODE-RED CMD] Master Alarm set to: %s\n", manual_master_alarm ? "ON" : "OFF");
  } 
  else if (topicStr == "garage/control/f2_emergency") {
    manual_f2_emerg = (msg == "1");
    sendCommandToSlave();
    Serial.printf("[NODE-RED CMD -> UART SLAVE] Floor 2 Fan set to: %s\n", manual_f2_emerg ? "ON" : "OFF");
  } 
  else if (topicStr == "garage/control/f3_emergency") {
    manual_f3_emerg = (msg == "1");
    sendCommandToSlave();
    Serial.printf("[NODE-RED CMD -> UART SLAVE] Floor 3 Fan set to: %s\n", manual_f3_emerg ? "ON" : "OFF");
  }
}

// ======================================================================================
// 11. Vehicle Entry & RFID Access Control
// ======================================================================================
void processVehicleEntry(String tagID, String driverName, String vehicleType) {
  if (f1_cars >= 5 && f2_cars >= 10 && f3_cars >= 10) {
    Serial.println("[GATE] Garage is COMPLETELY FULL! Entry denied.");
    showLcdPopup("   ACCESS DENIED!   ", "  GARAGE IS FULL!   ", "F1:5/5 F2:10 F3:10  ", "Please wait outside ");
    return;
  }

  entryServo.write(90);
  entryGateOpen    = true;
  entryGateTimer   = millis();
  intrusion_active = false;

  lastCardUID     = tagID;
  lastDriverName  = driverName;
  lastVehicleType = vehicleType;

  Serial.println("[ENTRY GATE OPEN] Welcome: " + driverName + " (" + tagID + ")");
  showLcdPopup("  ACCESS GRANTED!   ", driverName, vehicleType, "Gate Opening...     ");

  pendingGateEventJson = "{\"action\":\"ENTER\",\"rfid\":\"" + tagID + "\",\"driver\":\"" + driverName + "\",\"type\":\"" + vehicleType + "\"}";
  pendingGateEvent = true;
}

void handleRFID() {
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    if (cmd == '1') processVehicleEntry("F9E8D7C6", "Eng. Seif Hussein", "Sedan (Floor 2)");
    if (cmd == '2') processVehicleEntry("A1B2C3D4", "Ahmed Ali", "Truck (Floor 1)");
  }

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String tagID = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) tagID += "0";
    tagID += String(rfid.uid.uidByte[i], HEX);
  }
  tagID.toUpperCase();

  String driverName  = "Guest Driver";
  String vehicleType = "Passenger Car";

  if (tagID == "F9E8D7C6") {
    driverName  = "Eng. Seif Hussein";
    vehicleType = "Sedan (Floor 2)";
  } 
  else if (tagID == "A1B2C3D4") {
    driverName  = "Ahmed Ali";
    vehicleType = "Truck (Floor 1)";
  }

  processVehicleEntry(tagID, driverName, vehicleType);
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
    Serial.println("[GATE] Entry barrier auto-closed.");
  }

  // 2. Post-entry Intrusion IR check
  bool currentIntrusion = digitalRead(IR_INTRUSION_PIN);
  if (currentIntrusion == LOW && last_intrusion == HIGH && (now - lastDebounceIntrusion > DEBOUNCE_DELAY)) {
    lastDebounceIntrusion = now;
    if (!entryGateOpen) {
      intrusion_active = true;
      Serial.println("[ALARM] SECURITY BREACH! Unauthorized entry detected at Main Gate!");
      pendingGateEventJson = "{\"action\":\"ENTER\",\"rfid\":\"INTRUSION_ALERT\",\"driver\":\"UNAUTHORIZED\",\"type\":\"SECURITY_BREACH\"}";
      pendingGateEvent = true;
    }
  }
  last_intrusion = currentIntrusion;

  // 3. Pre-exit IR check to open Exit Gate
  bool currentExitIR = digitalRead(IR_EXIT_GATE_PIN);
  if (currentExitIR == LOW && last_exit_ir == HIGH && (now - lastDebounceExit > DEBOUNCE_DELAY)) {
    lastDebounceExit = now;
    if (!exitGateOpen) {
      exitServo.write(90);
      exitGateOpen  = true;
      exitGateTimer = now;
      Serial.println("[GATE] Vehicle departing. Exit barrier opened.");
      showLcdPopup("  VEHICLE EXITING   ", "  Goodbye & Drive   ", "      Safely!       ", "Exit Gate Open...   ");
    }
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
        showLcdPopup("  WIFI CONNECTED!   ", "Excel IP Address:   ", ip, "http://" + ip);
      }

      // Handle incoming Excel HTTP requests continuously without blocking
      excelServer.handleClient();

      // Handle MQTT Node-RED connection
      if (client.connected()) {
        mqttConnected = true;
        client.loop();

        if (pendingGateEvent) {
          pendingGateEvent = false;
          client.publish("garage/gate/event", pendingGateEventJson.c_str());
        }

        if (now - lastTelemetrySend >= 2000) {
          lastTelemetrySend = now;
          String payload = "{";
          payload += "\"f1_cars\":" + String(f1_cars) + ",";
          payload += "\"f2_temp\":" + String(t2, 1) + ",\"f2_smoke\":" + String(s2) + ",\"f2_cars\":" + String(f2_cars) + ",";
          payload += "\"f3_temp\":" + String(t3, 1) + ",\"f3_smoke\":" + String(s3) + ",\"f3_cars\":" + String(f3_cars);
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

  SPI.begin();
  rfid.PCD_Init();

  entryServo.attach(SERVO_ENTRY_PIN);
  exitServo.attach(SERVO_EXIT_PIN);
  entryServo.write(0); 
  exitServo.write(0);

  pinMode(IR_INTRUSION_PIN, INPUT_PULLUP);
  pinMode(IR_EXIT_GATE_PIN, INPUT_PULLUP);
  pinMode(MASTER_ALARM_PIN, OUTPUT);
  digitalWrite(MASTER_ALARM_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  setupExcelWebServer();
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