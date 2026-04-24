#include <Arduino.h>
#include <Wire.h>
#include <SoftwareSerial.h>
#include <MeMegaPi.h>
#include <MePS2.h>

// Physical mapping:
// PORT_12 = Front-Left
// PORT_4  = Front-Right
// PORT_9  = Rear-Left
// PORT_1  = Rear-Right
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

void stopAll() {
  portFL.run(0);
  portFR.run(0);
  portRL.run(0);
  portRR.run(0);
}

float applyDeadzone(float value) {
  if(abs(value) < DEADZONE) return 0;
  return value;
}

void mecanumDrive(float lx, float ly, float rx) {
  // Forward/back
  float flFwd = ly;
  float frFwd = ly;
  float rlFwd = ly;
  float rrFwd = ly;

  // Strafe: X-pattern mecanum
  float flStr =  lx;
  float frStr = -lx;
  float rlStr = -lx;
  float rrStr =  lx;

  // Rotate
  float flRot =  rx;
  float frRot = -rx;
  float rlRot =  rx;
  float rrRot = -rx;

  float frontLeft  = flFwd + flStr + flRot;
  float frontRight = frFwd + frStr + frRot;
  float rearLeft   = rlFwd + rlStr + rlRot;
  float rearRight  = rrFwd + rrStr + rrRot;

  float maxVal = max(max(abs(frontLeft), abs(frontRight)),
                     max(abs(rearLeft),  abs(rearRight)));
  if(maxVal > MAX_SPEED) {
    float scale = MAX_SPEED / maxVal;
    frontLeft  *= scale;
    frontRight *= scale;
    rearLeft   *= scale;
    rearRight  *= scale;
  }

  portFL.run(SIGN_FL * frontLeft);
  portFR.run(SIGN_FR * frontRight);
  portRL.run(SIGN_RL * rearLeft);
  portRR.run(SIGN_RR * rearRight);
}

void setup() {
  Serial.begin(9600);
  TCCR1A = _BV(WGM10);
  TCCR1B = _BV(CS11) | _BV(WGM12);
  TCCR2A = _BV(WGM21) | _BV(WGM20);
  TCCR2B = _BV(CS21);
  MePS2.begin(115200);
  delay(1000);
  stopAll();
  Serial.println("Ready.");
}

void _loop() {
  MePS2.loop();

  // Physical axis mapping:
  //   Joystick up/down (LY axis) → forward/back
  //   Joystick L/R     (LX axis) → strafe
  //   Right stick L/R  (RX axis) → rotate
  float physLY = applyDeadzone(MePS2.MeAnalog(MeJOYSTICK_LY));
  float physLX = applyDeadzone(MePS2.MeAnalog(MeJOYSTICK_LX));
  float physRX = applyDeadzone(MePS2.MeAnalog(MeJOYSTICK_RX));

  // mecanumDrive(lx=strafe, ly=forward, rx=rotate)
  mecanumDrive(physRX, -physLX, physLY);
}

void loop() {
  _loop();
}
