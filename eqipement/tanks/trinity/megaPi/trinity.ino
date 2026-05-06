#include <Arduino.h>
#include <Wire.h>
#include <SoftwareSerial.h>
#include <SPI.h>
#include <MFRC522.h>
#include <MeMegaPi.h>   // includes MeRGBLed — not used on Trinity
#include <MePS2.h>
#include <IRremote.h>   // Library Manager: "IRremote" by shirriff / z3t0 / ArminJo
#include <ArduinoJson.h>  // Library Manager: "ArduinoJson" by Benoit Blanchon

// ── Tank settings ──────────────────────────────────────────────────────────────
const char TANK_ID[]   = "TRNA1";   // Unique identifier, up to 6 characters
const char TANK_TYPE[] = "trinity";

// ── Motor port mapping (2-wheel differential drive) ───────────────────────────
// PORT_1 and PORT_4 are both PWM DC motor ports on the MegaPi.
// Flip SIGN_L or SIGN_R to +1/-1 if a motor runs the wrong direction.
MeMegaPiDCMotor portL(PORT_1);
MeMegaPiDCMotor portR(PORT_4);

const int SIGN_L = +1;
const int SIGN_R = -1;

MePS2 MePS2(PORT_15);

int maxSpeed = 160;  // max PWM — overridable via command
int minSpeed = 20;   // minimum PWM when moving — overridable via command
const int DEADZONE = 20;

// ── IR transmitter (NEC software, same as wheely) ─────────────────────────────
// Uses a 38 kHz software carrier via busy-wait — no IRremote sender needed.
// IrReceiver.stop() is called before TX and IrReceiver.start() after to avoid
// the timer ISR capturing its own outgoing burst as an incoming signal.
const uint8_t IR_TX_PIN = A12;  // = pin 66 on ATmega2560

// ── IR receivers (dual, time-multiplexed) ─────────────────────────────────────
// A14 = pin 68, A13 = pin 67.  IRremote uses a timer ISR to sample the active
// pin.  We swap the active pin every IR_SWITCH_MS milliseconds so both
// receivers get coverage.  A full NEC frame is ~68 ms; a 50 ms window gives
// reliable capture of frames that start within the listening window.
const uint8_t IR_RX_PIN_1 = A14;   // first receiver
const uint8_t IR_RX_PIN_2 = A13;   // second receiver
const unsigned long IR_SWITCH_MS = 50;
uint8_t       activeRxPin  = IR_RX_PIN_1;
unsigned long lastRxSwitch = 0;

// ── RFID reader ────────────────────────────────────────────────────────────────
// RST = 30 (same as wheely).  SS = 6 (digital pin 6 — absent from MeMegaPi
// port table, so it doesn't conflict with motor or sensor ports).
const uint8_t RST_PIN = 30;
const uint8_t SS_PIN  = 6;
MFRC522 rfid(SS_PIN, RST_PIN);   // SPI: MOSI=51, MISO=50, SCK=52

const unsigned long RFID_COOLDOWN_MS = 5000;
String        lastRfidUid  = "";
unsigned long lastRfidTime = 0;

// ── Game state ─────────────────────────────────────────────────────────────────
// Kept in the same shape as wheely so RPi/Serial2 commands are a drop-in.
int  health    = 100;
int  ammo      = 100;
int  ammoLevel = 3;    // 1–10: damage per shot (also sent in IR frame cmd byte)
int  fireSpeed = 5;    // 1–10: minimum ms between shots
bool immunable = false;

// ── Health-based speed scaling ─────────────────────────────────────────────────
// Max PWM scales linearly from 30 % at health=0 to 100 % at health=100.
// Keeps the tank mobile but penalises damage — more interesting than hard stop.
const float SPEED_MIN_MULT = 0.30f;
const float SPEED_MAX_MULT = 1.00f;
float currentMaxSpeed = maxSpeed;   // recalculated by updateSpeedFromHealth()

void updateSpeedFromHealth() {
  float pct  = constrain(health, 0, 100) / 100.0f;
  float mult = SPEED_MIN_MULT + pct * (SPEED_MAX_MULT - SPEED_MIN_MULT);
  currentMaxSpeed = maxSpeed * mult;
}

// ── Death / respawn ────────────────────────────────────────────────────────────
bool isDead = false;

void respawn() {
  health    = 100;
  ammo      = 100;
  isDead    = false;
  updateSpeedFromHealth();
  Serial.println(F("[RESPAWN] Tank back in game!"));
}

// ── Speed tracking ─────────────────────────────────────────────────────────────
float speedL = 0, speedR = 0;

