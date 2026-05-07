#include <Arduino.h>
#include <Wire.h>
#include <SoftwareSerial.h>
#include <SPI.h>
#include <MFRC522.h>
#include <MeMegaPi.h>   // includes MeRGBLed — no extra #include needed
#include <MePS2.h>
// Force IRremote onto Timer5 — Timer2 is used by MeMegaPi motor PWM (TCCR2A/2B
// in setup()) and would otherwise clobber IRremote's timer ISR after begin().
#define IR_USE_AVR_TIMER5
#include <IRremote.h>     // Library Manager: "IRremote" by shirriff / z3t0 / ArminJo
#include <ArduinoJson.h>  // Library Manager: "ArduinoJson" by Benoit Blanchon

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

int maxSpeed = 160;  // max PWM — overridable via RFID/command
int minSpeed = 20;   // minimum PWM when moving — overridable via RFID/command
const int DEADZONE = 20;

// ── IR transmitter ─────────────────────────────────────────────────────────────
// NEC protocol software carrier via busy-wait — no IRremote sender needed.
// addr = XOR-fold of TANK_ID bytes; cmd = ammoLevel (damage for receiver).
// IrReceiver.stop() / IrReceiver.start() wrap every transmission so the timer
// ISR does not capture the outgoing burst as an incoming signal.
const uint8_t IR_PIN = A12;  // = pin 66 on ATmega2560

// ── IR receivers (dual, time-multiplexed) ─────────────────────────────────────
// A11 = pin 65, A10 = pin 64.  IRremote uses a timer ISR to sample the active
// pin.  We swap every IR_SWITCH_MS ms so both receivers get coverage.
// A full NEC frame is ~68 ms; 50 ms gives reliable capture.
const uint8_t IR_RX_PIN_1 = A11;   // first receiver  (= pin 65)
const uint8_t IR_RX_PIN_2 = A10;   // second receiver (= pin 64)
const unsigned long IR_SWITCH_MS = 50;
uint8_t       activeRxPin  = IR_RX_PIN_1;
unsigned long lastRxSwitch = 0;

// ── Health LEDs (MeRGBLed / WS2812 2×2, pins A14 and A13) ───────────────────
// Both LEDs mirror the same colour.
// Boot:          yellow  (waiting for RPi)
// RPi connected: switches to health-based colour
// health > 50 → green | 5–50 → yellow | < 5 → red
MeRGBLed led1;  // A14
MeRGBLed led2;  // A13

bool rpiConnected = false;

// ── RFID reader ────────────────────────────────────────────────────────────────
const uint8_t RST_PIN = 30;
const uint8_t SS_PIN  = A6;  // = pin 60 on ATmega2560; not present in any MeMegaPi port table
MFRC522 rfid(SS_PIN, RST_PIN);  // Uses SPI: MOSI=51, MISO=50, SCK=52

const unsigned long RFID_COOLDOWN_MS = 5000;  // min ms between same-card events
String        lastRfidUid  = "";
unsigned long lastRfidTime = 0;

// ── Game state ─────────────────────────────────────────────────────────────────
int  health    = 100;
int  ammo      = 100;
int  ammoLevel = 1;    // 1–10
int  fireSpeed = 1;    // 1–10: minimum ms between shots
bool immunable = false;

// ── Death / respawn ────────────────────────────────────────────────────────────
bool isDead = false;

void respawn() {
  health    = 100;
  ammo      = 100;
  isDead    = false;
  updateHealthLed();
  Serial.println(F("[RESPAWN] Tank back in game!"));
}

// ── Speed tracking ─────────────────────────────────────────────────────────────
float speedFL = 0, speedFR = 0, speedRL = 0, speedRR = 0;

// ── Telemetry timing ───────────────────────────────────────────────────────────
const unsigned long TELEMETRY_INTERVAL_MS = 500;
unsigned long lastTelemetry = 0;

// ── RPi connectivity ping ──────────────────────────────────────────────────────
// Arduino sends a ping every PING_INTERVAL_MS; RPi replies with pong.
// On first pong, rpiConnected becomes true and LEDs switch to health mode.
const unsigned long PING_INTERVAL_MS = 5000;
unsigned long lastPing = 0;

// ── Fire control ───────────────────────────────────────────────────────────────
unsigned long lastFireTime   = 0;
bool          squarePrevious = false;

