/*
 * ======================================================================================
 *  BOARD 2      : ESP32 SLAVE (3-Floor IR Counters, DHT11, Smoke Sensors & Exhaust Fans)
 *  PROJECT NAME : Smart 3-Floor Industrial Garage & ASRS Control System (IoT)
 *  ARCHITECTURE : Pure Real-Time Sensor & Actuator Node (Communicates with Master via UART2)
 * ======================================================================================
 */

#include <DHT.h>

// ======================================================================================
// 1. UART2 Configuration (Link to Master ESP32: RX2 = GPIO 16 | TX2 = GPIO 17)
// ======================================================================================
#define UART_RX_PIN 16
#define UART_TX_PIN 17
String uartRxBuffer = "";

// ======================================================================================
// 2. Slave Hardware Pinout Configuration (Safe Pins Only - No Boot Conflicts!)
// ======================================================================================
// --- A) 3-Floor Vehicle Counting IR Sensors (Active LOW with Internal Pull-Up) ---
#define IR_F1_IN    26  // Floor 1 Entry IR (Outdoor Heavy Trucks Zone - Max 5)
#define IR_F1_OUT   25  // Floor 1 Exit IR
#define IR_F2_IN    33  // Floor 2 Entry IR (Indoor Sedans Zone A - Max 10)
#define IR_F2_OUT   32  // Floor 2 Exit IR
#define IR_F3_IN    27  // Floor 3 Entry IR (Indoor Sedans Zone B - Max 10) [Moved from 39]
#define IR_F3_OUT   14  // Floor 3 Exit IR [Moved from GPIO 0]

// --- B) Gas & Smoke Sensors (Indoor Floors 2 & 3 - ADC1) ---
#define GAS_F2_PIN  34  // Floor 2 Smoke/Gas Sensor
#define GAS_F3_PIN  35  // Floor 3 Smoke/Gas Sensor

// --- C) Temperature Sensors (Indoor Floors 2 & 3 - DHT11) ---
#define DHTTYPE     DHT11
#define DHT_F2_PIN  18  // Floor 2 DHT11 [Moved from 16 to free UART2]
#define DHT_F3_PIN  19  // Floor 3 DHT11 [Moved from 17 to free UART2]
DHT dht2(DHT_F2_PIN, DHTTYPE);
DHT dht3(DHT_F3_PIN, DHTTYPE);

// --- D) Emergency Exhaust Fan Relays ---
#define F2_EMERG_PIN 15 // Floor 2 Exhaust Fan & Local Alarm Relay
#define F3_EMERG_PIN 4  // Floor 3 Exhaust Fan & Local Alarm Relay

// ======================================================================================
// 3. Slave State Variables & Safety Thresholds
// ======================================================================================
int f1_cars = 0; // Max 5
int f2_cars = 0; // Max 10
int f3_cars = 0; // Max 10

float t2 = 26.5, t3 = 28.0;
int   s2 = 10,   s3 = 10;
bool  f2_danger = false, f3_danger = false;

// Manual override flags received from Master ESP32 (Node-RED commands)
bool manual_f2_emerg = false;
bool manual_f3_emerg = false;

// Edge detection & Non-blocking Debounce for 6 IR sensors
bool last_f1_in = HIGH, last_f1_out = HIGH;
bool last_f2_in = HIGH, last_f2_out = HIGH;
bool last_f3_in = HIGH, last_f3_out = HIGH;
unsigned long lastDebounceTime[6] = {0};
const unsigned long DEBOUNCE_DELAY = 250;

// Timers
unsigned long lastSensorRead = 0;
unsigned long lastUartSend   = 0;
bool immediateUartSync       = false;

// Thresholds
const float TEMP_THRESHOLD  = 45.0; // °C
const int   SMOKE_THRESHOLD = 60;   // %

