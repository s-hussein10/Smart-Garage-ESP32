# 🚗 3-Level Smart Industrial Parking Garage (IoT & Dual-ESP32)

An industrial 3-level smart parking garage system built on a **Dual-ESP32 (Master/Slave)** architecture for optimal load and GPIO pin balancing. The system features real-time monitoring and control via a **Node-RED** dashboard, alongside automated **Excel** data logging for multi-facility industrial synchronization.

---

## 🌟 Key Features

* **Dual-ESP32 Architecture:** Splits hardware tasks between a Master ESP32 and a Slave ESP32 communicating seamlessly to handle all sensors and actuators efficiently without exhausting GPIO pins on a single board.
* **Automated Access & Barrier Control:** Equipped with **RFID** access control, **Servo-driven barriers**, and **Photoelectric / IR sensors** across all three parking levels to track vehicle entry, exit, and slot occupancy.
* **Safety & Environmental Control:** Automated control of **exhaust ventilation fans** and real-time status monitoring across all garage floors.
* **IoT Dashboard & Data Logging:** Direct integration with a **Node-RED** dashboard for live supervision and control, coupled with automated local **Excel** data export to synchronize state data across connected industrial facilities.

---

## 📂 Repository Structure

* **`SMAERT_GARAGE_ESP32_MASTER/`**: Source code for the Master ESP32 microcontroller responsible for Wi-Fi connectivity, Node-RED/Excel data synchronization, primary sensors, and communication with the Slave unit[cite: 1, 3].
* **`SMAERT_GARAGE_ESP32_SLAVE/`**: Source code for the Slave ESP32 microcontroller managing secondary floor sensors and actuators[cite: 1, 3].
* **`SMART_GARAGE_Edition1/`**: Initial standalone version of the Smart Garage firmware[cite: 1, 3].
* **`جدول توصيلات مشروع الجراج الذكي - Google Gemini.pdf`**: Comprehensive hardware pinout and wiring guide for both ESP32 boards, sensors, and actuators[cite: 1, 3].

---

## 🛠️ Tech Stack & Hardware

* **Microcontrollers:** 2x ESP32 DevKit V1
* **Firmware:** C / C++ (Arduino IDE)
* **Communication Protocols:** UART / ESP-NOW & Wi-Fi
* **IoT & Data Integration:** Node-RED Dashboard, MQTT, Automated Excel Logging
* **Hardware Components:** RFID Reader, Photoelectric / IR Sensors, Servo Motors, Exhaust Ventilation Fans, Indicators

---

## 👨‍💻 Author

* **Eng. Seif Hussein Alshabory** – *Mechatronics & Embedded Systems Engineer*
* **GitHub:** [@s-hussein10](https://github.com/s-hussein10)
