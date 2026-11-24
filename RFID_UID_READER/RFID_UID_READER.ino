/*
 * ==============================================================================
 *  STANDALONE RFID UID READER (ESP32 + RC522 / FM17522)
 *  Same wiring as Smart Garage Master:
 *    SDA (SS) -> GPIO 5  | SCK  -> GPIO 18 | MISO -> GPIO 19 | MOSI -> GPIO 23
 *    RST      -> 3.3V    | 3.3V -> 3.3V    | GND  -> GND
 * ==============================================================================
 */

#include <SPI.h>
#include <MFRC522.h>

#define SS_PIN  5
#define RST_PIN -1  // Connect RC522 RST pin to 3.3V (or change to GPIO 4 if wired there)

MFRC522 rfid(SS_PIN, RST_PIN);                  

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n==========================================");
  Serial.println("      ESP32 RFID UID READER (RC522)       ");
  Serial.println("==========================================");

  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();
  delay(50);
  // Force differential RF drive (InvTx2RFOn=1 + Tx2RFEn=1 + Tx1RFEn=1 = 0x83) for 0x82 clone chips!
  rfid.PCD_WriteRegister(rfid.TxControlReg, 0x83);
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);

  byte ver = rfid.PCD_ReadRegister(rfid.VersionReg);
  byte ant = rfid.PCD_ReadRegister(rfid.TxControlReg);

  Serial.printf("RC522 Firmware Version : 0x%02X\n", ver);
  Serial.printf("Antenna Register       : 0x%02X (%s)\n", ant, (ant & 0x03) == 0x03 ? "ON" : "OFF");

  if (ver == 0x00 || ver == 0xFF) {
    Serial.println(">>> ERROR: RC522 NOT FOUND! Check 3.3V, GND, RST->3.3V & SPI wires.");
  } else {
    Serial.println(">>> READY! Tap your RFID card or keychain tag now...");
  }
  Serial.println("==========================================\n");
}

void loop() {
  if (!rfid.PICC_IsNewCardPresent()) {
    delay(100);
    return;
  }

  if (!rfid.PICC_ReadCardSerial()) {
    delay(100);
    return;
  }

  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  Serial.println("------------------------------------------");
  Serial.println("CARD DETECTED!");
  Serial.println("UID (Copy this) : " + uid);
  Serial.println("------------------------------------------");

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(1000);
}
