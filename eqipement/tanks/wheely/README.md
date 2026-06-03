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
| 2 | **WS2812 3×3 LED matrices** | Health / status indicators (MeRGBLed, 9 LEDs each, pins A14 and A13) |
| 1 | **Raspberry Pi Zero** | Network bridge to MQTT |
| 1 | **MFRC522 RFID reader** | SPI RFID card reader (RST=pin 30, SS=pin 7) |
| 1 | **IR LED** | Firing transmitter (pin A12) |

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
- `MeMegaPi` — Makeblock motor drivers; also bundles `MeRGBLed` for WS2812 LEDs
- `MePS2` — PS2 Bluetooth controller
- `ArduinoJson` by Benoit Blanchon
- `MFRC522` by Miguel Balboa — RFID reader
- `IRremote` by shirriff / z3t0 / ArminJo — IR receive decoding
- `SPI`, `Wire`, `SoftwareSerial` — built-in Arduino libraries

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

Deadzone: **20 units**

### Speed limits

Both limits are runtime-mutable via `command` messages or RFID actions.

| Variable | Default | Range | Description |
|:---|:---|:---|:---|
| `maxSpeed` | 160 | 1–255 | Motor PWM cap |
| `minSpeed` | 20 | 0–255 | Motor PWM floor when moving |

### RFID reader

| Constant | Value | Description |
|:---|:---|:---|
| `RST_PIN` | `30` | MFRC522 reset pin |
| `SS_PIN` | `A6` | MFRC522 SPI slave-select pin (= pin 60 on ATmega2560) |
| `RFID_COOLDOWN_MS` | `5000` | Min ms before the same UID triggers again |

Uses hardware SPI (MOSI=51, MISO=50, SCK=52 on ATmega2560). When a card is detected the firmware reads the UID, applies the 5 s debounce, and sends an `rfid` event over `Serial2`:

```json
{ "timestamp": 1234, "event": { "type": "rfid", "data": { "tankId": "WHLYA1", "tankType": "wheely", "uid": "A1B2C3D4" } } }
```

The Raspberry Pi enriches the event with `ip` and `hostname` before publishing to MQTT. The central server then looks up the UID in its `RFID_ACTIONS` table and sends a `command` back if a match is found.

### Tank settings

Defined as constants at the top of `wheely.ino`:

| Constant | Value | Description |
|:---|:---|:---|
| `TANK_ID` | `"WHLYA1"` | Unique identifier, up to 6 characters |
| `TANK_TYPE` | `"wheely"` | Tank model name |
| `IR_PIN` | `A12` | IR LED transmit pin (= pin 66 on ATmega2560) |
| `IR_RX_PIN_1` | `A11` | First IR receiver (= pin 65) |
| `IR_RX_PIN_2` | `A10` | Second IR receiver (= pin 64) |

### Firing (Square button)

Pressing the **Square** button triggers a shot if:
- `ammo > 0`
- Time since last shot ≥ `fireSpeed` ms

On each shot the firmware:
1. Pauses the IR receiver ISR (`IrReceiver.stop()`)
2. Transmits a **NEC IR frame** on `IR_PIN` (38 kHz software carrier)
   - Address byte: XOR-fold of `TANK_ID` ASCII bytes (identifies the shooter)
   - Command byte: current `ammoLevel` (damage level)
3. Resumes the IR receiver ISR (`IrReceiver.start()`)
4. Decrements `ammo` by 1
5. Immediately sends a `fire` event over `Serial2`

> **Note:** Button detection uses `MePS2.ButtonPressed(MeJOYSTICK_SQUARE)`. Rising-edge logic (one shot per press) is handled in firmware with `squarePrevious`.

### Receiving hits (dual IR receivers)

Two IR receivers cover both sides of the tank. Because `IRremote` can only sample one pin at a time they are **time-multiplexed**: the active receiver switches every **200 ms** via `IrReceiver.begin(newPin)`. The interval must exceed the NEC frame length (~68 ms); calling `begin()` mid-frame resets the decoder.

`IRremote` is forced onto **Timer3** (`#define IR_USE_AVR_TIMER3` before the include). Timer1/2 are used by MeMegaPi motor PWM; Timer4/5 are used by the Servo library bundled with MeMegaPi.

