#include <Arduino.h>
#include <Wire.h>
#include <SoftwareSerial.h>
#include <MeMegaPi.h>
#include <MePS2.h>
#include <SPI.h>
#include <MFRC522.h>
#include <IRremote.h>
#include <avr/wdt.h>

// ================= CONFIGURATION =================
namespace Config {
  constexpr float BASE_MAX_SPEED = 185.0f;
  constexpr int DEADZONE = 15;
  constexpr int INITIAL_HEALTH = 100;
  constexpr int MIN_HEALTH = 0;
  constexpr int MAX_HEALTH = 100;
  constexpr unsigned long DEATH_DELAY_MS = 5000;
  constexpr unsigned long HEALTH_DISPLAY_INTERVAL = 1000;
  
  // Speed scaling based on health
  constexpr float MIN_SPEED_MULTIPLIER = 0.3f;  // 30% speed at low health
  constexpr float MAX_SPEED_MULTIPLIER = 1.0f;  // 100% speed at full health
  
  // RFID
  constexpr uint8_t RST_PIN = 30;
  constexpr uint8_t SS_PIN = 22;
  const byte FLAG_UID[4] = {0x97, 0xCB, 0x1B, 0x03};
  
  // Dual IR Receivers - Time-multiplexing approach
  constexpr uint8_t IR_RECEIVE_PIN_1 = 11;
  constexpr uint8_t IR_RECEIVE_PIN_2 = 12;
  constexpr unsigned long IR_SWITCH_INTERVAL_MS = 50; // Switch between receivers every 50ms
  
  // IR Transmitter
  constexpr uint8_t IR_SEND_PIN = 35;
  constexpr uint32_t IR_ATTACK_CODE = 0x123456AB;
  constexpr unsigned long ATTACK_COOLDOWN_MS = 500; // Minimum time between attacks
  
  //Diagnostics
  constexpr bool DEBUG_MOTORS = false;
  constexpr bool DEBUG_HEALTH = true;
  constexpr bool DEBUG_IR_SEND = false;
}

// ================= HARDWARE =================
MeEncoderOnBoard Encoder_3(SLOT2);
MeEncoderOnBoard Encoder_4(SLOT4);
MePS2 ps2(PORT_15);
MFRC522 rfid(Config::SS_PIN, Config::RST_PIN);

// IR System - Single receiver that switches between pins
// IrReceiver and IrSender are global objects in IRremote 3.0+

// ================= STATE MANAGEMENT =================
struct RobotState {
  int health;
  float currentMaxSpeed;
  bool flagCaptured;
  bool isDead;
  bool wantReset;
  unsigned long lastHealthDisplay;
  unsigned long lastAttackTime;
  unsigned long lastIRSwitch;
  uint8_t activeIRPin;
  
  RobotState() : 
    health(Config::INITIAL_HEALTH),
    currentMaxSpeed(Config::BASE_MAX_SPEED),
    flagCaptured(false),
    isDead(false),
    wantReset(false),
    lastHealthDisplay(0),
    lastAttackTime(0),
    lastIRSwitch(0),
    activeIRPin(Config::IR_RECEIVE_PIN_1) {}
    
  void reset() {
    health = Config::INITIAL_HEALTH;
    updateMaxSpeed();
    flagCaptured = false;
    isDead = false;
    wantReset = false;
    lastAttackTime = 0;
  }
  
  void updateMaxSpeed() {
    // Calculate speed multiplier based on health percentage
    float healthPercent = constrain(health, Config::MIN_HEALTH, Config::MAX_HEALTH) / 100.0f;
    float speedMultiplier = Config::MIN_SPEED_MULTIPLIER + 
                           (healthPercent * (Config::MAX_SPEED_MULTIPLIER - Config::MIN_SPEED_MULTIPLIER));
    
    currentMaxSpeed = Config::BASE_MAX_SPEED * speedMultiplier;
    
    if (Config::DEBUG_HEALTH) {
      Serial.print(F("Health: "));
      Serial.print(health);
      Serial.print(F("% | Speed: "));
      Serial.print(currentMaxSpeed);
      Serial.print(F(" ("));
      Serial.print(speedMultiplier * 100);
      Serial.println(F("%)"));
    }
  }
  