// ── LED blink state ────────────────────────────────────────────────────────────
// Non-blocking: driven by updateBlink() in loop().
// 2 blinks = 4 half-periods (on→off→on→off); after done, restores health colour.
const unsigned long BLINK_INTERVAL_MS = 100;
uint8_t       blinkSteps = 0;       // remaining half-periods; 0 = idle
uint8_t       blinkR, blinkG, blinkB;
unsigned long blinkLast  = 0;

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

// Enforce minSpeed floor: if the motor is asked to move at all,
// ensure its absolute PWM is at least minSpeed.
float applyMinSpd(float v) {
  if (v >  0.5f) return max(v, (float)minSpeed);
  if (v < -0.5f) return min(v, -(float)minSpeed);
  return 0;
}

void mecanumDrive(float lx, float ly, float rx) {
  float frontLeft  = ly +  lx +  rx;
  float frontRight = ly + -lx + -rx;
  float rearLeft   = ly + -lx +  rx;
  float rearRight  = ly +  lx + -rx;

  // Scale down to maxSpeed
  float maxVal = max(max(abs(frontLeft), abs(frontRight)),
                     max(abs(rearLeft),  abs(rearRight)));
  if (maxVal > maxSpeed) {
    float scale = (float)maxSpeed / maxVal;
    frontLeft  *= scale; frontRight *= scale;
    rearLeft   *= scale; rearRight  *= scale;
  }

  // Raise up to minSpeed (only for non-zero wheels)
  frontLeft  = applyMinSpd(frontLeft);
  frontRight = applyMinSpd(frontRight);
  rearLeft   = applyMinSpd(rearLeft);
  rearRight  = applyMinSpd(rearRight);

  portFL.run(SIGN_FL * frontLeft);
  portFR.run(SIGN_FR * frontRight);
  portRL.run(SIGN_RL * rearLeft);
  portRR.run(SIGN_RR * rearRight);

  speedFL = abs(frontLeft);
  speedFR = abs(frontRight);
  speedRL = abs(rearLeft);
  speedRR = abs(rearRight);
}

// ── IR blaster — NEC protocol ──────────────────────────────────────────────────
// Carrier ~38 kHz via software toggle (11 µs high + 11 µs low ≈ 45 kHz;
// most IR receivers accept 36–40 kHz, so this works in practice).

void markIR(uint16_t us) {
  unsigned long t = micros();
  while ((micros() - t) < us) {
    digitalWrite(IR_PIN, HIGH);
    delayMicroseconds(11);
    digitalWrite(IR_PIN, LOW);
    delayMicroseconds(11);
  }
}

void spaceIR(uint16_t us) {
  digitalWrite(IR_PIN, LOW);
  delayMicroseconds(us);
}

// Sends one 32-bit NEC frame: leader + 32 bits LSB-first + stop pulse.
// addr    — 8-bit sender address (XOR-fold of TANK_ID)
// cmd     — 8-bit command (ammoLevel here)
void sendNEC(uint8_t addr, uint8_t cmd) {
  uint32_t data = ((uint32_t)addr)              |
                  ((uint32_t)(addr ^ 0xFF) << 8) |
                  ((uint32_t)cmd           << 16)|
                  ((uint32_t)(cmd  ^ 0xFF) << 24);
  markIR(9000);   // 9 ms leader mark
  spaceIR(4500);  // 4.5 ms leader space
  for (int i = 0; i < 32; i++) {
    markIR(560);
    spaceIR(((data >> i) & 1) ? 1690 : 560);
  }
  markIR(560);    // stop bit
}

void fireIR() {
  // Derive a stable 8-bit address from TANK_ID (XOR of each ASCII byte).
  uint8_t addr = 0;
  for (uint8_t i = 0; TANK_ID[i]; i++) addr ^= (uint8_t)TANK_ID[i];

  // Stop IRremote's timer ISR so it doesn't capture our own outgoing burst
  IrReceiver.stop();
  sendNEC(addr, (uint8_t)ammoLevel);
  IrReceiver.start();   // resume on the currently active pin
}

// ── IR receivers — dual, time-multiplexed ─────────────────────────────────────
void switchIRReceiver() {
  unsigned long now = millis();
  if (now - lastRxSwitch < IR_SWITCH_MS) return;
  lastRxSwitch = now;

  activeRxPin = (activeRxPin == IR_RX_PIN_1) ? IR_RX_PIN_2 : IR_RX_PIN_1;
  IrReceiver.begin(activeRxPin, DISABLE_LED_FEEDBACK);
}