// ── Fire control ───────────────────────────────────────────────────────────────
unsigned long lastFireTime   = 0;
bool          squarePrevious = false;

// ── Telemetry timing ───────────────────────────────────────────────────────────
const unsigned long TELEMETRY_INTERVAL_MS = 500;
unsigned long lastTelemetry = 0;

// ── RPi connectivity ping ──────────────────────────────────────────────────────
// Arduino sends a ping every PING_INTERVAL_MS; RPi replies with pong.
// On first pong/connected/heartbeat, rpiConnected becomes true.
const unsigned long PING_INTERVAL_MS = 5000;
unsigned long lastPing     = 0;
bool          rpiConnected = false;

// ── Command buffer (Serial2 ← RPi) ────────────────────────────────────────────
String cmdBuffer = "";

// ── Drive ──────────────────────────────────────────────────────────────────────
void stopAll() {
  portL.run(0); portR.run(0);
  speedL = speedR = 0;
}

float applyDeadzone(float v) {
  return (abs(v) < DEADZONE) ? 0.0f : v;
}

float applyMinSpd(float v) {
  if (v >  0.5f) return max(v, (float)minSpeed);
  if (v < -0.5f) return min(v, -(float)minSpeed);
  return 0.0f;
}

// Differential drive: forward = ±255, turn = ±255.
// Left stick Y  → forward/backward.
// Right stick X → turn.
void differentialDrive(float forward, float turn) {
  float left  = forward + turn;
  float right = forward - turn;

  // Scale down so neither wheel exceeds currentMaxSpeed
  float maxVal = max(abs(left), abs(right));
  if (maxVal > currentMaxSpeed) {
    float scale = currentMaxSpeed / maxVal;
    left  *= scale;
    right *= scale;
  }

  left  = applyMinSpd(left);
  right = applyMinSpd(right);

  portL.run(SIGN_L * left);
  portR.run(SIGN_R * right);

  speedL = abs(left);
  speedR = abs(right);
}

// ── IR transmitter — NEC software (identical to wheely) ───────────────────────
void markIR(uint16_t us) {
  unsigned long t = micros();
  while ((micros() - t) < us) {
    digitalWrite(IR_TX_PIN, HIGH);
    delayMicroseconds(11);
    digitalWrite(IR_TX_PIN, LOW);
    delayMicroseconds(11);
  }
}

void spaceIR(uint16_t us) {
  digitalWrite(IR_TX_PIN, LOW);
  delayMicroseconds(us);
}

// Sends a 32-bit NEC frame.
// addr = 8-bit sender address (XOR-fold of TANK_ID)
// cmd  = 8-bit command (ammoLevel — damage amount for the receiver)
void sendNEC(uint8_t addr, uint8_t cmd) {
  uint32_t data = ((uint32_t)addr)               |
                  ((uint32_t)(addr ^ 0xFF) << 8)  |
                  ((uint32_t)cmd           << 16) |
                  ((uint32_t)(cmd  ^ 0xFF) << 24);
  markIR(9000);    // 9 ms leader mark
  spaceIR(4500);   // 4.5 ms leader space
  for (int i = 0; i < 32; i++) {
    markIR(560);
    spaceIR(((data >> i) & 1) ? 1690 : 560);
  }
  markIR(560);     // stop bit
}

void fireIR() {
  uint8_t addr = 0;
  for (uint8_t i = 0; TANK_ID[i]; i++) addr ^= (uint8_t)TANK_ID[i];

  // Stop IRremote's timer ISR so it doesn't capture our own outgoing burst
  IrReceiver.stop();
  sendNEC(addr, (uint8_t)ammoLevel);
  IrReceiver.start();   // resume on the currently active pin
}

// ── IR receivers — dual, time-multiplexed ─────────────────────────────────────
// Switch the active receiver pin every IR_SWITCH_MS.
// IrReceiver.begin() reinitialises the timer ISR on the new pin.
void switchIRReceiver() {
  unsigned long now = millis();
  if (now - lastRxSwitch < IR_SWITCH_MS) return;
  lastRxSwitch = now;

  activeRxPin = (activeRxPin == IR_RX_PIN_1) ? IR_RX_PIN_2 : IR_RX_PIN_1;
  IrReceiver.begin(activeRxPin, DISABLE_LED_FEEDBACK);
}

