#include <Arduino.h>
#include <SoftwareSerial.h>  // required so the IDE links SoftwareSerial (used internally by MePS2)
#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <MeMegaPi.h>   // includes MeRGBLed and MeEncoderOnBoard
#include <MePS2.h>
// Force IRremote onto Timer4 (16-bit, pins 6/7/8).
// Timer1 & Timer2 are claimed by MeMegaPi motor PWM (TCCR1/TCCR2 in setup()).
// Timer5 is claimed by the Servo library bundled with MeMegaPi → __vector_47 clash.
// Timer4 is free on MegaPi with this motor port configuration.
// #define IR_USE_AVR_TIMER3 // Commented out: causes one motor to stop working
#include <IRremote.h>     // Library Manager: "IRremote" by shirriff / z3t0 / ArminJo
#include <ArduinoJson.h>  // Library Manager: "ArduinoJson" by Benoit Blanchon

// ── Tank settings ──────────────────────────────────────────────────────────────
const char TANK_ID[]   = "ROCKY1";   // Unique identifier, up to 6 characters
const char TANK_TYPE[] = "rocky";

// ── Motor slot mapping ─────────────────────────────────────────────────────────
// 3 encoder motors on MegaPi onboard encoder slots:
//   SLOT1 = LEFT track   (joystick: left  stick Y)   pin 11  Timer1
//   SLOT4 = RIGHT track  (joystick: right stick Y)   pin  5  Timer3
//   SLOT2 = AUX motor    (D-pad: UP=forward, DOWN)   pin  9  Timer2
//
// SLOT3 (pin 6, Timer4) is avoided: IRremote defaults to Timer4 on ATmega2560
// and hijacks pin 6, making any analogWrite to SLOT3 produce no output.
// SLOT2 (Timer2) is free — TCCR2 below already configures it for fast PWM.
// Note: SLOT2 encoder port-B shares A12 with IR_TX_PIN. Since we use direct
// setMotorPwm() (not encoder PID), the pulse count is never read, so the
// A12 overlap during IR firing is harmless.
MeEncoderOnBoard motorL(SLOT1);
MeEncoderOnBoard motorR(SLOT4);
MeEncoderOnBoard motorAux(SLOT2);

MePS2 MePS2(PORT_15);

// Calibration per motor: +1 normal, -1 reversed.
// Flip these if a stick/button moves a motor the wrong way.
const int SIGN_L   = +1;
const int SIGN_R   = +1;
const int SIGN_AUX = 1;

// Track speed (joystick driven)
int maxSpeed = 160;  // max PWM — overridable via RFID/command
int minSpeed = 20;   // minimum PWM when moving — overridable via RFID/command
const int DEADZONE = 20;

// Aux motor speed (D-pad driven) — fixed, not affected by maxSpeed.
// Bump this up if the aux motor doesn't move; raise toward 80–127 for heavy loads.
const int AUX_SPEED = 50;

// ── IR transmitter ─────────────────────────────────────────────────────────────
const uint8_t IR_PIN = A12;

// ── IR receivers (dual, time-multiplexed) ─────────────────────────────────────
const uint8_t IR_RX_PIN_1 = A11;
const uint8_t IR_RX_PIN_2 = A10;
const unsigned long IR_SWITCH_MS = 200;
uint8_t       activeRxPin  = IR_RX_PIN_1;
unsigned long lastRxSwitch = 0;

// ── Health LED (single WS2812 4×4 matrix, A9, 16 LEDs) ───────────────────────
MeRGBLed matrix;   // A9 — 4×4 matrix (16 LEDs)

bool rpiConnected = false;

// ── RFID reader ────────────────────────────────────────────────────────────────
const uint8_t RST_PIN = 30;
const uint8_t SS_PIN  = A6;
MFRC522 rfid(SS_PIN, RST_PIN);

const unsigned long RFID_COOLDOWN_MS = 5000;
String        lastRfidUid  = "";
unsigned long lastRfidTime = 0;