  void takeDamage(int damage, uint8_t receiverNum) {
    if (damage <= 0 || isDead) return;
    
    health = max(Config::MIN_HEALTH, health - damage);
    updateMaxSpeed();
    
    Serial.print(F("⚠ HIT from IR"));
    Serial.print(receiverNum);
    Serial.print(F("! Damage: "));
    Serial.print(damage);
    Serial.print(F(" | Health: "));
    Serial.print(health);
    Serial.print(F(" | Max Speed: "));
    Serial.println(currentMaxSpeed);
    
    if (health <= 0) {
      isDead = true;
    }
  }
};

RobotState state;

// ================= ENCODER ISR =================
inline void encoderISR(MeEncoderOnBoard &enc) {
  digitalRead(enc.getPortB()) == LOW ? 
    enc.pulsePosMinus() : enc.pulsePosPlus();
}

void isr_encoder3() { encoderISR(Encoder_3); }
void isr_encoder4() { encoderISR(Encoder_4); }

// ================= MOTOR CONTROL =================
inline void setMotors(float left, float right) {
  // Scale motors by current max speed (health-based)
  float scaledLeft = constrain(left, -state.currentMaxSpeed, state.currentMaxSpeed);
  float scaledRight = constrain(right, -state.currentMaxSpeed, state.currentMaxSpeed);
  
  Encoder_3.runSpeed(scaledLeft);
  Encoder_4.runSpeed(scaledRight);
  
  if (Config::DEBUG_MOTORS) {
    Serial.print(F("L:"));
    Serial.print(scaledLeft);
    Serial.print(F(" R:"));
    Serial.println(scaledRight);
  }
}

inline void stopMotors() {
  Encoder_3.runSpeed(0);
  Encoder_4.runSpeed(0);
}

inline void forceMotorUpdate() {
  Encoder_3.loop();
  Encoder_4.loop();
}

// ================= UTILITY FUNCTIONS =================
inline int applyDeadzone(int value) {
  return (abs(value) < Config::DEADZONE) ? 0 : value;
}

inline float normalizeJoystick(int raw) {
  return constrain(raw, -255, 255) / 255.0f;
}

void printHex(const byte *buffer, byte bufferSize) {
  for (byte i = 0; i < bufferSize; i++) {
    Serial.print(buffer[i] < 0x10 ? " 0" : " ");
    Serial.print(buffer[i], HEX);
  }
  Serial.println();
}

bool uidMatches(const byte *uid) {
  return memcmp(uid, Config::FLAG_UID, 4) == 0;
}

void performReset() {
  Serial.println(F("Resetting in 1 second..."));
  delay(1000);
  wdt_enable(WDTO_500MS);
  while (1) {}
}

// ================= SETUP =================
void initializeEncoders() {
  Serial.println(F("Initializing encoders..."));
  
  Encoder_3.setPulse(8);
  Encoder_3.setRatio(46.67);
  Encoder_3.setPosPid(1.8, 0, 1.2);
  Encoder_3.setSpeedPid(0.18, 0, 0);
  Encoder_3.setPulsePos(0);
  
  Encoder_4.setPulse(8);
  Encoder_4.setRatio(46.67);
  Encoder_4.setPosPid(1.8, 0, 1.2);
  Encoder_4.setSpeedPid(0.18, 0, 0);
  Encoder_4.setPulsePos(0);
  
  attachInterrupt(Encoder_3.getIntNum(), isr_encoder3, RISING);
  attachInterrupt(Encoder_4.getIntNum(), isr_encoder4, RISING);
  
  Serial.println(F("Testing motors..."));
  
  Encoder_3.runSpeed(50);
  Encoder_4.runSpeed(50);
  delay(200);
  for (int i = 0; i < 20; i++) {
    Encoder_3.loop();
    Encoder_4.loop();
    delay(10);
  }
  
  Encoder_3.runSpeed(0);
  Encoder_4.runSpeed(0);
  delay(200);
  for (int i = 0; i < 20; i++) {
    Encoder_3.loop();
    Encoder_4.loop();
    delay(10);
  }
  
  Encoder_3.setPulsePos(0);
  Encoder_4.setPulsePos(0);
  
  Serial.println(F("Motor test complete!"));
}

void initializePWM() {
  TCCR1A = _BV(WGM10);
  TCCR1B = _BV(CS11) | _BV(WGM12);
  TCCR2A = _BV(WGM21) | _BV(WGM20);
  TCCR2B = _BV(CS21);
}

