#include <Arduino.h>
#include <Wire.h>
#include <SoftwareSerial.h>
#include <SPI.h>
#include <MFRC522.h>
#include <MeMegaPi.h>
#include <MePS2.h>
#include <ArduinoJson.h>  // Install via Library Manager: "ArduinoJson" by Benoit Blanchon

// ── Tank settings ──────────────────────────────────────────────────────────────
const char TANK_ID[]   = "WHLYA1";   // Unique identifier, up to 6 characters
const char TANK_TYPE[] = "wheely";

// ── Motor port mapping ─────────────────────────────────────────────────────────
// PORT_12 = Front-Left  | PORT_4  = Front-Right
// PORT_9  = Rear-Left   | PORT_1  = Rear-Right
MeMegaPiDCMotor portFL(PORT_12);
MeMegaPiDCMotor portFR(PORT_4);
MeMegaPiDCMotor portRL(PORT_9);
MeMegaPiDCMotor portRR(PORT_1);

MePS2 MePS2(PORT_15);

// Calibration per motor: +1 normal, -1 reversed
const int SIGN_FL = +1;
const int SIGN_FR = -1;
const int SIGN_RL = -1;
const int SIGN_RR = +1;

const int MAX_SPEED = 255;
const int DEADZONE  = 20;

// ── IR blaster ─────────────────────────────────────────────────────────────────
const int IR_TX_PIN = 3;  // ← Set to your IR LED data pin

// ── RFID reader ────────────────────────────────────────────────────────────────
const uint8_t RST_PIN = 30;
const uint8_t SS_PIN  = 22;
MFRC522 rfid(SS_PIN, RST_PIN);  // Uses SPI: MOSI=51, MISO=50, SCK=52

const unsigned long RFID_COOLDOWN_MS = 3000;  // min ms between same-card events
String        lastRfidUid  = "";
unsigned long lastRfidTime = 0;

// ── Game state ─────────────────────────────────────────────────────────────────
int  health    = 100;
int  ammo      = 100;
int  ammoLevel = 3;    // 1–10
int  fireSpeed = 5;    // 1–10: minimum ms between shots
bool immunable = false;

// ── Speed tracking ─────────────────────────────────────────────────────────────
float speedFL = 0, speedFR = 0, speedRL = 0, speedRR = 0;

// ── Telemetry timing ───────────────────────────────────────────────────────────
const unsigned long TELEMETRY_INTERVAL_MS = 500;
unsigned long lastTelemetry = 0;

// ── Fire control ───────────────────────────────────────────────────────────────
unsigned long lastFireTime   = 0;
bool          squarePrevious = false;

// ── Command buffer (Serial2 ← RPi) ────────────────────────────────────────────
String cmdBuffer = "";

// ── Drive ──────────────────────────────────────────────────────────────────────
void stopAll() {
  portFL.run(0); portFR.run(0);
  portRL.run(0); portRR.run(0);
}

float applyDeadzone(float v) {
  return (abs(v) < DEADZONE) ? 0 : v;
}

void mecanumDrive(float lx, float ly, float rx) {
  float frontLeft  = ly +  lx +  rx;
  float frontRight = ly + -lx + -rx;
  float rearLeft   = ly + -lx +  rx;
  float rearRight  = ly +  lx + -rx;

  float maxVal = max(max(abs(frontLeft), abs(frontRight)),
                     max(abs(rearLeft),  abs(rearRight)));
  if (maxVal > MAX_SPEED) {
    float scale = MAX_SPEED / maxVal;
    frontLeft  *= scale; frontRight *= scale;
    rearLeft   *= scale; rearRight  *= scale;
  }

  portFL.run(SIGN_FL * frontLeft);
  portFR.run(SIGN_FR * frontRight);
  portRL.run(SIGN_RL * rearLeft);
  portRR.run(SIGN_RR * rearRight);

  speedFL = abs(frontLeft);
  speedFR = abs(frontRight);
  speedRL = abs(rearLeft);
  speedRR = abs(rearRight);
}

// ── IR blaster ─────────────────────────────────────────────────────────────────
void fireIR() {
  // TODO: replace with actual IR library call, e.g.:
  //   uint32_t payload = encodeShot(TANK_ID, ammoLevel);
  //   irsend.sendNEC(payload, 32);
  // Placeholder: single pulse on IR_TX_PIN
  digitalWrite(IR_TX_PIN, HIGH);
  delayMicroseconds(600);
  digitalWrite(IR_TX_PIN, LOW);
}

// ── RFID ───────────────────────────────────────────────────────────────────────
// Returns uppercase hex UID string, or "" if no new card / same card too soon.
String readRfidUid(unsigned long now) {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return "";

  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  // Debounce: skip same card within cooldown window
  if (uid == lastRfidUid && (now - lastRfidTime) < RFID_COOLDOWN_MS) return "";

  lastRfidUid  = uid;
  lastRfidTime = now;
  return uid;
}

