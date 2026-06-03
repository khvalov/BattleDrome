# Project Trinity

A differential-drive combat robot powered by MegaPi and Raspberry Pi.

<img width="1457" height="1080" alt="ChatGPT Image May 7, 2026, 10_14_34 AM" src="https://github.com/user-attachments/assets/a631860f-29b6-40e5-8e4d-54ea1f697cde" />

---

## Core Architecture

Trinity is a two-wheel differential-drive platform based on the **MegaPi** (ATmega2560) for low-level motor control and a **Raspberry Pi Zero** as the network bridge.

- **Drive System:** 2WD differential (skid-steer — forward, reverse, and in-place pivot)
- **Speed/health:** Fully server-controlled — firmware applies `maxSpeed` and `health` values received via commands with no local scaling

---

## Bill of Materials

### Electronic Components

| Qty | Component | Description |
|:---|:---|:---|
| 1 | **MegaPi** | Main microcontroller (Arduino Mega 2560 compatible) |
| 1 | **MegaPi** | Main microcontroller; built-in encoder motor drivers (SLOT1–SLOT4) |
| 1 | **Bluetooth Module** | Wireless remote control (PS2 protocol) |
| 3 | **Encoder DC Motors** | Left track (SLOT1), right track (SLOT4), aux (SLOT2) |
| 1 | **WS2812 4×4 LED matrix** | Health / status indicator (MeRGBLed, 16 LEDs, pin A9) |
| 1 | **Raspberry Pi Zero** | Network bridge to MQTT |
| 1 | **MFRC522 RFID reader** | SPI RFID card reader (RST=pin 30, SS=A6) |
| 1 | **IR LED** | Firing transmitter (A12 = pin 66) |
| 2 | **IR receivers** | Hit detection (A11 = pin 65, A10 = pin 64) |

### Building Components

- **Chassis:** Custom 2-wheel differential platform
- **Power:** 1x 6-AA Battery Holder (9V DC)
- **Hardware:** Spacers, standoffs, screws, and nuts

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

**File:** `megaPi/trinity.ino`

**Required libraries** (install via Arduino Library Manager):
- `MeMegaPi` — Makeblock motor drivers
- `MePS2` — PS2 Bluetooth controller
- `ArduinoJson` by Benoit Blanchon
- `MFRC522` by Miguel Balboa — RFID reader
- `IRremote` by shirriff / z3t0 / ArminJo — IR receive decoding
- `SPI`, `Wire`, `SoftwareSerial` — built-in Arduino libraries

### Controls

| Input | Axis | Action |
|:---|:---|:---|
| Left stick Y | `LY` | Left track forward / back |
| Right stick Y | `RY` | Right track forward / back |

Each stick directly controls its corresponding track. Push both sticks up to go forward, both down to reverse, opposite directions to pivot in place.

### Motor slot mapping

Motors are driven by the MegaPi's onboard encoder slots (`MeEncoderOnBoard`). The encoder ISRs are attached at startup even though we use `setMotorPwm()` (open-loop PWM) rather than PID position control.

| Slot | Motor | Control | Direction constant |
|:---|:---|:---|:---|
| SLOT1 | Left track | Left stick Y (`MeJOYSTICK_LY`) | `SIGN_L = +1` |
| SLOT4 | Right track | Right stick Y (`MeJOYSTICK_RY`) | `SIGN_R = -1` |
| SLOT2 | Aux motor | D-pad RIGHT = fwd, LEFT = rev | `SIGN_AUX = -1` |

Flip the corresponding `SIGN_*` constant to `+1`/`-1` if a motor runs the wrong direction. Aux speed is fixed at `AUX_SPEED = 30` and is not affected by `maxSpeed`.

Deadzone: **20 units** (joystick values below this threshold are treated as zero)

### Speed limits

Both limits are runtime-mutable via `command` messages from the server. The firmware applies these values directly with no local health-based scaling.

| Variable | Default | Range | Description |
|:---|:---|:---|:---|
| `maxSpeed` | 160 | 1–255 | Motor PWM ceiling |
| `minSpeed` | 20 | 0–255 | Motor PWM floor when moving |

### RFID reader