void initializeIR() {
  Serial.println(F("Initializing dual IR receiver system..."));
  
  // Start with receiver on pin 11
  IrReceiver.begin(Config::IR_RECEIVE_PIN_1, ENABLE_LED_FEEDBACK);
  state.activeIRPin = Config::IR_RECEIVE_PIN_1;
  
  Serial.print(F("IR Receiver 1: Pin "));
  Serial.println(Config::IR_RECEIVE_PIN_1);
  Serial.print(F("IR Receiver 2: Pin "));
  Serial.println(Config::IR_RECEIVE_PIN_2);
  Serial.println(F("Using time-multiplexed dual receiver"));
  
  // Initialize IR transmitter on pin 35
  Serial.println(F("Initializing IR transmitter..."));
  IrSender.begin(Config::IR_SEND_PIN, ENABLE_LED_FEEDBACK);
  Serial.print(F("IR Transmitter: Pin "));
  Serial.println(Config::IR_SEND_PIN);
  Serial.print(F("Attack Code: 0x"));
  Serial.println(Config::IR_ATTACK_CODE, HEX);
}

void sendEspCommand(String cmd, int timeout) {
  Serial2.println(cmd);
  delay(timeout);
  while(Serial2.available()) {
    Serial.write(Serial2.read());
  }
}

void sendEspData(int id, String data) {
  String cmd = "AT+CIPSEND=" + String(id) + "," + String(data.length());
  Serial2.println(cmd);
  delay(100);
  Serial2.print(data);
  delay(100);
  Serial2.println("AT+CIPCLOSE=" + String(id));
}


void setup() {
  Serial.begin(115200);
  //Serial2 for esp8266 comm
  while (!Serial && millis() < 3000) {}
  //Wait for 3 second to give ESP chance to boot.
  delay(3000);
  
  Serial2.begin(115200);
  while (!Serial2 && millis() < 3000) {}

  
  initializePWM();
  
  ps2.begin(115200);
  initializeEncoders();
  
  SPI.begin();
  rfid.PCD_Init();
  
  initializeIR();
  
  wdt_disable();
  
  Serial.println(F("============================="));
  Serial.println(F("   RoBBY Robot System v3.3   "));
  Serial.println(F("   Dual IR Time-Multiplexed  "));
  Serial.println(F("============================="));
  Serial.print(F("Health: "));
  Serial.print(state.health);
  Serial.print(F(" | Max Speed: "));
  Serial.println(state.currentMaxSpeed);
  Serial.println(F("IR: Alternating pins 11 & 12"));
  Serial.println(F("IR TX: Pin 35"));
  Serial.println(F("Controls:"));
  Serial.println(F("  R1 Button - Attack"));
  Serial.println(F("  + Button  - Reset"));
  Serial.println(F("Ready for battle!"));
  Serial.println();
}

// ================= GAME LOGIC =================
void switchIRReceiver() {
  unsigned long now = millis();
  
  // Check if it's time to switch
  if (now - state.lastIRSwitch >= Config::IR_SWITCH_INTERVAL_MS) {
    state.lastIRSwitch = now;
    
    // Switch to the other pin
    if (state.activeIRPin == Config::IR_RECEIVE_PIN_1) {
      state.activeIRPin = Config::IR_RECEIVE_PIN_2;
    } else {
      state.activeIRPin = Config::IR_RECEIVE_PIN_1;
    }
    
    // Reinitialize receiver on new pin
    IrReceiver.begin(state.activeIRPin, DISABLE_LED_FEEDBACK);
    
    if (Config::DEBUG_IR_SEND) {
      Serial.print(F("Switched to IR pin: "));
      Serial.println(state.activeIRPin);
    }
  }
}

void sendIRAttack() {
  unsigned long now = millis();
  
  // Check cooldown
  if (now - state.lastAttackTime < Config::ATTACK_COOLDOWN_MS) {
    if (Config::DEBUG_IR_SEND) {
      Serial.println(F("Attack on cooldown!"));
    }
    return;
  }
  
  // Send IR code using IRremote 3.0+ API
  IrSender.sendNECRaw(Config::IR_ATTACK_CODE, 0);
  state.lastAttackTime = now;
  
  if (Config::DEBUG_IR_SEND) {
    Serial.print(F("🔫 ATTACK! Sent IR code: 0x"));
    Serial.println(Config::IR_ATTACK_CODE, HEX);
  }
  
  // Restart receiver after transmission
  IrReceiver.start();
}