// ======================================================================================
// 4. Send Live Telemetry Packet to Master ESP32 via UART2
// ======================================================================================
void sendTelemetryToMaster() {
  String packet = "F1=" + String(f1_cars) +
                  ",F2=" + String(f2_cars) +
                  ",F3=" + String(f3_cars) +
                  ",T2=" + String(t2, 1) +
                  ",S2=" + String(s2) +
                  ",D2=" + String(f2_danger ? 1 : 0) +
                  ",T3=" + String(t3, 1) +
                  ",S3=" + String(s3) +
                  ",D3=" + String(f3_danger ? 1 : 0) + "\n";

  Serial2.print(packet);
}

// ======================================================================================
// 5. Receive Manual Override Commands from Master ESP32 via UART2
// ======================================================================================
void handleMasterCommands() {
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      uartRxBuffer.trim();
      // Expected format: CMD:F2=1,F3=0
      if (uartRxBuffer.startsWith("CMD:")) {
        int f2Idx = uartRxBuffer.indexOf("F2=");
        int f3Idx = uartRxBuffer.indexOf("F3=");
        if (f2Idx != -1) manual_f2_emerg = (uartRxBuffer.substring(f2Idx + 3, f2Idx + 4).toInt() == 1);
        if (f3Idx != -1) manual_f3_emerg = (uartRxBuffer.substring(f3Idx + 3, f3Idx + 4).toInt() == 1);
        
        Serial.printf("[UART RX FROM MASTER] Manual Override -> F2_Fan:%d | F3_Fan:%d\n", manual_f2_emerg, manual_f3_emerg);
      }
      uartRxBuffer = "";
    } else {
      uartRxBuffer += c;
    }
  }
}

// ======================================================================================
// 6. Bi-Directional Vehicle Counting Across Floors 1, 2, and 3
// ======================================================================================
void handleFloorCounting() {
  unsigned long now = millis();

  // --- Floor 1 (Max: 5 Heavy Trucks) ---
  bool c_f1_in  = digitalRead(IR_F1_IN);
  bool c_f1_out = digitalRead(IR_F1_OUT);
  if (c_f1_in == LOW && last_f1_in == HIGH && (now - lastDebounceTime[0] > DEBOUNCE_DELAY)) {
    lastDebounceTime[0] = now;
    if (f1_cars < 5) { f1_cars++; immediateUartSync = true; }
    Serial.printf("[SLAVE COUNT] Floor 1 IN -> %d/5\n", f1_cars);
  }
  if (c_f1_out == LOW && last_f1_out == HIGH && (now - lastDebounceTime[1] > DEBOUNCE_DELAY)) {
    lastDebounceTime[1] = now;
    if (f1_cars > 0) { f1_cars--; immediateUartSync = true; }
    Serial.printf("[SLAVE COUNT] Floor 1 OUT -> %d/5\n", f1_cars);
  }
  last_f1_in = c_f1_in; last_f1_out = c_f1_out;

  // --- Floor 2 (Max: 10 Sedans) ---
  bool c_f2_in  = digitalRead(IR_F2_IN);
  bool c_f2_out = digitalRead(IR_F2_OUT);
  if (c_f2_in == LOW && last_f2_in == HIGH && (now - lastDebounceTime[2] > DEBOUNCE_DELAY)) {
    lastDebounceTime[2] = now;
    if (f2_cars < 10) { f2_cars++; immediateUartSync = true; }
    Serial.printf("[SLAVE COUNT] Floor 2 IN -> %d/10\n", f2_cars);
  }
  if (c_f2_out == LOW && last_f2_out == HIGH && (now - lastDebounceTime[3] > DEBOUNCE_DELAY)) {
    lastDebounceTime[3] = now;
    if (f2_cars > 0) { f2_cars--; immediateUartSync = true; }
    Serial.printf("[SLAVE COUNT] Floor 2 OUT -> %d/10\n", f2_cars);
  }
  last_f2_in = c_f2_in; last_f2_out = c_f2_out;

  // --- Floor 3 (Max: 10 Sedans) ---
  bool c_f3_in  = digitalRead(IR_F3_IN);
  bool c_f3_out = digitalRead(IR_F3_OUT);
  if (c_f3_in == LOW && last_f3_in == HIGH && (now - lastDebounceTime[4] > DEBOUNCE_DELAY)) {
    lastDebounceTime[4] = now;
    if (f3_cars < 10) { f3_cars++; immediateUartSync = true; }
    Serial.printf("[SLAVE COUNT] Floor 3 IN -> %d/10\n", f3_cars);
  }
  if (c_f3_out == LOW && last_f3_out == HIGH && (now - lastDebounceTime[5] > DEBOUNCE_DELAY)) {
    lastDebounceTime[5] = now;
    if (f3_cars > 0) { f3_cars--; immediateUartSync = true; }
    Serial.printf("[SLAVE COUNT] Floor 3 OUT -> %d/10\n", f3_cars);
  }
  last_f3_in = c_f3_in; last_f3_out = c_f3_out;
}