| Constant | Value | Description |
|:---|:---|:---|
| `RST_PIN` | `30` | MFRC522 reset pin |
| `SS_PIN` | `A6` | MFRC522 SPI slave-select pin (= pin 60 on ATmega2560) |
| `RFID_COOLDOWN_MS` | `5000` | Min ms before the same UID triggers again |

Uses hardware SPI (MOSI=51, MISO=50, SCK=52 on ATmega2560). When a card is detected the firmware reads the UID, applies the 5 s debounce, and sends an `rfid` event over `Serial2`:

```json
{ "timestamp": 1234, "event": { "type": "rfid", "data": { "tankId": "TRNA1", "tankType": "trinity", "uid": "A1B2C3D4" } } }
```

The Raspberry Pi enriches the event with `ip` and `hostname` before publishing to MQTT. The central server then looks up the UID in its `RFID_ACTIONS` table and sends a `command` back if a match is found.

### Tank settings

Defined as constants at the top of `trinity.ino`:

| Constant | Value | Description |
|:---|:---|:---|
| `TANK_ID` | `"TRNA1"` | Unique identifier, up to 6 characters |
| `TANK_TYPE` | `"trinity"` | Tank model name |
| `IR_TX_PIN` | `A12` | IR LED transmit pin (= pin 66 on ATmega2560) |
| `IR_RX_PIN_1` | `A11` | First IR receiver (= pin 65) |
| `IR_RX_PIN_2` | `A10` | Second IR receiver (= pin 64) |

### IR combat system

Trinity uses the **NEC protocol** for both firing and hit detection.

#### Firing (Square button)

Pressing the **Square** button triggers a shot if:
- `ammo > 0`
- Time since last shot ≥ `fireSpeed` ms

On each shot the firmware:
1. Pauses the IR receiver ISR (`IrReceiver.stop()`)
2. Transmits a **NEC IR frame** on `IR_TX_PIN` (38 kHz software carrier)
   - Address byte: XOR-fold of all `TANK_ID` ASCII bytes — uniquely identifies the shooter
   - Command byte: current `ammoLevel` (damage level, 1–10)
3. Resumes the IR receiver ISR (`IrReceiver.start()`)
4. Decrements `ammo` by 1
5. Immediately sends a `fire` event over `Serial2`

> **Note:** Button detection uses rising-edge logic (`squarePrevious` flag) — one shot per press.

#### Receiving hits (dual IR receivers)

Two IR receivers cover both sides of the tank. Because `IRremote` can only sample one pin at a time, they are **time-multiplexed**: the active receiver switches every **50 ms** via `IrReceiver.begin(newPin)`.

On each decoded NEC frame:
1. Both NEC checksum pairs (`addr ^ ~addr`, `cmd ^ ~cmd`) are validated — noise is discarded
2. `addr` identifies the shooter (XOR-fold of their `TANK_ID`)
3. `cmd` = damage = attacker's `ammoLevel`
4. If not immune: `health = max(0, health - damage)`, speed cap recalculated
5. USB serial logs: `[HIT] IR<n> from 0x<addr> -<dmg> HP | Health: <hp>`
6. A **`hit` event** is sent over `Serial2` to the Raspberry Pi → MQTT → server
7. Server subtracts the damage from the receiver's health and sends a `command health <new>` back

#### NEC frame layout

```
 bits 0–7   : addr     (XOR-fold of shooter's TANK_ID)
 bits 8–15  : ~addr    (checksum)
 bits 16–23 : cmd      (ammoLevel = damage)
 bits 24–31 : ~cmd     (checksum)
```

### Health LED matrix

One **WS2812 4×4 LED matrix** (16 pixels) driven by `MeRGBLed` on pin **A9**. All 16 LEDs always show the same colour via `matrix.setColor(0, r, g, b)` (index 0 = broadcast to all pixels).

`startBlinkN(r, g, b, n)` runs N half-periods (N/2 full blinks) non-blocking; `startBlink(r, g, b)` = 2 blinks (n=4).

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

After any blink sequence the matrix automatically restores the current health colour.

### Death and respawn

When `health` reaches 0:
- `isDead = true` — motors stop, all inputs ignored
- USB serial prints: `[DEAD] Drive to home base to respawn`
- Server sends `command health 100` when the tank drives over its home base RFID → clears `isDead` automatically