void sendRfidEvent(const String& uid) {
  StaticJsonDocument<200> doc;
  doc["timestamp"] = millis() / 1000;
  JsonObject event = doc.createNestedObject("event");
  event["type"] = "rfid";
  JsonObject data = event.createNestedObject("data");
  data["tankId"]   = TANK_ID;
  data["tankType"] = TANK_TYPE;
  data["uid"]      = uid;

  serializeJson(doc, Serial2);
  Serial2.print("\r\n");
}

// ── Telemetry ──────────────────────────────────────────────────────────────────
void sendTelemetry() {
  int avgSpeed = (int)((speedFL + speedFR + speedRL + speedRR) / 4.0f);

  StaticJsonDocument<300> doc;
  doc["timestamp"] = millis() / 1000;
  JsonObject event = doc.createNestedObject("event");
  event["type"] = "telemetry";
  JsonObject data = event.createNestedObject("data");
  data["tankId"]    = TANK_ID;
  data["tankType"]  = TANK_TYPE;
  data["speed"]     = avgSpeed;
  data["health"]    = health;
  data["ammo"]      = ammo;
  data["ammoLevel"] = ammoLevel;
  data["fireSpeed"] = fireSpeed;
  data["immunable"] = immunable;

  serializeJson(doc, Serial2);
  Serial2.print("\r\n");
}

// Fire event: sent immediately on each shot
void sendFireEvent() {
  StaticJsonDocument<200> doc;
  doc["timestamp"] = millis() / 1000;
  JsonObject event = doc.createNestedObject("event");
  event["type"] = "fire";
  JsonObject data = event.createNestedObject("data");
  data["tankId"]    = TANK_ID;
  data["tankType"]  = TANK_TYPE;
  data["senderId"]  = TANK_ID;
  data["ammoLevel"] = ammoLevel;
  data["ammo"]      = ammo;

  serializeJson(doc, Serial2);
  Serial2.print("\r\n");
}

// ── Command handler ────────────────────────────────────────────────────────────
void handleCommand(const String& line) {
  // Buffer must hold the raw JSON copy (~79 bytes) + node overhead (~80 bytes).
  // 128 is too small → DeserializationError::NoMemory → commands silently ignored.
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err != DeserializationError::Ok) {
    Serial.print("[CMD] JSON error: "); Serial.println(err.c_str());
    return;
  }

  JsonObject ev = doc["event"];
  if (!ev || strcmp(ev["type"] | "", "command") != 0) return;

  const char* param = ev["param"] | "";
  int value = ev["value"] | 0;

  if      (strcmp(param, "health")    == 0) health    = constrain(value, 0, 100);
  else if (strcmp(param, "ammo")      == 0) ammo      = constrain(value, 0, 100);
  else if (strcmp(param, "ammoLevel") == 0) ammoLevel = constrain(value, 1, 10);
  else if (strcmp(param, "fireSpeed") == 0) fireSpeed = constrain(value, 1, 10);
  else if (strcmp(param, "immunable") == 0) immunable = (value != 0);

  Serial.print("[CMD] "); Serial.print(param);
  Serial.print(" = "); Serial.println(value);
}

// ── Setup ──────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);   // USB debug
  Serial2.begin(115200);  // Raspberry Pi via flex cable UART
  pinMode(IR_TX_PIN, OUTPUT);
  digitalWrite(IR_TX_PIN, LOW);

  SPI.begin();
  rfid.PCD_Init();
  Serial.print("RFID reader ready. Tank: "); Serial.println(TANK_ID);

  TCCR1A = _BV(WGM10);
  TCCR1B = _BV(CS11) | _BV(WGM12);
  TCCR2A = _BV(WGM21) | _BV(WGM20);
  TCCR2B = _BV(CS21);

  MePS2.begin(115200);
  delay(1000);
  stopAll();
}

// ── Loop ───────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── Receive commands from RPi ──────────────────────────────────────────────
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\n') {
      handleCommand(cmdBuffer);
      cmdBuffer = "";
    } else if (c != '\r') {
      cmdBuffer += c;
    }
  }

  // ── RFID scan ──────────────────────────────────────────────────────────────
  String uid = readRfidUid(now);
  if (uid.length() > 0) {
    Serial.print("RFID: "); Serial.println(uid);
    sendRfidEvent(uid);
  }

  // ── Read controller ────────────────────────────────────────────────────────
  MePS2.loop();
  float lx = applyDeadzone(MePS2.MeAnalog(MeJOYSTICK_LX));
  float ly = applyDeadzone(MePS2.MeAnalog(MeJOYSTICK_LY));
  float rx = applyDeadzone(MePS2.MeAnalog(MeJOYSTICK_RX));
  mecanumDrive(rx, -lx, ly);

  // ── Square button → fire ───────────────────────────────────────────────────
  bool squareNow = MePS2.ButtonPressed(MeJOYSTICK_SQUARE);
  if (squareNow && !squarePrevious) {
    if (ammo > 0 && (now - lastFireTime >= (unsigned long)fireSpeed)) {
      fireIR();
      ammo--;
      lastFireTime = now;
      sendFireEvent();
    }
  }
  squarePrevious = squareNow;

  // ── Periodic telemetry ─────────────────────────────────────────────────────
  if (now - lastTelemetry >= TELEMETRY_INTERVAL_MS) {
    sendTelemetry();
    lastTelemetry = now;
  }
}
