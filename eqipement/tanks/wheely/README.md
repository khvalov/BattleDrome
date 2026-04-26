<img width="1024" height="721" alt="image" src="https://github.com/user-attachments/assets/c787c519-9e55-49c5-81ca-1b3138111e80" />

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

UART uses **`Serial2`** on the ATmega2560 at **115200 baud** — this is the hardware serial pre-wired to the MegaPi's RPi flex-cable connector. `Serial` (USB) is for debug output only.

---

## Firmware

**File:** `megaPI/wheely.ino`

**Required libraries** (install via Arduino Library Manager):
- `MeMegaPi` — Makeblock motor drivers
- `MePS2` — PS2 Bluetooth controller
- `ArduinoJson` by Benoit Blanchon

### Controls

| Input | Axis | Action |
|:---|:---|:---|
| Left stick Y | `LY` | Forward / back |
| Left stick X | `LX` | Strafe left / right |
| Right stick X | `RX` | Rotate |

### Motor port mapping

| Port | Position | Direction |
|:---|:---|:---|
| PORT_12 | Front-Left | +1 (normal) |
| PORT_4 | Front-Right | −1 (reversed) |
| PORT_9 | Rear-Left | −1 (reversed) |
| PORT_1 | Rear-Right | +1 (normal) |

Deadzone: **20 units** | Max PWM: **255**

### Tank settings

Defined as constants at the top of `wheely.ino`:

| Constant | Value | Description |
|:---|:---|:---|
| `TANK_ID` | `"WHLYA1"` | Unique identifier, up to 6 characters |
| `TANK_TYPE` | `"wheely"` | Tank model name |
| `IR_TX_PIN` | `3` | IR LED data pin — change to match your wiring |

### Firing (Square button)

Pressing the **Square** button triggers a shot if:
- `ammo > 0`
- Time since last shot ≥ `fireSpeed` ms

On each shot the firmware:
1. Pulses `IR_TX_PIN` (placeholder — replace with your IR library call)
2. Decrements `ammo` by 1
3. Immediately sends a `fire` event over `Serial2`

The IR packet encodes `TANK_ID` and `ammoLevel` so the receiving tank can identify the attacker and calculate damage.

> **Note:** Button detection uses `!(MePS2.ButtonState() & PS2_SQUARE)`. Adjust the constant name if your MePS2 library version differs.

### Telemetry

Every **500 ms** the firmware sends a `telemetry` packet over `Serial2`. The Raspberry Pi enriches it with `ip` and `hostname` before publishing to MQTT:

```json
{
  "timestamp": 1234,
  "event": {
    "type": "telemetry",
    "data": {
      "tankId": "WHLYA1", "tankType": "wheely",
      "speed": 127, "health": 100, "ammo": 99,
      "ammoLevel": 3, "fireSpeed": 5, "immunable": false
    }
  }
}
```

`speed` is the average absolute PWM across all four motors.

### Fire event

Sent immediately on each shot (separate from the 500 ms telemetry cycle):

```json
{
  "timestamp": 1234,
  "event": {
    "type": "fire",
    "data": { "tankId": "WHLYA1", "tankType": "wheely", "senderId": "WHLYA1", "ammoLevel": 3, "ammo": 99 }
  }
}
```

The Raspberry Pi enriches this with `ip` and `hostname` before it reaches MQTT.

### Game-state variables

Held in memory on the Arduino. Updated at runtime via `command` messages received on `Serial2`.

| Variable | Default | Range | Description |
|:---|:---|:---|:---|
| `health` | 100 | 0–100 | Tank HP |
| `ammo` | 100 | 0–100 | Remaining ammunition |
| `ammoLevel` | 3 | 1–10 | Ammo power / damage multiplier |
| `fireSpeed` | 5 | 1–10 | Minimum ms between shots |
| `immunable` | false | bool | Immune to damage |

### Remote commands

Send a `command` JSON to the tank's MQTT commands topic — the Raspberry Pi forwards it to `Serial2`:

```json
{ "timestamp": 0, "event": { "type": "command", "param": "health", "value": 80 } }
```

Values are clamped via `constrain()`. `immunable` is boolean (`0` = false, any non-zero = true).

---

## Assembly Notes

1. **Wheel orientation:** Install Mecanum wheels in the correct "X" pattern for proper omnidirectional translation.
2. **Cable management:** Use the FFC to allow flexible positioning of the Pi inside the shell.