### Telemetry

Every **500 ms** the firmware sends a `telemetry` packet over `Serial2`. The Raspberry Pi enriches it with `ip` and `hostname` before publishing to MQTT:

```json
{
  "timestamp": 1234,
  "event": {
    "type": "telemetry",
    "data": {
      "tankId": "TRNA1", "tankType": "trinity",
      "speed": 80, "health": 75, "ammo": 97,
      "ammoLevel": 3, "fireSpeed": 5, "immunable": false,
      "maxSpeed": 160, "minSpeed": 20
    }
  }
}
```

`speed` is the average absolute PWM of both motors.

### Fire event

Sent immediately on each shot:

```json
{
  "timestamp": 1234,
  "event": {
    "type": "fire",
    "data": { "tankId": "TRNA1", "tankType": "trinity", "senderId": "TRNA1", "ammoLevel": 3, "ammo": 99 }
  }
}
```

### Hit event

Sent immediately when an incoming IR frame is decoded and damage applied:

```json
{
  "timestamp": 1234,
  "event": {
    "type": "hit",
    "data": {
      "tankId": "TRNA1", "tankType": "trinity",
      "receiverId": "TRNA1",
      "shooterAddr": 84,
      "damage": 3,
      "health": 97
    }
  }
}
```

`shooterAddr` is the 8-bit address byte from the NEC frame (XOR-fold of the attacker's `TANK_ID`). The central server resolves this to a tank name by folding all known `TANK_ID` values and finding a match.

### Game-state variables

Held in memory on the Arduino. Updated at runtime via `command` messages received on `Serial2`.

| Variable | Default | Range | Description |
|:---|:---|:---|:---|
| `health` | 100 | 0–100 | Tank HP |
| `ammo` | 100 | 0–100 | Remaining ammunition |
| `ammoLevel` | 3 | 1–10 | Ammo power / damage per shot |
| `fireSpeed` | 5 | 1–10 | Minimum ms between shots |
| `immunable` | false | bool | Immune to IR damage |
| `maxSpeed` | 160 | 1–255 | Motor PWM ceiling |
| `minSpeed` | 20 | 0–255 | Motor PWM floor when moving |

### Remote commands

Send a `command` JSON to the tank's MQTT commands topic — the Raspberry Pi strips the timestamp and forwards `{"event":{...}}` to `Serial2`:

```json
{ "event": { "type": "command", "param": "health", "value": 80 } }
```

Valid `param` values: `health`, `ammo`, `ammoLevel`, `fireSpeed`, `immunable`, `maxSpeed`, `minSpeed`, `led`.

Values are clamped via `constrain()`. `immunable` is boolean (`0` = false, any non-zero = true).

`health` command: if the tank is dead (`isDead = true`) and `value > 0`, `isDead` is cleared automatically — this is the server-triggered respawn path.

`led` param: triggers a one-shot LED blink effect (see LED matrix table above). Does not alter any game-state variable.

> **⚠️ Serial2 RX buffer — 64 bytes hard limit**  
> The ATmega2560 hardware UART RX buffer is 64 bytes. Messages longer than 64 bytes are silently truncated when the main loop is briefly busy, corrupting the JSON. The RPi bridge always omits `timestamp` before writing to serial so every message stays within this limit. Do not add fields to serial-bound messages without checking the byte count.

---

## RPi connectivity

The firmware pings the Raspberry Pi every **5 s** with:

```json
{"event":{"type":"system","action":"ping"}}
```

The RPi bridge replies with `pong`. On the first `pong` / `connected` / `heartbeat` received over `Serial2`, the `rpiConnected` flag is set and the USB serial prints `[SYS] RPi alive — telemetry active`. Until then, Serial2 events are still transmitted (the RPi may already be listening).

---

## Assembly Notes

1. **Motor direction:** If either motor runs backward, flip its `SIGN_L` or `SIGN_R` constant in the firmware (`+1` / `-1`).
2. **IR receiver placement:** Mount one receiver on each side of the hull for full 360° hit coverage.
3. **IR LED aim:** Point the transmitter forward (or at the angle most useful in your arena layout).
4. **Cable management:** Use the FFC to allow flexible positioning of the Pi inside the shell.