// ── Game state ─────────────────────────────────────────────────────────────────
int  health    = 100;
int  ammo      = 10;
int  ammoLevel = 2;
int  fireSpeed = 1;
bool immunable = false;
bool isDead    = false;

void respawn() {
  health    = 100;
  ammo      = 100;
  isDead    = false;
  stopPoliceLed();
  Serial.println(F("[RESPAWN] Tank back in game!"));
}

// ── Speed tracking ─────────────────────────────────────────────────────────────
float speedL = 0, speedR = 0;

// ── Timing ─────────────────────────────────────────────────────────────────────
const unsigned long TELEMETRY_INTERVAL_MS = 500;
unsigned long lastTelemetry = 0;
const unsigned long PING_INTERVAL_MS = 5000;
unsigned long lastPing = 0;

// ── Fire control ───────────────────────────────────────────────────────────────
unsigned long lastFireTime   = 0;
bool          squarePrevious = false;

// ── LED blink ──────────────────────────────────────────────────────────────────
const unsigned long BLINK_INTERVAL_MS = 100;
uint8_t       blinkSteps = 0;
uint8_t       blinkR, blinkG, blinkB;
unsigned long blinkLast  = 0;

// ── Police LED state ───────────────────────────────────────────────────────────
const unsigned long POLICE_INTERVAL_MS = 150;
bool          policeActive = false;
uint8_t       policePhase  = 0;
unsigned long policeLast   = 0;

// ── Command buffer ─────────────────────────────────────────────────────────────
String cmdBuffer = "";

// ── Encoder ISRs ───────────────────────────────────────────────────────────────
// Required by MeEncoderOnBoard. The L/R tracks use PWM-only control but the
// library still expects these to be attached. The AUX motor genuinely uses
// the encoder feedback via setTarPWM() + .loop(), which is what makes SLOT3
// reliable at low PWM values.
void isr_process_encoderL(void) {
  if (digitalRead(motorL.getPortB()) == 0) motorL.pulsePosMinus();
  else motorL.pulsePosPlus();
}
void isr_process_encoderR(void) {
  if (digitalRead(motorR.getPortB()) == 0) motorR.pulsePosMinus();
  else motorR.pulsePosPlus();
}
void isr_process_encoderAux(void) {
  if (digitalRead(motorAux.getPortB()) == 0) motorAux.pulsePosMinus();
  else motorAux.pulsePosPlus();
}

// ── Drive ──────────────────────────────────────────────────────────────────────
void stopAll() {
  motorL.setMotorPwm(0);
  motorR.setMotorPwm(0);
  motorAux.setMotorPwm(0);
}

float applyDeadzone(float v) {
  return (abs(v) < DEADZONE) ? 0 : v;
}

float applyMinSpd(float v) {
  if (v >  0.5f) return max(v, (float)minSpeed);
  if (v < -0.5f) return min(v, -(float)minSpeed);
  return 0;
}

// Tank drive: left stick Y → left track, right stick Y → right track.
void tankDrive(float leftIn, float rightIn) {
  float left  = constrain(leftIn,  -(float)maxSpeed, (float)maxSpeed);
  float right = constrain(rightIn, -(float)maxSpeed, (float)maxSpeed);

  left  = applyMinSpd(left);
  right = applyMinSpd(right);

  motorL.setMotorPwm(SIGN_L * (int)left);
  motorR.setMotorPwm(SIGN_R * (int)right);

  speedL = abs(left);
  speedR = abs(right);
}

// Aux motor: D-pad UP = forward at AUX_SPEED, DOWN = backward, neither = stop.
void auxDrive() {
  bool upHeld   = MePS2.ButtonPressed(MeJOYSTICK_UP);
  bool downHeld = MePS2.ButtonPressed(MeJOYSTICK_DOWN);

  if      (upHeld)   motorAux.setMotorPwm(SIGN_AUX *  AUX_SPEED);
  else if (downHeld) motorAux.setMotorPwm(SIGN_AUX * -AUX_SPEED);
  else               motorAux.setMotorPwm(0);
}