// Decode incoming NEC frame and apply damage.
// NEC packet layout: addr | (~addr)<<8 | cmd<<16 | (~cmd)<<24
// cmd = attacker's ammoLevel = damage points.
// Validates checksum bytes so random IR noise is ignored.
void handleIRReceive() {
  if (IrReceiver.decode()) {
    uint32_t raw  = IrReceiver.decodedIRData.decodedRawData;

    // Reject repeat codes and zeroed frames
    if (raw != 0 && raw != 0xFFFFFFFF) {
      uint8_t addr  =  raw        & 0xFF;
      uint8_t addrN = (raw >>  8) & 0xFF;
      uint8_t cmd   = (raw >> 16) & 0xFF;
      uint8_t cmdN  = (raw >> 24) & 0xFF;

      // Valid NEC: addr XOR ~addr == 0xFF, same for cmd
      if ((uint8_t)(addr ^ addrN) == 0xFF && (uint8_t)(cmd ^ cmdN) == 0xFF) {
        int damage = (int)cmd;   // ammoLevel sent by the attacker

        if (damage > 0 && damage <= 10) {
          if (!immunable) {
            int prev = health;
            health = max(0, health - damage);
            updateSpeedFromHealth();

            uint8_t rxNum = (activeRxPin == IR_RX_PIN_1) ? 1 : 2;
            Serial.print(F("[HIT] IR")); Serial.print(rxNum);
            Serial.print(F(" -")); Serial.print(damage);
            Serial.print(F(" HP | Health: ")); Serial.println(health);

            if (health == 0 && prev > 0) {
              isDead = true;
              Serial.println(F("[DEAD] Press START to respawn"));
            }
          } else {
            Serial.println(F("[HIT] Immune — no damage taken"));
          }
        }
      }
    }

    IrReceiver.resume();
  }

  switchIRReceiver();
}

// ── RFID ───────────────────────────────────────────────────────────────────────
// Returns uppercase hex UID, or "" if no new card / same card within cooldown.
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

  if (uid == lastRfidUid && (now - lastRfidTime) < RFID_COOLDOWN_MS) return "";
  lastRfidUid  = uid;
  lastRfidTime = now;
  return uid;
}

// ── Serial2 — outgoing messages ───────────────────────────────────────────────

// Ping: minimal fixed-size string — no ArduinoJson overhead needed.
void sendPing() {
  Serial2.print(F("{\"event\":{\"type\":\"system\",\"action\":\"ping\"}}\r\n"));
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

void sendTelemetry() {
  int avgSpeed = (int)((speedL + speedR) / 2.0f);

  StaticJsonDocument<384> doc;
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
  data["maxSpeed"]  = maxSpeed;
  data["minSpeed"]  = minSpeed;

  serializeJson(doc, Serial2);
  Serial2.print("\r\n");
}

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

// ── Serial2 — incoming handler (commands + system events from RPi) ────────────
void handleSerialLine(const String& line) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err != DeserializationError::Ok) {
    Serial.print(F("[UART] JSON error: ")); Serial.println(err.c_str());
    return;
  }

  JsonObject ev = doc["event"];
  if (!ev) return;
  const char* type = ev["type"] | "";

  // ── command ──────────────────────────────────────────────────────────────────
  if (strcmp(type, "command") == 0) {
    const char* param = ev["param"] | "";
    int value = ev["value"] | 0;

    if (strcmp(param, "health") == 0) {
      health = constrain(value, 0, 100);
      updateSpeedFromHealth();   // Trinity: speed scales with health
    }
    else if (strcmp(param, "ammo")      == 0)  ammo      = constrain(value, 0, 100);
    else if (strcmp(param, "ammoLevel") == 0)  ammoLevel = constrain(value, 1, 10);
    else if (strcmp(param, "fireSpeed") == 0)  fireSpeed = constrain(value, 1, 10);
    else if (strcmp(param, "immunable") == 0)  immunable = (value != 0);
    else if (strcmp(param, "maxSpeed")  == 0) {
      maxSpeed = constrain(value, 1, 255);
      updateSpeedFromHealth();   // recalculate cap at new maxSpeed
    }
    else if (strcmp(param, "minSpeed")  == 0)  minSpeed  = constrain(value, 0, 255);

    Serial.print(F("[CMD] ")); Serial.print(param);
    Serial.print(F(" = "));   Serial.println(value);

  // ── system ───────────────────────────────────────────────────────────────────
  } else if (strcmp(type, "system") == 0) {
    const char* action = ev["action"] | "";
    // "pong"      — RPi replied to our periodic ping (primary path).
    // "connected" — legacy one-shot sent when RPi MQTT connects.
    // "heartbeat" — fallback (only sent to MQTT, not serial, but kept for safety).
    if (!rpiConnected &&
        (strcmp(action, "pong")      == 0 ||
         strcmp(action, "connected") == 0 ||
         strcmp(action, "heartbeat") == 0)) {
      rpiConnected = true;
      Serial.println(F("[SYS] RPi alive — telemetry active"));
    }
  }
}

