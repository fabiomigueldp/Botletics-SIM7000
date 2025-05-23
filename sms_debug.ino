/*
 * SIM7000 SMS/GPS Controller
 * Hardware: Arduino UNO R3 + Botletics SIM7000 Shield + Hologram SIM
 *
 * Commands (case-insensitive):
 *   ON   or PWRON  –> PWR_PIN LOW  (0 V, relay energised, lamp ON)
 *   OFF  or PWROFF –> PWR_PIN HIGH (+5 V, relay released, lamp OFF)
 *   STATE          –> Returns "STATE: PWR LO (ON)" or "STATE: PWR HI (OFF)"
 *   GPS            –> One-line GPS fix + Google-Maps link
 *
 * Design notes:
 *   • Board never sleeps – always listening for SMS.
 *   • Inbox auto-cleans after each message.
 *   • SoftwareSerial dropped to 9600 baud for UNO reliability.
 */

#include <SoftwareSerial.h>
#include <ctype.h>
#include "BotleticsSIM7000.h"
#include <stdio.h>
#include <stdlib.h> // For dtostrf

// ─── Pin definitions ───────────────────────────────────────────
#define MODEM_RX      10
#define MODEM_TX      11
#define MODEM_RST      7
#define MODEM_PWRKEY   6
#define PWR_PIN       12          // Relay / lamp control (active-low)

// ─── Globals ───────────────────────────────────────────────────
SoftwareSerial modemSS(MODEM_RX, MODEM_TX);
Botletics_modem_LTE fona = Botletics_modem_LTE();
char smsBuffer[256];

// Forward declarations
void handleCommand(const char *sender, char *msg);
void sendSMS(const char *recipient, const __FlashStringHelper *text);
void sendSMS(const char *recipient, const char *text);
void sendGPS(const char *sender);

// ─── Setup ─────────────────────────────────────────────────────
void setup() {
  pinMode(PWR_PIN, OUTPUT);
  digitalWrite(PWR_PIN, HIGH);        // Start OFF (active-low relay)

  pinMode(MODEM_RST, OUTPUT);
  digitalWrite(MODEM_RST, HIGH);

  Serial.begin(115200);
  Serial.println(F("SIM7000 SMS/GPS Controller starting…"));

  modemSS.begin(115200);

  // Power-up modem
  Serial.println(F("Powering on SIM7000"));
  fona.powerOn(MODEM_PWRKEY);
  delay(3000); 

  // Drop to 9600 so SoftwareSerial is rock-solid
  modemSS.println("AT+IPR=9600");
  delay(200);
  modemSS.end();
  modemSS.begin(9600);

  Serial.println(F("Initializing modem..."));
  if (!fona.begin(modemSS)) {
    Serial.println(F("ERROR: cannot communicate with SIM7000 – check wiring/power."));
    while (1) { delay(1); }
  }
  Serial.println(F("Modem initialized."));

  fona.setNetworkSettings(F("hologram"));   // APN
  fona.setFunctionality(1);                 // Full-functionality mode

  // Wait up to 120 s for network registration
  Serial.print(F("Registering on network"));
  unsigned long t0 = millis();
  while (fona.getNetworkStatus() != 1 && fona.getNetworkStatus() != 5) {
    Serial.print('.');
    if (millis() - t0 > 120000UL) {
      Serial.println(F("\nTimed-out – check SIM/antenna"));
      break;
    }
    delay(1000);
  }
  Serial.println();
  if (fona.getNetworkStatus() == 1 || fona.getNetworkStatus() == 5) {
    Serial.println(F("Registered on network."));
  } else {
    Serial.println(F("Failed to register on network."));
  }

  fona.enableGPS(true);   // Keep GPS powered (no sleep)
  Serial.println(F("GPS enabled."));

  fona.deleteAllSMS();    // Clean inbox
  Serial.println(F("Setup complete – waiting for SMS commands"));
}

// ─── Main loop ────────────────────────────────────────────────
void loop() {
  int8_t smsCount = fona.getNumSMS();
  if (smsCount == -1) {
    Serial.println(F("Error checking SMS count."));
  } else if (smsCount > 0) {
    Serial.print(F("SMS count: "));
    Serial.println(smsCount);
    for (int8_t i = 0; i < smsCount; ++i) {
      uint16_t smslen;
      char sender[32] = "";

      if (!fona.getSMSSender(i, sender, sizeof(sender) - 1)) {
        Serial.print(F("Failed to get sender for SMS index "));
        Serial.println(i);
        continue;
      }
      if (!fona.readSMS(i, smsBuffer, sizeof(smsBuffer) - 1, &smslen)) {
        Serial.print(F("Failed to read SMS index "));
        Serial.println(i);
        continue;
      }

      smsBuffer[smslen] = '\0'; // Null-terminate the message
      Serial.print(F("From: ")); Serial.println(sender);
      Serial.print(F("Message: ")); Serial.println(smsBuffer);

      handleCommand(sender, smsBuffer);
      if (fona.deleteSMS(i)) {                 // Always clear slot
        Serial.print(F("Deleted SMS index "));
        Serial.println(i);
      } else {
        Serial.print(F("Failed to delete SMS index "));
        Serial.println(i);
      }
      delay(1000);
    }
  }
  delay(5000);                           // Poll every 5 s
}