On each decoded NEC frame:
1. Both NEC checksum pairs (`addr ^ ~addr`, `cmd ^ ~cmd`) are validated — noise is discarded
2. `addr` identifies the shooter (XOR-fold of their `TANK_ID`)
3. `cmd` = damage = attacker's `ammoLevel`
4. If not immune: `health = max(0, health - damage)`; LEDs blink red
5. USB serial logs: `[HIT] IR<n> from 0x<addr> -<dmg> HP | Health: <hp>`
6. A **`hit` event** is sent over `Serial2` → RPi → MQTT → server
7. Server subtracts the damage from cached health and sends `command health <new>` back

```json
{
  "timestamp": 1234,
  "event": {
    "type": "hit",
    "data": {
      "tankId": "WHLYA1", "tankType": "wheely",
      "receiverId": "WHLYA1",
      "shooterAddr": 84,
      "damage": 1,
      "health": 99
    }
  }
}
```

`shooterAddr` is the 8-bit NEC address byte (XOR-fold of the attacker's `TANK_ID`). The server resolves this to a tank name by folding all known tank IDs.

### Death and respawn

When `health` reaches 0:
- `isDead = true` — motors stop, all inputs ignored
- USB serial prints: `[DEAD] Press START to respawn`
- Pressing **START** on the PS2 controller calls `respawn()`: health and ammo reset to 100

### Health LED matrices

Two **WS2812 3×3 LED matrices** (9 pixels each) driven by `MeRGBLed` (bundled with `MeMegaPi`). Both matrices always show the same colour.

| Pin | Object | LEDs |
|:---|:---|:---|
| A14 (= pin 68) | `led1` | 9 (3×3) |
| A13 (= pin 67) | `led2` | 9 (3×3) |

`startBlinkN(r, g, b, n)` runs N half-periods (N/2 full blinks) non-blocking; `startBlink(r, g, b)` is a shorthand for 2 blinks (n=4).

**Colour table:**

| Trigger | Colour | Blinks |
|:---|:---|:---|
| Startup — waiting for RPi | 🟡 Yellow | steady |
| RPi connected, health > 50 | 🟢 Green | steady |
| RPi connected, health 5–50 | 🟡 Yellow | steady |
| RPi connected, health < 5 / dead | 🔴 Red | steady |
| IR hit received | 🔴 Red | ×2 |
| Health command decreased (server) | 🔴 Red | ×2 |
| Health command increased (heal / respawn) | 🟢 Green | ×2 |
| `led 1` — treasure collected | 🟠 Gold (180,120,0) | ×3 |
| `led 2` — immunity granted | 🟣 Purple (120,0,180) | ×2 |
| `led 3` — win / bonus | ⬜ White (180,180,180) | ×4 |

After any blink sequence, LEDs automatically restore the current health colour.

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
      "ammoLevel": 3, "fireSpeed": 5, "immunable": false,
      "maxSpeed": 160, "minSpeed": 20
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
| `ammoLevel` | 1 | 1–10 | Ammo power / damage multiplier |
| `fireSpeed` | 1 | 1–10 | Minimum ms between shots |
| `immunable` | false | bool | Immune to damage |
| `maxSpeed` | 160 | 1–255 | Motor PWM cap |
| `minSpeed` | 20 | 0–255 | Motor PWM floor when moving |

### Remote commands

Send a `command` JSON to the tank's MQTT commands topic — the Raspberry Pi strips the timestamp and forwards `{"event":{...}}` to `Serial2`:

```json
{ "event": { "type": "command", "param": "health", "value": 80 } }
```

Valid `param` values: `health`, `ammo`, `ammoLevel`, `fireSpeed`, `immunable`, `maxSpeed`, `minSpeed`, `led`.

Values are clamped via `constrain()`. `immunable` is boolean (`0` = false, any non-zero = true).

The `led` param triggers a one-shot LED effect (see LED colour table above) and does not alter game state.

> **⚠️ Serial2 RX buffer — 64 bytes hard limit**
> The ATmega2560 hardware UART RX buffer is 64 bytes. Messages longer than 64 bytes are silently truncated when the main loop is briefly busy, corrupting the JSON. The RPi bridge always omits `timestamp` before writing to serial so every message stays within this limit. Do not add fields to serial-bound messages without checking the byte count.

---

## Assembly Notes

1. **Wheel orientation:** Install Mecanum wheels in the correct "X" pattern for proper omnidirectional translation.
2. **Cable management:** Use the FFC to allow flexible positioning of the Pi inside the shell.
