# Project Wheely

A high-performance Mecanum-wheel robot powered by MegaPi and Raspberry Pi.

---

## Core Architecture

Wheely is an omnidirectional mobile platform based on the **mBot Mega** architecture. It uses a **MegaPi** (ATmega2560) for low-level motor control and a **Raspberry Pi Zero** as the network bridge.

- **Reference:** [mBot Mega Robot Car](https://www.robotshop.com/products/makeblock-mbot-mega-robot-car-bluetooth-remote-controller)
- **Drive System:** 4WD Mecanum (omnidirectional movement)

---

## Bill of Materials

### Electronic Components

| Qty | Component | Description |
|:---|:---|:---|
| 1 | **MegaPi** | Main microcontroller (Arduino Mega 2560 compatible) |
| 2 | **DC Motor Drivers** | Dual-channel drivers for all 4 motors |
| 1 | **Bluetooth Module** | Wireless remote control (PS2 protocol) |
| 4 | **Encoder DC Motors** | High-torque motors for precision movement |
| 2 | **RGB LED Modules** | Status indicators |
| 1 | **Raspberry Pi Zero** | Network bridge to MQTT |

### Building Components

- **Chassis:** 1x Body Shell, 1x Upper Shell
- **Power:** 1x 6-AA Battery Holder (9V DC)
- **Wheels:** 2x pairs of 60mm Mecanum Wheels (Left/Right configuration)
- **Hardware:** 68x spacers, standoffs, screws, and nuts
- **Transmission:** 4x motor couplings and 4x brackets

---

## Hardware Integration

### MegaPi ↔ Raspberry Pi Zero

Connected via the MegaPi's dedicated RPi port using a 10-pin flex cable:

- [10-Pin Header](https://www.amazon.com/dp/B0F9NSTWCV)
- [Flex Flat Cable (FFC)](https://www.amazon.com/dp/B01DP55PZQ)

> **Wiring Alert:** Ensure the 5V output from MegaPi matches the 5V input on the Pi side. Solder the 10-pin header on the upper side of the MegaPi with the key/shroud facing the board edge.

The UART connection uses `Serial2` on the ATmega2560 at **115200 baud**. `Serial` (USB) is used for debug output only.

---

## Firmware

**File:** `megaPI/wheely.ino`

**Required libraries** (install via Arduino Library Manager):
- `MeMegaPi` — Makeblock motor drivers
- `MePS2` — PS2 Bluetooth controller
- `ArduinoJson` by Benoit Blanchon

### Controls

| Joystick | Axis | Action |
|:---|:---|:---|
| Left stick Y | `LY` | Forward / back |
| Left stick X | `LX` | Strafe left / right |
| Right stick X | `RX` | Rotate |

### Motor port mapping

| Port | Position | Calibration |
|:---|:---|:---|
| PORT_12 | Front-Left | +1 |
| PORT_4 | Front-Right | −1 |
| PORT_9 | Rear-Left | −1 |
| PORT_1 | Rear-Right | +1 |

Deadzone: **20 units** | Max PWM: **255**

### Telemetry

Every **500 ms** the firmware sends a JSON telemetry packet over `Serial2` to the Raspberry Pi:

```json
{
  "timestamp": 1234,
  "event": {
    "type": "telemetry",
    "data": {
      "speed": 127,
      "health": 100,
      "ammo": 100,
      "ammoLevel": 3,
      "fireSpeed": 5,
      "immunable": false
    }
  }
}
```

`speed` is the average absolute PWM value across all four motors.

### Remote commands

Game-state variables can be updated at runtime by sending a `command` JSON message via `Serial2` (the Raspberry Pi forwards MQTT commands here automatically):

```json
{ "timestamp": 0, "event": { "type": "command", "param": "health", "value": 80 } }
```

| Variable | Default | Range | Description |
|:---|:---|:---|:---|
| `health` | 100 | 0–100 | Tank HP |
| `ammo` | 100 | 0–100 | Remaining ammunition |
| `ammoLevel` | 3 | 1–10 | Ammo power level |
| `fireSpeed` | 5 | 1–10 | Fire rate level |
| `immunable` | false | bool | Immune to damage |

---

## Assembly Notes

1. **Wheel orientation:** Install Mecanum wheels in the correct "X" pattern for proper omnidirectional translation.
2. **Cable management:** Use the FFC to allow flexible positioning of the Pi inside the shell.
