/*
 * ======================================================================================
 *  BOARD 2      : ESP32 SLAVE (Ground & First Floor: 2-IR, DHT11, Smoke, Flame & Fans)
 *  PROJECT NAME : Smart Industrial Garage (Trucks Yard on Master | Ground & First Floor on Slave)
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
// 2. Slave Hardware Pinout Configuration (Single-Lane 2-IR Sequence Counting)
//    IR_A = Outer sensor (outside/approach side of the floor lane)
//    IR_B = Inner sensor (inside/floor side of the floor lane)
//    Sequence: A -> B = ENTER (+1) | B -> A = EXIT (-1)
// ======================================================================================
// --- A) Ground Floor & First Floor Bi-Directional IR Sensor Pairs (Active LOW) ---
#define IR_GF_A     33  // Ground Floor Outer IR (A) - Max 10 Cars
#define IR_GF_B     32  // Ground Floor Inner IR (B)
#define IR_FF_A     27  // First Floor Outer IR (A) - Max 10 Cars
#define IR_FF_B     14  // First Floor Inner IR (B)

// --- B) Flame Sensors (Ground Floor & First Floor - Digital DO, Active LOW) ---
#define FLAME_GF_PIN 26 // Ground Floor Flame Sensor (DO)
#define FLAME_FF_PIN 25 // First Floor Flame Sensor (DO)

// --- C) Gas & Smoke Sensors (Ground Floor & First Floor - ADC1) ---
#define GAS_F2_PIN  34  // Ground Floor Smoke/Gas Sensor (AO)
#define GAS_F3_PIN  35  // First Floor Smoke/Gas Sensor (AO)

// --- D) Temperature & Humidity Sensors (Ground Floor & First Floor - DHT11) ---
#define DHTTYPE     DHT11
#define DHT_F2_PIN  18  // Ground Floor DHT11
#define DHT_F3_PIN  19  // First Floor DHT11
DHT dht2(DHT_F2_PIN, DHTTYPE);
DHT dht3(DHT_F3_PIN, DHTTYPE);

// --- E) Emergency Exhaust Fan Relays ---
// Most 5V optocoupler relay modules are Active-LOW (LOW = Fan ON, HIGH = Fan OFF).
// If you use Active-HIGH relays or MOSFETs, swap LOW and HIGH below.
#define RELAY_ON     LOW
#define RELAY_OFF    HIGH
#define F2_EMERG_PIN 15 // Ground Floor Exhaust Fan & Local Alarm Relay
#define F3_EMERG_PIN 4  // First Floor Exhaust Fan & Local Alarm Relay

// ======================================================================================
// 3. Bi-Directional Single-Lane IR State Machine
// ======================================================================================
enum LaneState {
  LANE_IDLE = 0,
  LANE_A_FIRST,      // Outer IR triggered first -> waiting for Inner IR (ENTER)
  LANE_B_FIRST,      // Inner IR triggered first -> waiting for Outer IR (EXIT)
  LANE_WAIT_CLEAR    // Wait until both sensors are unblocked before next vehicle
};

struct BiDirCounter {
  int pinOuter;      // IR_A
  int pinInner;      // IR_B
  int maxCap;
  int count;
  LaneState state;
  unsigned long stateTime;
  const char* zoneName;
};

BiDirCounter groundCounter = { IR_GF_A, IR_GF_B, 10, 0, LANE_IDLE, 0, "Ground Floor" };
BiDirCounter firstCounter  = { IR_FF_A, IR_FF_B, 10, 0, LANE_IDLE, 0, "First Floor"  };

const unsigned long LANE_TIMEOUT_MS = 4000; // Reset if vehicle reverses without completing pass
const unsigned long LANE_CLEAR_MS   = 300;  // Clear gap between consecutive vehicles

// ======================================================================================
// 4. Environmental State Variables & Safety Thresholds
// ======================================================================================
float t2 = 26.5, t3 = 28.0; // Temperature (°C)
float h2 = 50.0, h3 = 50.0; // Humidity (%)
int   s2 = 0,    s3 = 0;    // Smoke / Gas (%)
bool  fl2 = false, fl3 = false; // Flame detected (Active LOW sensor)
bool  f2_danger = false, f3_danger = false;

// Manual override flags received from Master ESP32 (Node-RED commands)
bool manual_f2_emerg = false;
bool manual_f3_emerg = false;

// Timers & Flame Debounce (prevents EMI/servo noise from false-triggering relays)
unsigned long lastSensorRead = 0;
unsigned long lastUartSend   = 0;
unsigned long flame2LowSince = 0;
unsigned long flame3LowSince = 0;
const unsigned long FLAME_DEBOUNCE_MS = 300;
bool immediateUartSync       = false;

// Thresholds with Hysteresis (prevents relay chatter near threshold)
const float TEMP_THRESHOLD_ON   = 45.0; // °C
const float TEMP_THRESHOLD_OFF  = 42.0; // °C
const int   SMOKE_THRESHOLD_ON  = 60;   // %
const int   SMOKE_THRESHOLD_OFF = 55;   // %

// Helper: Averaged analog read for stable gas/smoke readings
int readGasPercent(int pin) {
  long sum = 0;
  const int samples = 8;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
  }
  int avg = sum / samples;
  return constrain(map(avg, 0, 4095, 0, 100), 0, 100);
}

// ======================================================================================
// 5. Send Live Telemetry Packet to Master ESP32 via UART2
//    (Master manages Trucks Yard f1_cars; Slave reports Ground Floor F2 & First Floor F3)
// ======================================================================================
void sendTelemetryToMaster() {
  String packet = "F2=" + String(groundCounter.count) +
                  ",F3=" + String(firstCounter.count) +
                  ",T2=" + String(t2, 1) +
                  ",H2=" + String((int)h2) +
                  ",S2=" + String(s2) +
                  ",FL2=" + String(fl2 ? 1 : 0) +
                  ",D2=" + String(f2_danger ? 1 : 0) +
                  ",T3=" + String(t3, 1) +
                  ",H3=" + String((int)h3) +
                  ",S3=" + String(s3) +
                  ",FL3=" + String(fl3 ? 1 : 0) +
                  ",D3=" + String(f3_danger ? 1 : 0) + "\n";

  Serial2.print(packet);
}

// ======================================================================================
// 6. Receive Manual Override Commands from Master ESP32 via UART2
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

        Serial.printf("[UART RX FROM MASTER] Manual Override -> GF_Fan:%d | FF_Fan:%d\n", manual_f2_emerg, manual_f3_emerg);
      }
      uartRxBuffer = "";
    } else if (uartRxBuffer.length() < 128) {
      uartRxBuffer += c;
    } else {
      uartRxBuffer = "";
    }
  }
}

// ======================================================================================
// 7. Sequence-Based Bi-Directional Counting (Single Lane, 2 IR Sensors per Floor)
// ======================================================================================
void updateBiDirCounter(BiDirCounter& c, unsigned long now) {
  bool aBlocked = (digitalRead(c.pinOuter) == LOW); // Active LOW
  bool bBlocked = (digitalRead(c.pinInner) == LOW); // Active LOW

  switch (c.state) {
    case LANE_IDLE:
      if (aBlocked && !bBlocked) {
        c.state = LANE_A_FIRST;
        c.stateTime = now;
      } else if (bBlocked && !aBlocked) {
        c.state = LANE_B_FIRST;
        c.stateTime = now;
      }
      break;

    case LANE_A_FIRST:
      if (bBlocked) {
        if (c.count < c.maxCap) {
          c.count++;
          immediateUartSync = true;
        }
        Serial.printf("[SLAVE COUNT] %s ENTER (A->B) -> %d/%d\n", c.zoneName, c.count, c.maxCap);
        c.state = LANE_WAIT_CLEAR;
        c.stateTime = now;
      } else if (now - c.stateTime > LANE_TIMEOUT_MS) {
        c.state = LANE_IDLE;
      }
      break;

    case LANE_B_FIRST:
      if (aBlocked) {
        if (c.count > 0) {
          c.count--;
          immediateUartSync = true;
        }
        Serial.printf("[SLAVE COUNT] %s EXIT  (B->A) -> %d/%d\n", c.zoneName, c.count, c.maxCap);
        c.state = LANE_WAIT_CLEAR;
        c.stateTime = now;
      } else if (now - c.stateTime > LANE_TIMEOUT_MS) {
        c.state = LANE_IDLE;
      }
      break;

    case LANE_WAIT_CLEAR:
      if (!aBlocked && !bBlocked) {
        if (now - c.stateTime >= LANE_CLEAR_MS) {
          c.state = LANE_IDLE;
        }
      } else {
        c.stateTime = now;
      }
      break;
  }
}

void handleFloorCounting() {
  unsigned long now = millis();
  updateBiDirCounter(groundCounter, now);
  updateBiDirCounter(firstCounter, now);
}

// ======================================================================================
// 8. Environmental Monitoring (Flame + DHT11 Temp/Humidity + MQ Gas) & Emergency Fans
// ======================================================================================
void handleSensorsAndFans() {
  // 1. Debounced Flame Sensor polling (Active LOW, must hold LOW for 300ms to ignore servo EMI)
  unsigned long now = millis();
  bool prev_fl2 = fl2, prev_fl3 = fl3;

  if (digitalRead(FLAME_GF_PIN) == LOW) {
    if (flame2LowSince == 0) flame2LowSince = now;
    if (now - flame2LowSince >= FLAME_DEBOUNCE_MS) fl2 = true;
  } else {
    flame2LowSince = 0;
    fl2 = false;
  }

  if (digitalRead(FLAME_FF_PIN) == LOW) {
    if (flame3LowSince == 0) flame3LowSince = now;
    if (now - flame3LowSince >= FLAME_DEBOUNCE_MS) fl3 = true;
  } else {
    flame3LowSince = 0;
    fl3 = false;
  }

  if (fl2) f2_danger = true;
  if (fl3) f3_danger = true;

  if (fl2 != prev_fl2 || fl3 != prev_fl3) {
    if (fl2) Serial.println("[FLAME ALERT] FIRE DETECTED ON GROUND FLOOR!");
    if (fl3) Serial.println("[FLAME ALERT] FIRE DETECTED ON FIRST FLOOR!");
    immediateUartSync = true;
  }

  // 2. Drive exhaust fan relays immediately (Active-LOW relay logic)
  digitalWrite(F2_EMERG_PIN, (f2_danger || manual_f2_emerg) ? RELAY_ON : RELAY_OFF);
  digitalWrite(F3_EMERG_PIN, (f3_danger || manual_f3_emerg) ? RELAY_ON : RELAY_OFF);

  if (millis() - lastSensorRead < 1500) return;
  lastSensorRead = millis();

  // 3. Keep last valid readings on transient DHT11 read errors
  float read_t2 = dht2.readTemperature();
  float read_h2 = dht2.readHumidity();
  if (!isnan(read_t2)) t2 = read_t2;
  else Serial.println("[WARN] DHT11 Ground Floor temp read failed, keeping last valid.");
  if (!isnan(read_h2)) h2 = read_h2;

  float read_t3 = dht3.readTemperature();
  float read_h3 = dht3.readHumidity();
  if (!isnan(read_t3)) t3 = read_t3;
  else Serial.println("[WARN] DHT11 First Floor temp read failed, keeping last valid.");
  if (!isnan(read_h3)) h3 = read_h3;

  s2 = readGasPercent(GAS_F2_PIN);
  s3 = readGasPercent(GAS_F3_PIN);

  bool prev_d2 = f2_danger, prev_d3 = f3_danger;

  // Hysteresis logic for Ground Floor (Flame overrides immediately)
  if (!f2_danger) {
    if (fl2 || t2 >= TEMP_THRESHOLD_ON || s2 >= SMOKE_THRESHOLD_ON) f2_danger = true;
  } else {
    if (!fl2 && t2 < TEMP_THRESHOLD_OFF && s2 < SMOKE_THRESHOLD_OFF) f2_danger = false;
  }

  // Hysteresis logic for First Floor (Flame overrides immediately)
  if (!f3_danger) {
    if (fl3 || t3 >= TEMP_THRESHOLD_ON || s3 >= SMOKE_THRESHOLD_ON) f3_danger = true;
  } else {
    if (!fl3 && t3 < TEMP_THRESHOLD_OFF && s3 < SMOKE_THRESHOLD_OFF) f3_danger = false;
  }

  // Trigger instant UART update if hazard state changes
  if (f2_danger != prev_d2 || f3_danger != prev_d3) {
    immediateUartSync = true;
  }
}

// ======================================================================================
// 9. Slave System Initialization (Setup)
// ======================================================================================
void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  Serial.println("\n=======================================================");
  Serial.println(" SMART GARAGE SLAVE ESP32 - Ground & First Floor Node");
  Serial.println(" Ground Floor (10 Cars) | First Floor (10 Cars)");
  Serial.println(" Sensors: 2-IR + DHT11 + MQ Smoke + Flame (GPIO 26/25)");
  Serial.println("=======================================================");

  dht2.begin();
  dht3.begin();

  pinMode(IR_GF_A, INPUT_PULLUP);      pinMode(IR_GF_B, INPUT_PULLUP);
  pinMode(IR_FF_A, INPUT_PULLUP);      pinMode(IR_FF_B, INPUT_PULLUP);
  pinMode(FLAME_GF_PIN, INPUT_PULLUP); pinMode(FLAME_FF_PIN, INPUT_PULLUP);

  pinMode(F2_EMERG_PIN, OUTPUT);
  pinMode(F3_EMERG_PIN, OUTPUT);
  digitalWrite(F2_EMERG_PIN, RELAY_OFF);
  digitalWrite(F3_EMERG_PIN, RELAY_OFF);
}

// ======================================================================================
// 10. Slave Main Real-Time Loop
// ======================================================================================
void loop() {
  handleMasterCommands(); // 1. Listen for fan override commands from Master
  handleFloorCounting();  // 2. Sequence-based 2-IR counting for Ground & First Floor
  handleSensorsAndFans(); // 3. Read Flame, DHT11 (Temp+Hum) & Gas sensors and trigger fans

  // 4. Send telemetry to Master every 400 ms OR immediately when a car passes / fire detected
  if (immediateUartSync || (millis() - lastUartSend >= 400)) {
    immediateUartSync = false;
    lastUartSend = millis();
    sendTelemetryToMaster();
  }
}
