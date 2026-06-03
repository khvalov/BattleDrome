# Project Rocky

A tracked combat robot with an auxiliary arm motor, powered by MegaPi and Raspberry Pi.

---

## Core Architecture

Rocky is a two-track differential-drive platform with an auxiliary encoder motor for an arm or turret mechanism. It uses a **MegaPi** (ATmega2560) for low-level motor and LED control, and a **Raspberry Pi Zero** as the network bridge.

- **Drive System:** 2-track differential (skid-steer — forward, reverse, in-place pivot)
- **Auxiliary motor:** Separate encoder slot controlled by D-pad UP / DOWN

---

## Bill of Materials

### Electronic Components

| Qty | Component | Description |
|:---|:---|:---|
| 1 | **MegaPi** | Main microcontroller; built-in encoder motor drivers (SLOT1–SLOT4) |
| 1 | **Bluetooth Module** | Wireless remote control (PS2 protocol) |
| 3 | **Encoder DC Motors** | Left track (SLOT1), right track (SLOT4), aux (SLOT2) |
| 1 | **WS2812 4×4 LED matrix** | Health / status indicator (MeRGBLed, 16 LEDs, pin A9) |
| 1 | **Raspberry Pi Zero** | Network bridge to MQTT |
| 1 | **MFRC522 RFID reader** | SPI RFID card reader (RST=pin 30, SS=A6) |
| 1 | **IR LED** | Firing transmitter (A12 = pin 66) |
| 2 | **IR receivers** | Hit detection (A11 = pin 65, A10 = pin 64) |

### Building Components

- **Chassis:** Tracked platform
- **Power:** 1× 6-AA Battery Holder (9V DC)
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

**File:** `megaPi/rocky.ino`

**Required libraries** (install via Arduino Library Manager):
- `MeMegaPi` — Makeblock encoder motor drivers; also bundles `MeRGBLed` for WS2812 LEDs
- `MePS2` — PS2 Bluetooth controller
- `ArduinoJson` by Benoit Blanchon
- `MFRC522` by Miguel Balboa — RFID reader
- `IRremote` by shirriff / z3t0 / ArminJo — IR receive decoding
- `SPI`, `Wire`, `SoftwareSerial` — built-in Arduino libraries

### Controls

| Input | Action |
|:---|:---|
| Left stick Y | Left track forward / back |
| Right stick Y | Right track forward / back |
| D-pad UP | Aux motor forward at `AUX_SPEED` |
| D-pad DOWN | Aux motor backward at `AUX_SPEED` |
| Square button | Fire IR shot |

Each track stick directly drives its track. Push both sticks up to go forward, both down to reverse, opposite directions to pivot in place.

### Motor slot mapping

| Slot | Motor | Control | Direction constant |
|:---|:---|:---|:---|
| SLOT1 | Left track | Left stick Y (`MeJOYSTICK_LY`) | `SIGN_L = +1` |
| SLOT4 | Right track | Right stick Y (`MeJOYSTICK_RY`) | `SIGN_R = +1` |
| SLOT2 | Aux motor | D-pad UP / DOWN | `SIGN_AUX = +1` |

> **SLOT3 avoided:** IRremote claims Timer4 on ATmega2560, which conflicts with SLOT3's PWM pin (pin 6). SLOT2 (Timer2) is free — `TCCR2` in `setup()` already configures it for fast PWM.

Flip the corresponding `SIGN_*` constant to `+1`/`-1` if a motor runs the wrong direction. Aux speed is fixed at `AUX_SPEED = 50` and is not affected by `maxSpeed`.

### Speed limits

| Variable | Default | Range | Description |
|:---|:---|:---|:---|
| `maxSpeed` | 160 | 1–255 | Track motor PWM ceiling |
| `minSpeed` | 20 | 0–255 | Track motor PWM floor when moving |

### RFID reader

| Constant | Value | Description |
|:---|:---|:---|
| `RST_PIN` | `30` | MFRC522 reset pin |
| `SS_PIN` | `A6` | MFRC522 SPI slave-select (= pin 60 on ATmega2560) |
| `RFID_COOLDOWN_MS` | `5000` | Min ms before the same UID triggers again |

Uses hardware SPI (MOSI=51, MISO=50, SCK=52). When a card is detected the firmware reads the UID, applies the 5 s debounce, and sends an `rfid` event over `Serial2`:

```json
{ "timestamp": 1234, "event": { "type": "rfid", "data": { "tankId": "ROCKY1", "tankType": "rocky", "uid": "A1B2C3D4" } } }
```