// ─── Command handler ──────────────────────────────────────────
void handleCommand(const char *sender, char *msg) {
  if (!sender || !*sender || !msg || !*msg) return;

  Serial.print(F("Handling command from "));
  Serial.print(sender);
  Serial.print(F(": "));
  Serial.println(msg);

  for (char *p = msg; *p; ++p) *p = toupper((unsigned char)*p);

  bool acted = false;

  if (strstr(msg, "PWRON") || strstr(msg, "ON")) {
    digitalWrite(PWR_PIN, LOW);
    sendSMS(sender, F("PWR set to ON (0 V – relay energised)"));
    acted = true;
  }
  else if (strstr(msg, "PWROFF") || strstr(msg, "OFF")) {
    digitalWrite(PWR_PIN, HIGH);
    sendSMS(sender, F("PWR set to OFF (+5 V – relay released)"));
    acted = true;
  }
  else if (strstr(msg, "STATE")) {
    sendSMS(sender,
            digitalRead(PWR_PIN) == LOW ?
            F("STATE: PWR LO (ON)") :
            F("STATE: PWR HI (OFF)"));
    acted = true;
  }
  else if (strstr(msg, "GPS")) {
    sendGPS(sender);
    acted = true;
  }

  if (!acted) {
    sendSMS(sender,
      F("Unknown command. Use ON/PWRON, OFF/PWROFF, STATE, or GPS."));
  }
}

// ─── GPS routine ──────────────────────────────────────────────
void sendGPS(const char *sender) {
  float lat=0.0, lon=0.0, speed_kph=0.0, heading=0.0, alt=0.0;
  const unsigned long timeout = 30000UL;
  unsigned long t0 = millis();

  Serial.println(F("Waiting for GPS fix (30 s)…"));
  bool fixObtained = false;
  while (millis() - t0 < timeout) {
    if (fona.getGPS(&lat, &lon, &speed_kph, &heading, &alt)) {
        if (lat != 0.0 || lon != 0.0) {
            fixObtained = true;
            break;
        }
    }
    Serial.print(".");
    delay(2000); 
  }
  Serial.println();

  if (fixObtained) {
      char msg[160];
      char latS[12], lonS[12], spdS[8], hdgS[8], altS[8];

      // dtostrf(floatVal, minStringWidthIncDecimalPoint, numVarsAfterDecimal, charBuf);
      dtostrf(lat, 9, 6, latS); // e.g., -12.345678 (9 chars total, 6 after decimal)
      dtostrf(lon, 10, 6, lonS); // e.g., -123.456789 (10 chars total, 6 after decimal)
      dtostrf(speed_kph, 4, 1, spdS);
      dtostrf(heading, 3, 0, hdgS);
      dtostrf(alt, 5, 1, altS);

      snprintf(msg, sizeof(msg),
               "Lat:%s Lon:%s Spd:%skm/h Dir:%sdeg Alt:%sm https://maps.google.com/?q=%s,%s",
               latS, lonS, spdS, hdgS, altS, latS, lonS);

      sendSMS(sender, msg);
      Serial.print(F("GPS data sent: "));
      Serial.println(msg);
      return;
  }
  
  sendSMS(sender, F("GPS fix not available – try again later"));
  Serial.println(F("GPS fix not available."));
}

// ─── SMS helpers ──────────────────────────────────────────────
void sendSMS(const char *recipient, const __FlashStringHelper *text) {
  if (recipient && *recipient && text) { 
    String textStr = String(text); 
    Serial.print(F("Sending SMS to "));
    Serial.print(recipient);
    Serial.print(F(": "));
    Serial.println(textStr);
    if (!fona.sendSMS(recipient, textStr.c_str())) { 
        Serial.println(F("Failed to send SMS."));
    } else {
        Serial.println(F("SMS sent successfully."));
    }
  }
}

void sendSMS(const char *recipient, const char *text) {
  if (recipient && *recipient && text && *text) { 
    Serial.print(F("Sending SMS to "));
    Serial.print(recipient);
    Serial.print(F(": "));
    Serial.println(text);
    if(!fona.sendSMS(recipient, text)) {
        Serial.println(F("Failed to send SMS."));
    } else {
        Serial.println(F("SMS sent successfully."));
    }
  }
}