// ── IR blaster — NEC protocol ──────────────────────────────────────────────────
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

void sendNEC(uint8_t addr, uint8_t cmd) {
  uint32_t data = ((uint32_t)addr)              |
                  ((uint32_t)(addr ^ 0xFF) << 8) |
                  ((uint32_t)cmd           << 16)|
                  ((uint32_t)(cmd  ^ 0xFF) << 24);
  markIR(9000);
  spaceIR(4500);
  for (int i = 0; i < 32; i++) {
    markIR(560);
    spaceIR(((data >> i) & 1) ? 1690 : 560);
  }
  markIR(560);
}

void fireIR() {
  uint8_t addr = 0;
  for (uint8_t i = 0; TANK_ID[i]; i++) addr ^= (uint8_t)TANK_ID[i];

  IrReceiver.stop();
  sendNEC(addr, (uint8_t)ammoLevel);
  IrReceiver.start();
}

// ── IR receivers ──────────────────────────────────────────────────────────────
void switchIRReceiver() {
  unsigned long now = millis();
  if (now - lastRxSwitch < IR_SWITCH_MS) return;
  lastRxSwitch = now;

  activeRxPin = (activeRxPin == IR_RX_PIN_1) ? IR_RX_PIN_2 : IR_RX_PIN_1;
  IrReceiver.begin(activeRxPin, DISABLE_LED_FEEDBACK);
}

