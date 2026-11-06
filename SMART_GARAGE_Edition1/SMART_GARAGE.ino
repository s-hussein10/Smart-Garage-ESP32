/*
 * ======================================================================================
 *  PROJECT NAME : Smart 3-Floor Industrial Garage & ASRS Control System (IoT)
 *  ARCHITECTURE : Dual-Core Offline-First (Core 1: Real-Time Hardware | Core 0: WiFi/MQTT)
 * ======================================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ======================================================================================
// 1. Network & MQTT Broker Configuration (Node-RED Server)
// ======================================================================================
const char* ssid        = "Seif's iPhone"; // Wi-Fi Network SSID
const char* password    = "12345678";      // Wi-Fi Network Password
const char* mqtt_server = "172.20.10.3";   // Laptop IPv4 Address (running Node-RED MQTT Broker)
const int   mqtt_port   = 1883;            // Default MQTT Broker Port

WiFiClient espClient;
PubSubClient client(espClient);

// Network state flags managed asynchronously on Core 0 (zero hardware blocking)
volatile bool wifiConnected           = false;
volatile bool mqttConnected           = false;
volatile bool nodeRedVerified         = false;
bool          wifiPreviouslyConnected = false;
bool          mqttPreviouslyConnected = false;

// Thread-safe event buffer to pass gate events from Core 1 to Core 0
volatile bool pendingGateEvent     = false;
String        pendingGateEventJson = "";

// ======================================================================================
// 2. 20x4 I2C LCD Configuration (SDA -> GPIO 21 | SCL -> GPIO 22)
// ======================================================================================
LiquidCrystal_I2C lcd(0x27, 20, 4);

bool lcdEventActive = false;
unsigned long lcdEventTimer = 0;
const unsigned long LCD_EVENT_DURATION = 3000; // Popup display duration in milliseconds

// ======================================================================================
// 3. Hardware Pinout Configuration (Conflict-Free & Safe-Boot Mapping)
// ======================================================================================

// --- A) RFID Reader (RC522 via Hardware VSPI: SCK=18, MISO=19, MOSI=23) ---
#define SS_PIN           5   // SDA / Chip Select (CS) pin on RC522
#define RST_PIN          -1  // Tied directly to 3.3V to save GPIO pins
MFRC522 rfid(SS_PIN, RST_PIN);

// --- B) Gate Servo Motors ---
#define SERVO_ENTRY_PIN  13  // Main Entry Gate Barrier Servo
#define SERVO_EXIT_PIN   12  // Main Exit Gate Barrier Servo
Servo entryServo;
Servo exitServo;

// --- C) Gate Security & Access IR Sensors (Active LOW) ---
#define IR_INTRUSION_PIN 14  // Post-entry gate IR sensor for unauthorized intrusion detection
#define IR_EXIT_GATE_PIN 27  // Pre-exit gate IR sensor for automatic exit barrier opening

// --- D) 3-Floor Vehicle Counting IR Sensors (Active LOW) ---
#define IR_F1_IN         26  // Floor 1 Entry IR (Outdoor Heavy Trucks Zone)
#define IR_F1_OUT        25  // Floor 1 Exit IR
#define IR_F2_IN         33  // Floor 2 Entry IR (Indoor Sedans Zone A)
#define IR_F2_OUT        32  // Floor 2 Exit IR
#define IR_F3_IN         39  // Floor 3 Entry IR (VN Pin - Input Only, requires external pull-up)
#define IR_F3_OUT        0   // Floor 3 Exit IR (Also connected to onboard BOOT button)

// --- E) Gas & Smoke Sensors (Indoor Floors 2 & 3 Only - ADC1 Channels) ---
#define GAS_F2_PIN       34  // Floor 2 Smoke/Gas Sensor (ADC1_CH6)
#define GAS_F3_PIN       35  // Floor 3 Smoke/Gas Sensor (ADC1_CH7)

// --- F) Temperature Sensors (Indoor Floors 2 & 3 Only - DHT11) ---
#define DHTTYPE          DHT11
#define DHT_F2_PIN       16  // Floor 2 Temperature Sensor (RX2)
#define DHT_F3_PIN       17  // Floor 3 Temperature Sensor (TX2)
DHT dht2(DHT_F2_PIN, DHTTYPE);
DHT dht3(DHT_F3_PIN, DHTTYPE);

// --- G) Emergency Actuators (3 Independent Dedicated Output Pins) ---
#define MASTER_ALARM_PIN 2   // Master Alarm Buzzer + Gate Strobe LED (and Onboard Blue LED)
#define F2_EMERG_PIN     15  // Floor 2 Exhaust Ventilation Fan & Local Alarm Relay
#define F3_EMERG_PIN     4   // Floor 3 Exhaust Ventilation Fan & Local Alarm Relay

// ======================================================================================
// 4. System State Variables & Non-Blocking Timers
// ======================================================================================
bool entryGateOpen = false;
bool exitGateOpen  = false;
unsigned long entryGateTimer    = 0;
unsigned long exitGateTimer     = 0;
unsigned long lastSensorRead    = 0;
unsigned long lastLcdUpdateTime = 0;

// Real-time vehicle occupancy counters per floor
volatile int f1_cars = 0; // Floor 1 (Outdoor): Max capacity = 5 Heavy Trucks
volatile int f2_cars = 0; // Floor 2 (Indoor):  Max capacity = 10 Passenger Cars
volatile int f3_cars = 0; // Floor 3 (Indoor):  Max capacity = 10 Passenger Cars

// Environmental readings for indoor monitored zones (Floors 2 & 3)
volatile float t2 = 26.5, t3 = 28.0;
volatile int   s2 = 10,   s3 = 10;
volatile bool  f2_danger = false, f3_danger = false;

// Previous sensor states for falling-edge detection
bool last_f1_in = HIGH, last_f1_out = HIGH;
bool last_f2_in = HIGH, last_f2_out = HIGH;
bool last_f3_in = HIGH, last_f3_out = HIGH;
bool last_intrusion = HIGH;
bool last_exit_ir   = HIGH;

unsigned long lastDebounceTime[8] = {0};
const unsigned long DEBOUNCE_DELAY = 250; // 250 ms non-blocking debounce time

// Manual override flags controlled via Node-RED Dashboard switches
volatile bool manual_master_alarm = false;
volatile bool manual_f2_emerg     = false;
volatile bool manual_f3_emerg     = false;
volatile bool intrusion_active    = false;

// Environmental hazard safety thresholds
const float TEMP_THRESHOLD  = 45.0; // Fire hazard temperature threshold (°C)
const int   SMOKE_THRESHOLD = 60;   // Smoke/Gas hazard concentration threshold (%)

// ======================================================================================
// 5. Helper Functions for 20x4 LCD Formatting & MQTT Error Diagnostics
// ======================================================================================
void showLcdPopup(String line0, String line1, String line2, String line3) {
  lcdEventActive = true;
  lcdEventTimer  = millis();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(line0.substring(0, 20));
  lcd.setCursor(0, 1); lcd.print(line1.substring(0, 20));
  lcd.setCursor(0, 2); lcd.print(line2.substring(0, 20));
  lcd.setCursor(0, 3); lcd.print(line3.substring(0, 20));
}

// Pads any string to exactly 20 characters to overwrite old characters without flickering
String pad20(String str) {
  while (str.length() < 20) str += " ";
  return str.substring(0, 20);
}

// Translates PubSubClient state codes into human-readable diagnostic messages
String getMqttErrorReason(int state) {
  switch (state) {
    case -4: return "Connection Timeout (Server didn't respond)";
    case -3: return "Connection Lost (Network issue)";
    case -2: return "Connect Failed (Broker Offline / Check IP & Port 1883)";
    case -1: return "Disconnected cleanly";
    case  1: return "Bad Protocol Version";
    case  2: return "Client ID Rejected";
    case  3: return "Broker Unavailable";
    case  4: return "Bad Username/Password";
    case  5: return "Not Authorized";
    default: return "Unknown MQTT Error";
  }
}

// ======================================================================================
// 6. Real-Time 20x4 LCD Dashboard Updater (Flicker-Free)
// ======================================================================================
void updateLCD() {
  unsigned long now = millis();

  // Hold temporary event popup screen until its timer expires
  if (lcdEventActive) {
    if (now - lcdEventTimer >= LCD_EVENT_DURATION) {
      lcdEventActive = false;
      lcd.clear();
    } else {
      return;
    }
  }

  // Refresh normal status screen every 500 ms
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
  String netStatus = mqttConnected ? "ONLINE" : (wifiConnected ? "NO-MQTT" : "OFFLINE");
  String gateState = (entryGateOpen || exitGateOpen) ? "OPEN" : "CLSD";
  lcd.setCursor(0, 0);
  lcd.print(pad20("SYS:" + netStatus + " GT:" + gateState));

  // Row 1: Floor 1 Outdoor Heavy Trucks Zone (Occupancy only, natural ventilation)
  lcd.setCursor(0, 1);
  lcd.print(pad20("F1(Trk):" + String(f1_cars) + "/5  [OPEN]"));

  // Row 2: Floor 2 Indoor Sedans Zone A (Occupancy + Temperature)
  lcd.setCursor(0, 2);
  lcd.print(pad20("F2(Car):" + String(f2_cars) + "/10 T:" + String((int)t2) + "C"));

  // Row 3: Floor 3 Indoor Sedans Zone B (Occupancy + Temperature)
  lcd.setCursor(0, 3);
  lcd.print(pad20("F3(Car):" + String(f3_cars) + "/10 T:" + String((int)t3) + "C"));
}

// ======================================================================================
// 7. MQTT Callback: Handles Incoming Manual Commands & Node-RED Verification
// ======================================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  String topicStr = String(topic);

  // Confirm active two-way communication with Node-RED upon receiving any message
  if (!nodeRedVerified) {
    nodeRedVerified = true;
    Serial.println("=======================================================");
    Serial.println("[NODE-RED] >>> VERIFIED! Active 2-Way Link Established <<<");
    Serial.println("=======================================================");
  }

  Serial.print("[NODE-RED -> ESP32] RX Topic: ");
  Serial.print(topicStr);
  Serial.print(" | Payload: ");
  Serial.println(msg);

  // Check for explicit Node-RED handshake acknowledgment
  if (topicStr == "garage/control/nodered_ack") {
    Serial.println("[NODE-RED] Handshake ACK received! Dashboard is ONLINE & synced.");
    return;
  }

  if (topicStr == "garage/control/master_alarm") {
    manual_master_alarm = (msg == "1");
    if (!manual_master_alarm) intrusion_active = false;
    Serial.printf("[NODE-RED CMD] Master Alarm set to: %s\n", manual_master_alarm ? "ON" : "OFF");
  } 
  else if (topicStr == "garage/control/f2_emergency") {
    manual_f2_emerg = (msg == "1");
    Serial.printf("[NODE-RED CMD] Floor 2 Emergency Fan set to: %s\n", manual_f2_emerg ? "ON" : "OFF");
  } 
  else if (topicStr == "garage/control/f3_emergency") {
    manual_f3_emerg = (msg == "1");
    Serial.printf("[NODE-RED CMD] Floor 3 Emergency Fan set to: %s\n", manual_f3_emerg ? "ON" : "OFF");
  }
}

// ======================================================================================
// 8. Helper Function: Process Authorized Vehicle Entry (Works 100% Offline & Online)
// ======================================================================================
void processVehicleEntry(String tagID, String driverName, String vehicleType) {
  // Deny access if all three floors have reached maximum capacity
  if (f1_cars >= 5 && f2_cars >= 10 && f3_cars >= 10) {
    Serial.println("[GATE] Garage is COMPLETELY FULL! Entry denied.");
    showLcdPopup("   ACCESS DENIED!   ", "  GARAGE IS FULL!   ", "F1:5/5 F2:10 F3:10  ", "Please wait outside ");
    return;
  }

  // Open main entry barrier (90 degrees) and start auto-close timer
  entryServo.write(90);
  entryGateOpen    = true;
  entryGateTimer   = millis();
  intrusion_active = false;

  Serial.println("[ENTRY GATE OPEN] Welcome: " + driverName + " (" + tagID + ")");
  showLcdPopup("  ACCESS GRANTED!   ", driverName, vehicleType, "Gate Opening...     ");

  // Queue the access event to be published by Core 0 in the background
  pendingGateEventJson = "{\"action\":\"ENTER\",\"rfid\":\"" + tagID + "\",\"driver\":\"" + driverName + "\",\"type\":\"" + vehicleType + "\"}";
  pendingGateEvent = true;
}

// ======================================================================================
// 9. RFID Access Control & Serial Monitor Test Commands ('1', '2')
// ======================================================================================
void handleRFID() {
  // A) Allow simulated vehicle entry via Serial Monitor ('1' for Sedan, '2' for Truck)
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    if (cmd == '1') processVehicleEntry("F9E8D7C6", "Eng. Seif Hussein", "Sedan (Floor 2)");
    if (cmd == '2') processVehicleEntry("A1B2C3D4", "Ahmed Ali", "Truck (Floor 1)");
  }

  // B) Poll physical RC522 RFID reader
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String tagID = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) tagID += "0";
    tagID += String(rfid.uid.uidByte[i], HEX);
  }
  tagID.toUpperCase();

  Serial.print("[RFID] Scanned Card UID: ");
  Serial.println(tagID);

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
// 10. Automated Gate Barriers & Unauthorized Intrusion Detection
// ======================================================================================
void handleGatesAndIntrusion() {
  unsigned long now = millis();

  // 1. Auto-close Entry Gate barrier after 4000 ms
  if (entryGateOpen && (now - entryGateTimer >= 4000)) {
    entryServo.write(0);
    entryGateOpen = false;
    Serial.println("[GATE] Entry barrier auto-closed.");
  }

  // 2. Intrusion Detection: Trigger alarm if post-entry IR beam is broken while gate is closed
  bool currentIntrusion = digitalRead(IR_INTRUSION_PIN);
  if (currentIntrusion == LOW && last_intrusion == HIGH && (now - lastDebounceTime[6] > DEBOUNCE_DELAY)) {
    lastDebounceTime[6] = now;
    if (!entryGateOpen) {
      intrusion_active = true;
      Serial.println("[ALARM] SECURITY BREACH! Unauthorized entry detected at Main Gate!");
      pendingGateEventJson = "{\"action\":\"ENTER\",\"rfid\":\"INTRUSION_ALERT\",\"driver\":\"UNAUTHORIZED\",\"type\":\"SECURITY_BREACH\"}";
      pendingGateEvent = true;
    }
  }
  last_intrusion = currentIntrusion;

  // 3. Auto-open Exit Gate barrier when outgoing vehicle triggers pre-exit IR sensor
  bool currentExitIR = digitalRead(IR_EXIT_GATE_PIN);
  if (currentExitIR == LOW && last_exit_ir == HIGH && (now - lastDebounceTime[7] > DEBOUNCE_DELAY)) {
    lastDebounceTime[7] = now;
    if (!exitGateOpen) {
      exitServo.write(90);
      exitGateOpen  = true;
      exitGateTimer = now;
      Serial.println("[GATE] Vehicle departing. Exit barrier opened.");
      showLcdPopup("  VEHICLE EXITING   ", "  Goodbye & Drive   ", "      Safely!       ", "Exit Gate Open...   ");
    }
  }
  last_exit_ir = currentExitIR;

  // 4. Auto-close Exit Gate barrier after 4000 ms
  if (exitGateOpen && (now - exitGateTimer >= 4000)) {
    exitServo.write(0);
    exitGateOpen = false;
    Serial.println("[GATE] Exit barrier auto-closed.");
  }
}

// ======================================================================================
// 11. Multi-Floor Bi-Directional Vehicle Counting (Floors 1, 2, and 3)
// ======================================================================================
void handleFloorCounting() {
  unsigned long now = millis();

  // --- Floor 1: Outdoor Heavy Trucks Zone (Max Capacity: 5) ---
  bool c_f1_in  = digitalRead(IR_F1_IN);
  bool c_f1_out = digitalRead(IR_F1_OUT);
  if (c_f1_in == LOW && last_f1_in == HIGH && (now - lastDebounceTime[0] > DEBOUNCE_DELAY)) {
    lastDebounceTime[0] = now;
    if (f1_cars < 5) f1_cars++;
    Serial.printf("[COUNT] Floor 1 (Outdoor) IN -> Total: %d/5\n", f1_cars);
  }
  if (c_f1_out == LOW && last_f1_out == HIGH && (now - lastDebounceTime[1] > DEBOUNCE_DELAY)) {
    lastDebounceTime[1] = now;
    if (f1_cars > 0) f1_cars--;
    Serial.printf("[COUNT] Floor 1 (Outdoor) OUT -> Total: %d/5\n", f1_cars);
  }
  last_f1_in = c_f1_in; last_f1_out = c_f1_out;

  // --- Floor 2: Indoor Sedans Zone A (Max Capacity: 10) ---
  bool c_f2_in  = digitalRead(IR_F2_IN);
  bool c_f2_out = digitalRead(IR_F2_OUT);
  if (c_f2_in == LOW && last_f2_in == HIGH && (now - lastDebounceTime[2] > DEBOUNCE_DELAY)) {
    lastDebounceTime[2] = now;
    if (f2_cars < 10) f2_cars++;
    Serial.printf("[COUNT] Floor 2 IN -> Total: %d/10\n", f2_cars);
  }
  if (c_f2_out == LOW && last_f2_out == HIGH && (now - lastDebounceTime[3] > DEBOUNCE_DELAY)) {
    lastDebounceTime[3] = now;
    if (f2_cars > 0) f2_cars--;
    Serial.printf("[COUNT] Floor 2 OUT -> Total: %d/10\n", f2_cars);
  }
  last_f2_in = c_f2_in; last_f2_out = c_f2_out;

  // --- Floor 3: Indoor Sedans Zone B (Max Capacity: 10) ---
  bool c_f3_in  = digitalRead(IR_F3_IN);
  bool c_f3_out = digitalRead(IR_F3_OUT);
  if (c_f3_in == LOW && last_f3_in == HIGH && (now - lastDebounceTime[4] > DEBOUNCE_DELAY)) {
    lastDebounceTime[4] = now;
    if (f3_cars < 10) f3_cars++;
    Serial.printf("[COUNT] Floor 3 IN -> Total: %d/10\n", f3_cars);
  }
  if (c_f3_out == LOW && last_f3_out == HIGH && (now - lastDebounceTime[5] > DEBOUNCE_DELAY)) {
    lastDebounceTime[5] = now;
    if (f3_cars > 0) f3_cars--;
    Serial.printf("[COUNT] Floor 3 OUT -> Total: %d/10\n", f3_cars);
  }
  last_f3_in = c_f3_in; last_f3_out = c_f3_out;
}

// ======================================================================================
// 12. Local Environmental Monitoring & Instant Actuators (Runs 100% Locally on Core 1)
// ======================================================================================
void handleLocalSensorsAndActuators() {
  // Instant zero-latency update for emergency buzzer and exhaust fans
  bool any_danger = (f2_danger || f3_danger || manual_master_alarm || intrusion_active);
  digitalWrite(MASTER_ALARM_PIN, any_danger ? HIGH : LOW);
  digitalWrite(F2_EMERG_PIN,     (f2_danger || manual_f2_emerg) ? HIGH : LOW);
  digitalWrite(F3_EMERG_PIN,     (f3_danger || manual_f3_emerg) ? HIGH : LOW);

  // Sample environmental sensors every 2000 ms
  if (millis() - lastSensorRead < 2000) return;
  lastSensorRead = millis();

  // 1. Read temperatures (°C) for indoor floors 2 & 3 (with safe fallback values if disconnected)
  float read_t2 = dht2.readTemperature(); t2 = isnan(read_t2) ? 26.5 : read_t2;
  float read_t3 = dht3.readTemperature(); t3 = isnan(read_t3) ? 28.0 : read_t3;

  // 2. Read analog Gas/Smoke sensors (12-bit ADC: 0-4095) and map to percentage (0-100%)
  s2 = map(analogRead(GAS_F2_PIN), 0, 4095, 0, 100);
  s3 = map(analogRead(GAS_F3_PIN), 0, 4095, 0, 100);

  // 3. Evaluate fire or gas hazard conditions for indoor floors 2 & 3
  f2_danger = (t2 >= TEMP_THRESHOLD || s2 >= SMOKE_THRESHOLD);
  f3_danger = (t3 >= TEMP_THRESHOLD || s3 >= SMOKE_THRESHOLD);
}
// ======================================================================================
// 13. Background Network Task (Runs Independently on Core 0 - Clean Wi-Fi State Machine)
// ======================================================================================
void networkTask(void * parameter) {
  unsigned long lastMqttRetry     = 0;
  unsigned long lastTelemetrySend = 0;

  espClient.setTimeout(2);
  client.setSocketTimeout(2);

  for (;;) {
    unsigned long now = millis();
    wl_status_t wifiStatus = WiFi.status();

    // --- 1. Monitor Wi-Fi connectivity with clean state management ---
    if (wifiStatus == WL_CONNECTED) {
      wifiConnected = true;
      if (!wifiPreviouslyConnected) {
        wifiPreviouslyConnected = true;
        Serial.println("\n=======================================================");
        Serial.println("[WIFI] >>> Connected to Wi-Fi Network Successfully! <<<");
        Serial.print("[WIFI] ESP32 IP Address : ");
        Serial.println(WiFi.localIP());
        Serial.print("[WIFI] Target Broker IP : ");
        Serial.println(mqtt_server);
        Serial.println("=======================================================\n");
      }
    } 
    else {
      wifiConnected = false;
      mqttConnected = false;
      nodeRedVerified = false;

      if (wifiPreviouslyConnected) {
        wifiPreviouslyConnected = false;
        mqttPreviouslyConnected = false;
        Serial.println("\n[WIFI WARNING] Wi-Fi Disconnected! Operating in OFFLINE MODE.");
      }

      // Check if ESP32 is NOT currently attempting a connection before triggering reconnect
      if (wifiStatus != WL_DISCONNECTED && wifiStatus != WL_CONNECTION_LOST && wifiStatus != WL_NO_SSID_AVAIL) {
        // It is currently trying to connect, so we just wait and DO NOT spam reconnect commands
    } else {
        // Safe to trigger reconnection attempt
        WiFi.begin(ssid, password);
        vTaskDelay(2000 / portTICK_PERIOD_MS); // Give it time to initiate
      }
    }

    // --- 2. Manage MQTT Broker & Node-RED synchronization when Wi-Fi is available ---
    if (wifiConnected) {
      if (client.connected()) {
        mqttConnected = true;
        client.loop();

        if (pendingGateEvent) {
          pendingGateEvent = false;
          if (client.publish("garage/gate/event", pendingGateEventJson.c_str())) {
            Serial.println("[MQTT -> NODE-RED] Gate Event published successfully.");
          }
        }

        if (now - lastTelemetrySend >= 2000) {
          lastTelemetrySend = now;
          String payload = "{";
          payload += "\"f1_cars\":" + String(f1_cars) + ",";
          payload += "\"f2_temp\":" + String(t2, 1) + ",\"f2_smoke\":" + String(s2) + ",\"f2_cars\":" + String(f2_cars) + ",";
          payload += "\"f3_temp\":" + String(t3, 1) + ",\"f3_smoke\":" + String(s3) + ",\"f3_cars\":" + String(f3_cars);
          payload += "}";

          if (client.publish("garage/telemetry", payload.c_str())) {
            Serial.print("[MQTT -> NODE-RED] Telemetry Sent OK: ");
            Serial.println(payload);
          }
        }
      } 
      else {
        mqttConnected = false;
        nodeRedVerified = false;

        if (now - lastMqttRetry > 5000) {
          lastMqttRetry = now;
          if (client.connect("ESP32_SmartGarage_Seif")) {
            mqttPreviouslyConnected = true;
            mqttConnected = true;
            Serial.println("[MQTT] Connected to Broker Successfully!");
            client.subscribe("garage/control/#");
            
            String bootMsg = "{\"status\":\"ESP32_ONLINE\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
            client.publish("garage/system/status", bootMsg.c_str(), true);
          }
        }
      }
    }

    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}
// ======================================================================================
// 14. System Initialization (Setup)
// ======================================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=======================================================");
  Serial.println(" Smart 3-Floor Garage System - By Eng. Seif Hussein");
  Serial.println(" Architecture: Dual-Core Offline-First & IoT Ready");
  Serial.println("=======================================================");

  // 1. Initialize 20x4 I2C LCD (SDA = GPIO 21, SCL = GPIO 22)
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(" SMART GARAGE SYS");
  lcd.setCursor(0, 1); lcd.print("  Eng. Seif Hussein ");
  lcd.setCursor(0, 2); lcd.print(" Mechatronics - DUST");
  lcd.setCursor(0, 3); lcd.print(" Booting Hardware...");
  delay(1200);

  // 2. Initialize SPI bus, RC522 RFID reader, and indoor DHT11 sensors
  SPI.begin();
  rfid.PCD_Init();
  dht2.begin(); 
  dht3.begin();

  // 3. Attach and initialize gate servos to closed position (0 degrees)
  entryServo.attach(SERVO_ENTRY_PIN);
  exitServo.attach(SERVO_EXIT_PIN);
  entryServo.write(0); 
  exitServo.write(0);

  // 4. Configure IR sensor input pins
  pinMode(IR_INTRUSION_PIN, INPUT_PULLUP);
  pinMode(IR_EXIT_GATE_PIN, INPUT_PULLUP);
  pinMode(IR_F1_IN, INPUT_PULLUP); pinMode(IR_F1_OUT, INPUT_PULLUP);
  pinMode(IR_F2_IN, INPUT_PULLUP); pinMode(IR_F2_OUT, INPUT_PULLUP);
  pinMode(IR_F3_IN, INPUT);        // GPIO 39 (VN) is input-only (requires external pull-up)
  pinMode(IR_F3_OUT, INPUT_PULLUP);

  // 5. Configure emergency actuator output pins
  pinMode(MASTER_ALARM_PIN, OUTPUT);
  pinMode(F2_EMERG_PIN, OUTPUT);
  pinMode(F3_EMERG_PIN, OUTPUT);
  digitalWrite(MASTER_ALARM_PIN, LOW);
  digitalWrite(F2_EMERG_PIN, LOW);
  digitalWrite(F3_EMERG_PIN, LOW);

  // 6. Start Wi-Fi asynchronously in the background (Zero Boot Delay if Offline!)
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  lcd.clear();
  Serial.println("[SYSTEM] Hardware Initialized! Starting Offline-First Real-Time Control...");

  // 7. Launch dedicated Network & MQTT Task on Core 0
  xTaskCreatePinnedToCore(
    networkTask,       // Task function
    "NetworkTask",     // Task name
    8192,              // Stack size in bytes
    NULL,              // Task input parameter
    1,                 // Task priority
    NULL,              // Task handle
    0                  // Pin task to Core 0 (Main loop runs on Core 1)
  );
}

// ======================================================================================
// 15. Main Real-Time Hardware Loop (Runs on Core 1 - 100% Immune to Network Delays)
// ======================================================================================
void loop() {
  handleRFID();                     // 1. Poll RFID reader & Serial test commands ('1', '2')
  handleGatesAndIntrusion();        // 2. Manage gate barriers & unauthorized intrusion alarm
  handleFloorCounting();            // 3. Count vehicles across F1 (Outdoor) and F2/F3 (Indoor)
  handleLocalSensorsAndActuators(); // 4. Monitor F2/F3 hazards & drive emergency actuators locally
  updateLCD();                      // 5. Refresh 20x4 LCD status screen
}