### Tank settings

| Constant | Value | Description |
|:---|:---|:---|
| `TANK_ID` | `"ROCKY1"` | Unique identifier, up to 6 characters |
| `TANK_TYPE` | `"rocky"` | Tank model name |
| `IR_PIN` | `A12` | IR LED transmit pin (= pin 66 on ATmega2560) |
| `IR_RX_PIN_1` | `A11` | First IR receiver (= pin 65) |
| `IR_RX_PIN_2` | `A10` | Second IR receiver (= pin 64) |

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

### Firing (Square button)

Pressing the **Square** button triggers a shot if:
- `ammo > 0`
- Time since last shot ≥ `fireSpeed` ms

On each shot:
1. `IrReceiver.stop()` — pauses the ISR so it doesn't capture the outgoing burst
2. Transmits a **NEC IR frame** (address = XOR-fold of `TANK_ID`, command = `ammoLevel`)
3. `IrReceiver.start()` — resumes on the active pin
4. Decrements `ammo` by 1
5. Sends a `fire` event over `Serial2`

### Receiving hits (dual IR receivers)

Two IR receivers time-multiplexed every **200 ms** via `IrReceiver.begin(newPin)`. On each valid NEC frame:
1. Both checksum pairs validated — noise discarded
2. `addr` identifies the shooter; `cmd` = damage = attacker's `ammoLevel`
3. If not immune and not dead: `health -= damage`; LED blinks red ×2
4. Sends a `hit` event over `Serial2`
5. If health reaches 0: `isDead = true`, motors stop

### Death and respawn

When `health` reaches 0:
- `isDead = true` — motors stop, all inputs ignored
- USB serial prints: `[DEAD] Drive to home base to respawn`
- Server sends `command health 100` when the tank drives over its home base RFID → clears `isDead` automatically

### Telemetry

Every **500 ms** the firmware sends a `telemetry` packet over `Serial2`:

```json
{
  "timestamp": 1234,
  "event": {
    "type": "telemetry",
    "data": {
      "tankId": "ROCKY1", "tankType": "rocky",
      "speed": 80, "health": 75, "ammo": 8,
      "ammoLevel": 2, "fireSpeed": 1, "immunable": false,
      "maxSpeed": 160, "minSpeed": 20
    }
  }
}
```

`speed` is the average absolute PWM of the two tracks.

### Game-state variables

| Variable | Default | Range | Description |
|:---|:---|:---|:---|
| `health` | 100 | 0–100 | Tank HP |
| `ammo` | 10 | 0–100 | Remaining ammunition |
| `ammoLevel` | 2 | 1–10 | Damage per shot |
| `fireSpeed` | 1 | 1–10 | Minimum ms between shots |
| `immunable` | false | bool | Immune to IR damage |
| `maxSpeed` | 160 | 1–255 | Track motor PWM ceiling |
| `minSpeed` | 20 | 0–255 | Track motor PWM floor when moving |

### Remote commands

```json
{ "event": { "type": "command", "param": "health", "value": 80 } }
```

Valid `param` values: `health`, `ammo`, `ammoLevel`, `fireSpeed`, `immunable`, `maxSpeed`, `minSpeed`, `led`.

`health` command: if the tank is dead and `value > 0`, `isDead` is cleared (server-triggered respawn).

`led` param: one-shot LED blink effect — does not alter game state:

| `value` | Effect |
|:---|:---|
| `1` | Treasure collected — gold blink ×3 |
| `2` | Immunity granted — purple blink ×2 |
| `3` | Win / bonus — white blink ×4 |

> **⚠️ Serial2 RX buffer — 64 bytes hard limit.** The RPi bridge omits `timestamp` before writing to serial. Do not add fields without checking the byte count.

---

## RPi connectivity

The firmware pings the Raspberry Pi every **5 s**. On the first `pong` / `connected` / `heartbeat` received over `Serial2`, the LED switches from yellow (waiting) to health-based colour.

---

## Assembly Notes

1. **Track direction:** If either track runs backward, flip its `SIGN_L` or `SIGN_R` constant (`+1`/`-1`).
2. **Aux motor direction:** Flip `SIGN_AUX` if the arm/turret moves the wrong way on D-pad UP.
3. **SLOT3 not used:** IRremote occupies Timer4 (SLOT3's PWM timer) — always use SLOT2 for the aux motor.
4. **IR receiver placement:** Mount one receiver on each side of the hull for 360° hit coverage.
5. **Cable management:** Use the FFC for flexible Pi positioning inside the shell.