void handleIRReceive() {
  if (IrReceiver.decode()) {
    uint32_t raw = IrReceiver.decodedIRData.decodedRawData;

    if (raw != 0 && raw != 0xFFFFFFFF) {
      uint8_t addr  =  raw        & 0xFF;
      uint8_t addrN = (raw >>  8) & 0xFF;
      uint8_t cmd   = (raw >> 16) & 0xFF;
      uint8_t cmdN  = (raw >> 24) & 0xFF;

      if ((uint8_t)(addr ^ addrN) == 0xFF && (uint8_t)(cmd ^ cmdN) == 0xFF) {
        int damage = (int)cmd;

        if (damage > 0 && damage <= 10) {
          if (!immunable && !isDead) {
            int prev = health;
            health = max(0, health - damage);

            uint8_t rxNum = (activeRxPin == IR_RX_PIN_1) ? 1 : 2;
            Serial.print(F("[HIT] IR")); Serial.print(rxNum);
            Serial.print(F(" from 0x")); Serial.print(addr, HEX);
            Serial.print(F(" -")); Serial.print(damage);
            Serial.print(F(" HP | Health: ")); Serial.println(health);

            sendHitEvent(addr, damage);
            startBlink(180, 0, 0);
            updateHealthLed();

            if (health == 0 && prev > 0) {
              isDead = true;
              startPoliceLed();
              Serial.println(F("[DEAD] Drive to home base to respawn"));
            }
          } else if (!isDead) {
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

// ── RPi events ─────────────────────────────────────────────────────────────────
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
  matrix.setColor(0, r, g, b);
  matrix.show();
}

void setMatrixAlive() { setAllLeds(0,   180, 0); }
void setMatrixDead()  { setAllLeds(180,   0, 0); }

void updateHealthLed() {
  if (!rpiConnected) return;
  if      (health > 50) setAllLeds(0,   180,   0);
  else if (health >= 5) setAllLeds(180, 140,   0);
  else                  setAllLeds(180,   0,   0);
}

// ── LED blink ──────────────────────────────────────────────────────────────────
// startBlinkN — N half-periods (N/2 full blinks). Fires immediately.
// startBlink  — convenience wrapper: 2 full blinks (4 half-periods).
// Non-blocking: driven by updateBlink() every loop() tick.
// After the sequence finishes, updateHealthLed() restores the correct colour.
void startBlinkN(uint8_t r, uint8_t g, uint8_t b, uint8_t n) {
  blinkR = r; blinkG = g; blinkB = b;
  blinkSteps = n;
  blinkLast  = millis() - BLINK_INTERVAL_MS;
}
void startBlink(uint8_t r, uint8_t g, uint8_t b) {
  startBlinkN(r, g, b, 4);  // 4 half-periods = 2 full blinks
}

void updateBlink(unsigned long now) {
  if (blinkSteps == 0 || policeActive) return;  // police takes visual priority
  if (now - blinkLast < BLINK_INTERVAL_MS) return;
  blinkLast = now;

  if (blinkSteps % 2 == 0) setAllLeds(0, 0, 0);
  else                      setAllLeds(blinkR, blinkG, blinkB);

  blinkSteps--;
  if (blinkSteps == 0) updateHealthLed();
}

// ── Police LED — looping red/blue, active while tank is dead ──────────────────
void startPoliceLed() {
  blinkSteps   = 0;
  policeActive = true;
  policePhase  = 0;
  policeLast   = millis() - POLICE_INTERVAL_MS;
}

void stopPoliceLed() {
  policeActive = false;
  updateHealthLed();
}

void updatePoliceLed(unsigned long now) {
  if (!policeActive) return;
  if (now - policeLast < POLICE_INTERVAL_MS) return;
  policeLast = now;

  if (policePhase == 0) setAllLeds(180,   0,   0);  // red
  else                  setAllLeds(  0,   0, 180);  // blue
  policePhase ^= 1;
}

// ── Serial handler ────────────────────────────────────────────────────────────
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

  if (strcmp(type, "command") == 0) {
    const char* param = ev["param"] | "";
    int value = ev["value"] | 0;

    if (strcmp(param, "health") == 0) {
      int prev = health;
      health = constrain(value, 0, 100);
      if (isDead && health > 0) {
        isDead = false;
        stopPoliceLed();
        Serial.println(F("[RESPAWN] Server restored health — back in game!"));
      }
      if      (health < prev) startBlink(180, 0,   0);
      else if (health > prev) startBlink(0,   180, 0);
      else                    updateHealthLed();
    }
    else if (strcmp(param, "ammo")      == 0)   ammo      = constrain(value, 0, 100);
    else if (strcmp(param, "ammoLevel") == 0)   ammoLevel = constrain(value, 1, 10);
    else if (strcmp(param, "fireSpeed") == 0)   fireSpeed = constrain(value, 1, 10);
    else if (strcmp(param, "immunable") == 0)   immunable = (value != 0);
    else if (strcmp(param, "maxSpeed")  == 0)   maxSpeed  = constrain(value, 1, 255);
    else if (strcmp(param, "minSpeed")  == 0)   minSpeed  = constrain(value, 0, 255);
    else if (strcmp(param, "led")       == 0) {
      // Server-triggered LED effect.
      // 1 = treasure (gold ×3)  2 = immune (purple ×2)  3 = win/bonus (white ×4)
      // 4 = police (red/blue loop while dead — stopped automatically on respawn)
      switch (value) {
        case 1: startBlinkN(180, 120,   0, 6); break;
        case 2: startBlinkN(120,   0, 180, 4); break;
        case 3: startBlinkN(180, 180, 180, 8); break;
        case 4: startPoliceLed();              break;
      }
    }

    Serial.print("[CMD] "); Serial.print(param);
    Serial.print(" = "); Serial.println(value);

  } else if (strcmp(type, "system") == 0) {
    const char* action = ev["action"] | "";
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
  Serial.begin(115200);
  Serial2.begin(115200);

  // Encoder ISRs — required by MeEncoderOnBoard.
  // The aux motor on SLOT3 genuinely needs this (it drives setTarPWM/.loop control);
  // the track motors just attach it for library compatibility.
  attachInterrupt(motorL.getIntNum(),   isr_process_encoderL,   RISING);
  attachInterrupt(motorR.getIntNum(),   isr_process_encoderR,   RISING);
  attachInterrupt(motorAux.getIntNum(), isr_process_encoderAux, RISING);

  // IR transmitter
  pinMode(IR_PIN, OUTPUT);
  digitalWrite(IR_PIN, LOW);

  // IR receivers
  IrReceiver.begin(IR_RX_PIN_1, DISABLE_LED_FEEDBACK);
  activeRxPin = IR_RX_PIN_1;
  Serial.print(F("IR RX: A11 (pin ")); Serial.print(IR_RX_PIN_1);
  Serial.print(F(") / A10 (pin "));   Serial.print(IR_RX_PIN_2);
  Serial.println(F(") — time-multiplexed"));
  Serial.print(F("IR TX: A12 (pin ")); Serial.print(IR_PIN);
  Serial.println(F(") — NEC software"));

  // 4×4 health matrix on A9 — start yellow (waiting for RPi)
  matrix.setpin(A9); matrix.setNumber(16);
  setAllLeds(180, 140, 0);

//RFID fix 
pinMode(53, OUTPUT);
digitalWrite(53, HIGH);  // Mega HW-SS: must stay HIGH or SPI drops to slave mode (kills RFID)

  SPI.begin();
  rfid.PCD_Init();
  Serial.print("RFID ready. SS_PIN="); Serial.print(SS_PIN);
  Serial.print("  Tank: "); Serial.println(TANK_ID);

  // PWM timer setup — same as the mBlock5 reference sketch.
  // Timer1 → SLOT1 (left track), Timer2 → SLOT4 (right track).
  // SLOT3 (aux) is driven by the .loop() PID system; Timer3/4 left at defaults.
  TCCR1A = _BV(WGM10);
  TCCR1B = _BV(CS11) | _BV(WGM12);
  TCCR2A = _BV(WGM21) | _BV(WGM20);
  TCCR2B = _BV(CS21);

  MePS2.begin(115200);
  delay(1000);
  stopAll();

  Serial.println(F("Motors: SLOT1=LEFT track, SLOT4=RIGHT track, SLOT3=AUX"));
  Serial.println(F("Control: L-stick Y=left track, R-stick Y=right track, D-pad U/D=aux"));
}

// ── Loop ───────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  updateBlink(now);
  updatePoliceLed(now);
  handleIRReceive();

  // ── Death state ────────────────────────────────────────────────────────────
  if (isDead) {
    while (Serial2.available()) {
      char c = (char)Serial2.read();
      Serial.write(c);
      if (c == '\n') {
        handleSerialLine(cmdBuffer);
        cmdBuffer = "";
      } else if (c != '\r') {
        cmdBuffer += c;
      }
    }

    String deadUid = readRfidUid(now);
    if (deadUid.length() > 0) {
      Serial.print(F("RFID (dead): ")); Serial.println(deadUid);
      sendRfidEvent(deadUid);
    }

    // Driving still works while dead — both tracks and aux motor
    MePS2.loop();
    float ly = applyDeadzone(MePS2.MeAnalog(MeJOYSTICK_LY));
    float ry = applyDeadzone(MePS2.MeAnalog(MeJOYSTICK_RY));
    tankDrive(ly, ry);
    auxDrive();

    if (now - lastPing >= PING_INTERVAL_MS) { sendPing(); lastPing = now; }
    if (now - lastTelemetry >= TELEMETRY_INTERVAL_MS) { sendTelemetry(); lastTelemetry = now; }
    return;
  }

  // ── Receive from RPi ───────────────────────────────────────────────────────
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    Serial.write(c);
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

  // ── Read controller — tracks + aux motor ───────────────────────────────────
  MePS2.loop();
  float ly = applyDeadzone(MePS2.MeAnalog(MeJOYSTICK_LY));
  float ry = applyDeadzone(MePS2.MeAnalog(MeJOYSTICK_RY));
  tankDrive(ly, ry);
  auxDrive();

  // ── Square → fire ──────────────────────────────────────────────────────────
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

  // ── Periodic ping + telemetry ──────────────────────────────────────────────
  if (now - lastPing >= PING_INTERVAL_MS) {
    sendPing();
    lastPing = now;
  }
  if (now - lastTelemetry >= TELEMETRY_INTERVAL_MS) {
    sendTelemetry();
    lastTelemetry = now;
  }
}