// ======================================================================================
// 7. Environmental Monitoring & Instant Emergency Fan Actuation
// ======================================================================================
void handleSensorsAndFans() {
  // Drive exhaust fan relays immediately
  digitalWrite(F2_EMERG_PIN, (f2_danger || manual_f2_emerg) ? HIGH : LOW);
  digitalWrite(F3_EMERG_PIN, (f3_danger || manual_f3_emerg) ? HIGH : LOW);

  if (millis() - lastSensorRead < 1500) return;
  lastSensorRead = millis();

  float read_t2 = dht2.readTemperature(); t2 = isnan(read_t2) ? 26.5 : read_t2;
  float read_t3 = dht3.readTemperature(); t3 = isnan(read_t3) ? 28.0 : read_t3;

  s2 = map(analogRead(GAS_F2_PIN), 0, 4095, 0, 100);
  s3 = map(analogRead(GAS_F3_PIN), 0, 4095, 0, 100);

  bool prev_d2 = f2_danger, prev_d3 = f3_danger;
  f2_danger = (t2 >= TEMP_THRESHOLD || s2 >= SMOKE_THRESHOLD);
  f3_danger = (t3 >= TEMP_THRESHOLD || s3 >= SMOKE_THRESHOLD);

  // Trigger instant UART update if hazard state changes
  if (f2_danger != prev_d2 || f3_danger != prev_d3) {
    immediateUartSync = true;
  }
}

// ======================================================================================
// 8. Slave System Initialization (Setup)
// ======================================================================================
void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  Serial.println("\n=======================================================");
  Serial.println(" SMART GARAGE SLAVE ESP32 - Floors & Environment Node");
  Serial.println("=======================================================");

  dht2.begin();
  dht3.begin();

  pinMode(IR_F1_IN, INPUT_PULLUP); pinMode(IR_F1_OUT, INPUT_PULLUP);
  pinMode(IR_F2_IN, INPUT_PULLUP); pinMode(IR_F2_OUT, INPUT_PULLUP);
  pinMode(IR_F3_IN, INPUT_PULLUP); pinMode(IR_F3_OUT, INPUT_PULLUP);

  pinMode(F2_EMERG_PIN, OUTPUT);
  pinMode(F3_EMERG_PIN, OUTPUT);
  digitalWrite(F2_EMERG_PIN, LOW);
  digitalWrite(F3_EMERG_PIN, LOW);
}

// ======================================================================================
// 9. Slave Main Real-Time Loop
// ======================================================================================
void loop() {
  handleMasterCommands(); // 1. Listen for fan override commands from Master
  handleFloorCounting();  // 2. Count vehicles on Floors 1, 2, and 3
  handleSensorsAndFans(); // 3. Read DHT11 & Gas sensors and trigger exhaust fans

  // 4. Send telemetry to Master every 400 ms OR immediately when a car passes
  if (immediateUartSync || (millis() - lastUartSend >= 400)) {
    immediateUartSync = false;
    lastUartSend = millis();
    sendTelemetryToMaster();
  }
}