void handleIRDamage() {
  // Check for incoming IR on active receiver
  if (IrReceiver.decode()) {
    uint32_t hexCode = IrReceiver.decodedIRData.decodedRawData;
    
    // Ignore repeat/invalid codes
    if (hexCode != 0xFFFFFFFF && hexCode != 0x0) {
      // Extract damage from byte 2 (bits 16-23)
      byte damage = (byte)((hexCode >> 16) & 0xFF);
      
      if (damage > 0) {
        // Determine which receiver got the hit
        uint8_t receiverNum = (state.activeIRPin == Config::IR_RECEIVE_PIN_1) ? 1 : 2;
        state.takeDamage(damage, receiverNum);
      }
      
      if (Config::DEBUG_IR_SEND) {
        Serial.print(F("IR"));
        Serial.print((state.activeIRPin == Config::IR_RECEIVE_PIN_1) ? 1 : 2);
        Serial.print(F(" Received: 0x"));
        Serial.print(hexCode, HEX);
        Serial.print(F(" | Damage: "));
        Serial.println(damage);
      }
    }
    
    // Resume receiving
    IrReceiver.resume();
  }
  
  // Switch between receivers
  switchIRReceiver();
}

void handlePS2Input() {
  ps2.loop();
  
  // Reset button (+)
  if (ps2.ButtonPressed(13)) {
    Serial.println(F("Manual reset requested..."));
    state.wantReset = true;
  }
  
  // Attack button (example: button 14 - could be any button you prefer)
  // Common PS2 buttons: 
  // 14 = Triangle, 15 = Circle, 16 = X, 17 = Square
  // 18 = L1, 19 = R1, 20 = L2, 21 = R2
  if (ps2.ButtonPressed(19)) {  // R1 button for attack
    sendIRAttack();
  }
}

void handleRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return;
  }
  
  if (uidMatches(rfid.uid.uidByte)) {
    Serial.println(F(""));
    Serial.println(F("🏁 ==========================="));
    Serial.println(F("   FLAG CAPTURED! VICTORY!"));
    Serial.println(F("   ==========================="));
    state.flagCaptured = true;
    stopMotors();
  } else {
    Serial.print(F("Unknown RFID tag: "));
    printHex(rfid.uid.uidByte, rfid.uid.size);
  }
  
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

void updateMotorsFromJoystick() {
  int rawX = applyDeadzone(ps2.MeAnalog(MeJOYSTICK_LX));
  int rawY = applyDeadzone(ps2.MeAnalog(MeJOYSTICK_LY));
  
  float normX = normalizeJoystick(rawX);
  float normY = normalizeJoystick(rawY);
  
  // Arcade drive with health-based speed scaling
  float forward = -normX * state.currentMaxSpeed;
  float turn = -normY * state.currentMaxSpeed;
  
  float leftMotor = forward + turn;
  float rightMotor = forward - turn;
  
  setMotors(leftMotor, rightMotor);
}

void updateEncoders() {
  Encoder_3.loop();
  Encoder_4.loop();
}

void displayPeriodicHealth() {
  unsigned long now = millis();
  if (now - state.lastHealthDisplay >= Config::HEALTH_DISPLAY_INTERVAL) {
    state.lastHealthDisplay = now;
    Serial.print(F("❤ Health: "));
    Serial.print(state.health);
    Serial.print(F(" | Speed: "));
    Serial.print((state.currentMaxSpeed / Config::BASE_MAX_SPEED) * 100);
    Serial.println(F("%"));
  }
}

// ================= MAIN LOOP =================
void loop() {
  wdt_reset();
  
  if (state.wantReset) {
    performReset();
  }
  
  if (state.isDead) {
    stopMotors();
    Serial.println(F(""));
    Serial.println(F("💀 ==========================="));
    Serial.println(F("      YOU DIED!"));
    Serial.println(F("   Press + to respawn"));
    Serial.println(F("   ==========================="));
    delay(Config::DEATH_DELAY_MS);
    state.reset();
    return;
  }
  
  handleIRDamage();
  handlePS2Input();
  handleRFID();
  
  if (state.flagCaptured) {
    updateEncoders();
    return;
  }
  
  updateMotorsFromJoystick();
  updateEncoders();
  
  delayMicroseconds(100);
}