// ── Setup ──────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);   // USB debug
  Serial2.begin(115200);  // Raspberry Pi via UART

  // IR transmitter
  pinMode(IR_TX_PIN, OUTPUT);
  digitalWrite(IR_TX_PIN, LOW);

  // IR receivers — start listening on pin 1
  IrReceiver.begin(IR_RX_PIN_1, DISABLE_LED_FEEDBACK);
  activeRxPin = IR_RX_PIN_1;
  Serial.print(F("IR RX: A14 (pin ")); Serial.print(IR_RX_PIN_1);
  Serial.print(F(") / A13 (pin "));   Serial.print(IR_RX_PIN_2);
  Serial.println(F(") — time-multiplexed"));
  Serial.print(F("IR TX: A12 (pin ")); Serial.print(IR_TX_PIN);
  Serial.println(F(") — NEC software"));

  // RFID
  SPI.begin();
  rfid.PCD_Init();
  Serial.print(F("RFID ready. SS_PIN=")); Serial.print(SS_PIN);
  Serial.print(F("  Tank: "));            Serial.println(TANK_ID);

  // MegaPi PWM timers (same prescaler as wheely)
  TCCR1A = _BV(WGM10);
  TCCR1B = _BV(CS11) | _BV(WGM12);
  TCCR2A = _BV(WGM21) | _BV(WGM20);
  TCCR2B = _BV(CS21);

  MePS2.begin(115200);
  delay(1000);
  stopAll();

  updateSpeedFromHealth();

  Serial.println(F("================================="));
  Serial.println(F("  Trinity ready — RPi mode       "));
  Serial.println(F("  Serial2 @ 115200 → Raspberry Pi"));
  Serial.println(F("  Square → fire IR               "));
  Serial.println(F("  START  → respawn               "));
  Serial.println(F("================================="));
}

// ── Loop ───────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── Receive from RPi (commands + system events) ───────────────────────────
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    Serial.write(c);  // DEBUG: echo to USB so we can monitor incoming traffic
    if (c == '\n') {
      handleSerialLine(cmdBuffer);
      cmdBuffer = "";
    } else if (c != '\r') {
      cmdBuffer += c;
    }
  }

  // ── Death state ────────────────────────────────────────────────────────────
  // Motors stop; only the START button is polled until the player respawns.
  if (isDead) {
    stopAll();
    MePS2.loop();
    if (MePS2.ButtonPressed(MeJOYSTICK_START)) respawn();
    return;
  }

  // ── IR receive ─────────────────────────────────────────────────────────────
  handleIRReceive();

  // ── RFID scan ──────────────────────────────────────────────────────────────
  String uid = readRfidUid(now);
  if (uid.length() > 0) {
    Serial.print(F("[RFID] UID: ")); Serial.println(uid);
    sendRfidEvent(uid);
  }

  // ── Read controller ────────────────────────────────────────────────────────
  MePS2.loop();

  float ly = applyDeadzone(MePS2.MeAnalog(MeJOYSTICK_LY));
  float rx = applyDeadzone(MePS2.MeAnalog(MeJOYSTICK_RX));

  // LY = forward / backward  |  RX = turn
  differentialDrive(-ly, rx);

  // ── Square button → fire ───────────────────────────────────────────────────
  bool squareNow = MePS2.ButtonPressed(MeJOYSTICK_SQUARE);
  if (squareNow && !squarePrevious) {
    if (ammo > 0 && (now - lastFireTime >= (unsigned long)fireSpeed)) {
      fireIR();
      ammo--;
      lastFireTime = now;
      sendFireEvent();
      Serial.print(F("[FIRE] ammoLevel=")); Serial.print(ammoLevel);
      Serial.print(F(" | Ammo left: "));    Serial.println(ammo);
    } else if (ammo == 0) {
      Serial.println(F("[FIRE] Out of ammo!"));
    }
  }
  squarePrevious = squareNow;

  // ── Periodic RPi ping ──────────────────────────────────────────────────────
  if (now - lastPing >= PING_INTERVAL_MS) {
    sendPing();
    lastPing = now;
  }

  // ── Periodic telemetry ─────────────────────────────────────────────────────
  if (now - lastTelemetry >= TELEMETRY_INTERVAL_MS) {
    sendTelemetry();
    lastTelemetry = now;
  }
}