// Decode incoming NEC frame and apply damage.
// NEC packet: addr | (~addr)<<8 | cmd<<16 | (~cmd)<<24
// addr = attacker's XOR-folded TANK_ID  |  cmd = attacker's ammoLevel (damage).
// Validates both checksum pairs so random IR noise is rejected.
void handleIRReceive() {
  if (IrReceiver.decode()) {
    uint32_t raw = IrReceiver.decodedIRData.decodedRawData;

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

            uint8_t rxNum = (activeRxPin == IR_RX_PIN_1) ? 1 : 2;
            Serial.print(F("[HIT] IR")); Serial.print(rxNum);
            Serial.print(F(" from 0x")); Serial.print(addr, HEX);
            Serial.print(F(" -")); Serial.print(damage);
            Serial.print(F(" HP | Health: ")); Serial.println(health);

            sendHitEvent(addr, damage);   // notify server → server deducts HP
            startBlink(180, 0, 0);        // red blink on hit
            updateHealthLed();

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

// ── Ping (connectivity poll) ───────────────────────────────────────────────────
// Minimal fixed-size string — no ArduinoJson overhead needed.
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

// ── Telemetry ──────────────────────────────────────────────────────────────────
void sendTelemetry() {
  int avgSpeed = (int)((speedFL + speedFR + speedRL + speedRR) / 4.0f);

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

// Hit event: sent immediately when an incoming IR frame is decoded and damage is applied.
// receiverId  = this tank (who got hit)
// shooterAddr = 8-bit addr from the NEC frame (XOR-fold of attacker's TANK_ID)
// damage      = cmd byte from the NEC frame (attacker's ammoLevel)
// health      = remaining health after the hit
void sendHitEvent(uint8_t shooterAddr, int damage) {
  StaticJsonDocument<256> doc;
  doc["timestamp"] = millis() / 1000;
  JsonObject event = doc.createNestedObject("event");
  event["type"] = "hit";
  JsonObject data = event.createNestedObject("data");
  data["tankId"]      = TANK_ID;
  data["tankType"]    = TANK_TYPE;
  data["receiverId"]  = TANK_ID;
  data["shooterAddr"] = shooterAddr;
  data["damage"]      = damage;
  data["health"]      = health;

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

// ── LEDs ───────────────────────────────────────────────────────────────────────
void setAllLeds(uint8_t r, uint8_t g, uint8_t b) {
  led1.setColor(r, g, b); led1.show();
  led2.setColor(r, g, b); led2.show();
}

// Called on every health update and on RPi connect.
// No-op until rpiConnected — startup colour is handled in setup().
void updateHealthLed() {
  if (!rpiConnected) return;
  if      (health > 50) setAllLeds(0,   180,   0);  // green
  else if (health >= 5) setAllLeds(180, 140,   0);  // yellow
  else                  setAllLeds(180,   0,   0);  // red
}

// ── LED blink ──────────────────────────────────────────────────────────────────
// Kick off a 2-blink sequence in the chosen colour.
// updateBlink() must be called every loop() tick to advance the animation.
void startBlink(uint8_t r, uint8_t g, uint8_t b) {
  blinkR = r; blinkG = g; blinkB = b;
  blinkSteps = 4;   // 4 half-periods = 2 full blinks
  blinkLast  = millis() - BLINK_INTERVAL_MS;  // fire first step immediately
}

void updateBlink(unsigned long now) {
  if (blinkSteps == 0) return;
  if (now - blinkLast < BLINK_INTERVAL_MS) return;
  blinkLast = now;

  // Odd remaining steps → colour on; even → off
  if (blinkSteps % 2 == 0) setAllLeds(0, 0, 0);
  else                      setAllLeds(blinkR, blinkG, blinkB);

  blinkSteps--;
  if (blinkSteps == 0) updateHealthLed();  // restore correct health colour when done
}

// ── Serial handler (commands + system events from RPi) ────────────────────────
void handleSerialLine(const String& line) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err != DeserializationError::Ok) {
    Serial.print("[UART] JSON error: "); Serial.println(err.c_str());
    return;
  }

  JsonObject ev = doc["event"];
  if (!ev) return;
  const char* type = ev["type"] | "";

  // ── command ────────────────────────────────────────────────────────────────
  if (strcmp(type, "command") == 0) {
    const char* param = ev["param"] | "";
    int value = ev["value"] | 0;

    if      (strcmp(param, "health")    == 0) {
      int prev = health;
      health = constrain(value, 0, 100);
      if      (health < prev) startBlink(180, 0,   0);  // hit  → red blink
      else if (health > prev) startBlink(0,   180, 0);  // heal → green blink
      else                    updateHealthLed();          // no change, just refresh
    }
    else if (strcmp(param, "ammo")      == 0)   ammo      = constrain(value, 0, 100);
    else if (strcmp(param, "ammoLevel") == 0)   ammoLevel = constrain(value, 1, 10);
    else if (strcmp(param, "fireSpeed") == 0)   fireSpeed = constrain(value, 1, 10);
    else if (strcmp(param, "immunable") == 0)   immunable = (value != 0);
    else if (strcmp(param, "maxSpeed")  == 0)   maxSpeed  = constrain(value, 1, 255);
    else if (strcmp(param, "minSpeed")  == 0)   minSpeed  = constrain(value, 0, 255);

    Serial.print("[CMD] "); Serial.print(param);
    Serial.print(" = "); Serial.println(value);

  // ── system ────────────────────────────────────────────────────────────────
  } else if (strcmp(type, "system") == 0) {
    const char* action = ev["action"] | "";
    // "pong"      — RPi replied to our periodic ping (primary path).
    // "connected" — legacy one-shot sent when RPi MQTT connects.
    // "heartbeat" — fallback (only sent to MQTT, not serial, but kept for safety).
    if (!rpiConnected &&
        (strcmp(action, "pong") == 0 ||
         strcmp(action, "connected") == 0 ||
         strcmp(action, "heartbeat") == 0)) {
      rpiConnected = true;
      updateHealthLed();
      Serial.println("[SYS] RPi alive — LEDs → health mode");
    }
  }
}

// ── Setup ──────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);   // USB debug
  Serial2.begin(115200);  // Raspberry Pi via flex cable UART

  // IR transmitter
  pinMode(IR_PIN, OUTPUT);
  digitalWrite(IR_PIN, LOW);

  // IR receivers — start listening on pin 1
  IrReceiver.begin(IR_RX_PIN_1, DISABLE_LED_FEEDBACK);
  activeRxPin = IR_RX_PIN_1;
  Serial.print(F("IR RX: A11 (pin ")); Serial.print(IR_RX_PIN_1);
  Serial.print(F(") / A10 (pin "));   Serial.print(IR_RX_PIN_2);
  Serial.println(F(") — time-multiplexed"));
  Serial.print(F("IR TX: A12 (pin ")); Serial.print(IR_PIN);
  Serial.println(F(") — NEC software"));

  // Health LEDs — both yellow while waiting for RPi
  led1.setpin(A14); led1.setNumber(4);
  led2.setpin(A13); led2.setNumber(4);
  setAllLeds(180, 140, 0);  // yellow = not yet connected

  SPI.begin();
  rfid.PCD_Init();
  Serial.print("RFID ready. SS_PIN="); Serial.print(SS_PIN);
  Serial.print("  Tank: "); Serial.println(TANK_ID);

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

  // ── LED blink animation ────────────────────────────────────────────────────
  updateBlink(now);

  // ── IR receive ─────────────────────────────────────────────────────────────
  handleIRReceive();

  // ── Death state ────────────────────────────────────────────────────────────
  // Motors stop; only the START button is polled until the player respawns.
  if (isDead) {
    portFL.run(0); portFR.run(0);
    portRL.run(0); portRR.run(0);
    MePS2.loop();
    if (MePS2.ButtonPressed(MeJOYSTICK_START)) respawn();
    return;
  }

  // ── Receive from RPi (commands + system events) ───────────────────────────
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    Serial.write(c);  // DEBUG: echo every byte to USB serial so we can see what arrives
    if (c == '\n') {
      handleSerialLine(cmdBuffer);
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
      fireIR();             // send NEC frame: addr=TANK_ID hash, cmd=ammoLevel
      ammo--;
      lastFireTime = now;
      sendFireEvent();
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
