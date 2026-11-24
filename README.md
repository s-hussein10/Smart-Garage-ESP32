# 🚗 3-Level Smart Industrial Parking Garage (IoT & Dual-ESP32)

An industrial 3-level smart parking garage system built on a **Dual-ESP32 (Master/Slave)** architecture for optimal load and GPIO pin balancing. The system features real-time monitoring and control via a **Node-RED** dashboard (MQTT), alongside a built-in **HTTP Excel** data endpoint (`/`, `/excel.csv`, `/data.json`) for multi-facility industrial synchronization.

---

## 🌟 Key Features

* **Dual-ESP32 Architecture (UART2):** Splits hardware tasks between a **Master ESP32** (Gates, RFID, 20x4 LCD, Wi-Fi, MQTT, HTTP Excel Server) and a **Slave ESP32** (3-Floor IR Counters, DHT11 Temperature, MQ Gas/Smoke Sensors, Exhaust Fan Relays).
* **Automated Access & Barrier Control:** Equipped with **RFID (RC522)** access control, **Servo-driven entry/exit barriers**, **Main Gate IR**, and **zone-aware capacity enforcement** (**Trucks Yard**: 6 Trucks tracked by individual RFID IDs + Main Gate IR, **Ground Floor**: 10 Cars, **First Floor**: 10 Cars — Total 26).
* **Safety & Environmental Control:** Real-time **Flame sensors**, temperature/humidity (`DHT11`), and smoke/gas (`MQ`) monitoring on indoor floors (**Ground Floor** & **First Floor**) with **hysteresis-controlled exhaust fan relays** (ON on Flame or ≥ 45°C / 60% smoke, OFF when no flame & < 42°C & < 55% smoke) and instant Master Alarm triggering.
* **IoT Dashboard, Excel & Google Sheets:** Direct 2-way **Node-RED Pro Dashboard** over MQTT with live ESP32/Wi-Fi/Slave indicators, automatic local **Excel CSV logging** (`garage_live_log.csv`), **Google Sheets** live sync (`google_sheets_apps_script.gs`), plus ESP32 local HTTP endpoints (`/`, `/excel.csv`, `/data.json`).

---

## 📂 Repository Structure

* **`SMAERT_GARAGE_ESP32_MASTER/`**: Source code for the Master ESP32 (Dual-Core: Core 1 real-time control + Core 0 Wi-Fi/MQTT/Excel HTTP Server).
* **`SMAERT_GARAGE_ESP32_SLAVE/`**: Source code for the Slave ESP32 managing all floor counters, environmental sensors, and exhaust fan relays.
* **`nodered_smart_garage_flow.json`**: Ready-to-import Node-RED Dashboard flow with live gate statistics (`Gate Live Status`, `Total Entered`, `Total Exited`, `Vehicles Inside Now`), floor gauges, and emergency controls.
* **`جدول توصيلات مشروع الجراج الذكي - Google Gemini.pdf`**: Legacy single-ESP32 wiring reference (see the updated Dual-ESP32 pinout tables below).

---

## 🔌 Updated Dual-ESP32 Hardware Pinout

### 🔗 Master ↔ Slave UART2 Interconnect (CRITICAL)
| Master ESP32 | Slave ESP32 | Note |
| :--- | :--- | :--- |
| **GPIO 17 (TX2)** | **GPIO 16 (RX2)** | Cross-connected TX → RX |
| **GPIO 16 (RX2)** | **GPIO 17 (TX2)** | Cross-connected RX ← TX |
| **GND** | **GND** | **Common Ground is mandatory** |

### 🟦 Board 1: Master ESP32
| GPIO | Function | Component | Notes |
| :--- | :--- | :--- | :--- |
| **GPIO 21 / 22** | I2C SDA / SCL | 20x4 I2C LCD | Address `0x27` |
| **GPIO 5 / 18 / 19 / 23** | VSPI SS / SCK / MISO / MOSI | RFID RC522 | Connect RC522 `RST` to `3.3V` |
| **GPIO 13** | PWM Output | Entry Gate Servo | 0° Closed, 90° Open |
| **GPIO 12** | PWM Output | Exit Gate Servo | ⚠️ Boot strapping pin (`MTDI`): ensure servo does not pull HIGH on boot |
| **GPIO 14** | Digital Input (`INPUT_PULLUP`) | Entry Intrusion IR | Active LOW |
| **GPIO 27** | Digital Input (`INPUT_PULLUP`) | Exit Gate IR | Active LOW |
| **GPIO 2** | Digital Output | Master Alarm Buzzer & Strobe LED | ⚠️ Must be LOW during flashing |

### 🟩 Board 2: Slave ESP32 (Ground & First Floor: 2-IR Sequence + Flame + DHT11 + MQ Smoke)
| GPIO | Function | Component | Notes |
| :--- | :--- | :--- | :--- |
| **GPIO 33 / 32** | Digital Input (`INPUT_PULLUP`) | Ground Floor `IR_A` (Outer) / `IR_B` (Inner) | Single lane, sequence-based (Max 10) |
| **GPIO 27 / 14** | Digital Input (`INPUT_PULLUP`) | First Floor `IR_A` (Outer) / `IR_B` (Inner) | Single lane, sequence-based (Max 10) |
| **GPIO 26 / 25** | Digital Input (`INPUT_PULLUP`) | Flame Sensor `DO` (Ground / First Floor) | Active LOW — instant fire alarm & fan trigger |
| **GPIO 34 / 35** | Analog Input (ADC1) | MQ Gas/Smoke (Ground / First Floor) | 8-sample averaged |
| **GPIO 18 / 19** | Digital Input | DHT11 Temp & Humidity (Ground / First Floor) | Moved from 16/17 to free UART2 |
| **GPIO 15 / 4** | Digital Output | Exhaust Fan Relay (Ground / First Floor) | Active HIGH |

---

## 📡 MQTT Topics (Node-RED Integration)

* **Publish (`garage/telemetry`)**: JSON payload every 2s containing `f1_cars`, `f2_cars`, `f2_temp`, `f2_smoke`, `f2_danger`, `f3_cars`, `f3_temp`, `f3_smoke`, `f3_danger`, `total_inside`, `total_entered`, `total_exited`, `gate_status`, `intrusion`, `master_alarm`, `slave_connected`.
* **Publish (`garage/gate/event`)**: JSON payload on vehicle `ENTER`, `EXIT`, or `INTRUSION`.
* **Publish (`garage/system/status`)**: Retained online status with local IP for Excel.
* **Subscribe (`garage/control/master_alarm`, `garage/control/f2_emergency`, `garage/control/f3_emergency`)**: Accepts `"1"` / `"0"` (or `"true"` / `"false"`).

---

## 🛠️ Tech Stack & Hardware

* **Microcontrollers:** 2x ESP32 DevKit V1
* **Firmware:** C / C++ (Arduino IDE, FreeRTOS Dual-Core)
* **Communication Protocols:** UART2 (115200 baud), Wi-Fi, MQTT, HTTP
* **IoT & Data Integration:** Node-RED Dashboard, Mosquitto/Aedes MQTT Broker, Microsoft Excel Web Query

---

## 👨‍💻 Author

* **Eng. Seif Hussein Alshabory** – *Mechatronics & Embedded Systems Engineer*
* **GitHub:** [@s-hussein10](https://github.com/s-hussein10)
