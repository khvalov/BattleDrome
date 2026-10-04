# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

BattleDrome is a hybrid physical-digital tank arena. Physical Mecanum-wheel robots fight in real space while a Node.js server governs gameplay state. IR blasters handle attacks; RFID tags on the field apply buffs/debuffs.

## Architecture

**Communication chain:**
```
PS2 Bluetooth remote → MegaPi (ATmega2560) ←UART→ Raspberry Pi Zero ←MQTT→ Central Server → Browser (WebSocket)
```

**Three layers:**
1. **Firmware** (`eqipement/tanks/wheely/megaPI/wheely.ino`) — Arduino/C++ on MegaPi. Reads PS2 joysticks, drives Mecanum wheels, sends telemetry JSON every 500 ms via `Serial2`, receives commands via `Serial2`.
2. **Tank bridge** (`eqipement/tanks/raspberry/server.js`) — Node.js on Raspberry Pi Zero. Forwards serial JSON → MQTT events topic; forwards MQTT commands topic → serial. Sends a heartbeat every 5 s. On WiFi loss, launches `wifi-connect` captive portal.
3. **Central server** (`server/index.js`) — Subscribes to MQTT wildcard, tracks online/offline state per tank (15 s timeout), serves the dashboard HTML, pushes live updates via WebSocket.

**MQTT topics:**
- `battledrome/tanks/{hostname}/events` — tank → server (telemetry, fire, rfid, heartbeat, errors)
- `battledrome/tanks/{hostname}/commands` — server → tank (param update commands)

**Message types:**

| `event.type` | Direction | Purpose |
|---|---|---|
| `system` | any | Generic system action (`connected`, `heartbeat`) |
| `error` | tank→server | Failure report |
| `telemetry` | tank→server | Tank stats snapshot every 500 ms (speed, health, ammo, …) |
| `fire` | tank→server | Immediate shot event (senderId, ammoLevel, remaining ammo) |
| `hit` | tank→server | IR hit received (receiverId, shooterAddr, damage, health after hit) |
| `rfid` | tank→server | RFID tag scanned (uid); server looks up action and replies with `command` |
| `command` | server→tank | Update a game-state variable on the Arduino |

## Firmware Development

Two tank firmwares live in this repo — both share the same game-state schema and Serial2 protocol.

| Tank | File | Drive | IR TX | IR RX | RFID SS |
|---|---|---|---|---|---|
| Wheely | `eqipement/tanks/wheely/megaPI/wheely.ino` | 4-wheel Mecanum | A12 | A11, A10 | A6 |
| Trinity | `eqipement/tanks/trinity/megaPi/trinity.ino` | 2-wheel differential | A12 | A11, A10 | A6 |

- Required libraries (both): `MeMegaPi`, `MePS2` (Makeblock), **`ArduinoJson`** by Benoit Blanchon, **`MFRC522`** by Miguel Balboa, **`IRremote`** by shirriff/z3t0/ArminJo, `SPI`, `Wire`, `SoftwareSerial` (built-in) — all via Library Manager
- Upload target: MegaPi board (ATmega2560)
- `Serial` (115200) = USB debug; `Serial2` (115200) = Raspberry Pi UART via flex cable
- Tank identity set via `TANK_ID` (≤6 chars) and `TANK_TYPE` (`"wheely"` or `"trinity"`) constants at top of each `.ino` file — must be set before uploading to a new board
- Square button fires: checks `ammo > 0` and `fireSpeed` ms cooldown; decrements ammo, sends `fire` event via Serial2
- RFID reader on RST_PIN=30, SS_PIN=A6 via SPI (MFRC522 library); sends `rfid` event with UID on card scan; 5 s debounce per UID

**IR combat system (NEC protocol):**

- **TX pin A12** (= ATmega2560 pin 66): software 38 kHz carrier via busy-wait (`markIR` / `spaceIR`)
- **RX pins A11 + A10** (= pins 65/64): dual receivers, time-multiplexed every **200 ms** via `IrReceiver.begin()` — gives both sensors coverage without running two IRremote instances. Must be > 68 ms (full NEC frame length); calling `IrReceiver.begin()` mid-frame resets the decoder.
- **IRremote timer: `#define IR_USE_AVR_TIMER3`** must appear before `#include <IRremote.h>`. ATmega2560 timer allocation on MegaPi:
  - Timer0 — Arduino core (`millis`/`micros`) — reserved
  - Timer1, Timer2 — MeMegaPi motor PWM (`TCCR1`/`TCCR2` in `setup()`) — claimed
  - Timer3 — **IRremote** ✓ free
  - Timer4, Timer5 — Servo library (bundled with MeMegaPi) — claimed
- **Frame format (32-bit NEC):** `addr | (~addr)<<8 | cmd<<16 | (~cmd)<<24`
  - `addr` = XOR-fold of all TANK_ID bytes — uniquely identifies the shooter
  - `cmd`  = `ammoLevel` (1–10) — damage points applied by the receiver
  - Both checksum bytes (`~addr`, `~cmd`) are validated; corrupt/noise frames are silently dropped
- `IrReceiver.stop()` is called before TX and `IrReceiver.start()` after, so the timer ISR does not capture the outgoing burst as an incoming signal
- On hit: firmware decrements health locally, sends `hit` event via Serial2 → RPi → MQTT; server computes authoritative new health and sends `command health <N>` back
- Health = 0 → `isDead = true`; motors stop; pressing START respawns (health/ammo reset to 100)

**Wheely-specific:**
- Motor port mapping: FL=PORT_12, FR=PORT_4, RL=PORT_9, RR=PORT_1
- WS2812 health LEDs on A14 (led1) and A13 (led2): yellow = waiting for RPi, green/yellow/red = health level; blinks red on hit, green on heal

**Trinity-specific:**
- Motor port mapping: L=PORT_1, R=PORT_4
- Health-based speed scaling: max PWM = 30 % at 0 HP → 100 % at 100 HP (`updateSpeedFromHealth()`)
- No LEDs (A13/A14 are not used)

**Game-state variables (set via `command` messages):**

| Variable | Wheely default | Trinity default | Range |
|---|---|---|---|
| `health` | 100 | 100 | 0–100 |
| `ammo` | 100 | 100 | 0–100 |
| `ammoLevel` | 1 | 3 | 1–10 |
| `fireSpeed` | 1 | 6 | 1–10 |
| `immunable` | false | false | bool |
| `maxSpeed` | 160 | 160 | 1–255 |
| `minSpeed` | 20 | 20 | 0–255 |

## Development Commands

```bash
# Central server (dashboard at http://localhost:8080)
cd server && npm install && npm start

# RPi bridge (run on the Pi itself)
npm install serialport @serialport/parser-readline mqtt
node eqipement/tanks/raspberry/server.js

# Send a command to a tank manually
mosquitto_pub -h broker.hivemq.com \
  -t "battledrome/tanks/{hostname}/commands" \
  -m '{"timestamp":0,"event":{"type":"command","param":"health","value":80}}'

# Monitor all tank traffic
mosquitto_sub -h broker.hivemq.com -t "battledrome/tanks/#"
```

There are no automated tests. Validate firmware changes by uploading to the MegaPi and watching `Serial` (115200) in the Arduino IDE Serial Monitor.

## Raspberry Pi Setup

**Critical UART fix** — without this, commands sent to the Arduino are silently garbled:

```
# /boot/config.txt
enable_uart=1
```

Then: `raspi-config → Interfacing Options → Serial` → disable login shell, enable hardware port. This routes GPIO14/15 to the stable PL011 UART (`/dev/ttyAMA0`) instead of the clock-drifting mini UART. `server.js` opens `/dev/serial0`, which follows this setting automatically.

Systemd service config and full setup steps are in [`eqipement/tanks/raspberry/README.md`](eqipement/tanks/raspberry/README.md). WiFi provisioning uses `wifi-connect` (checked via `nmcli`).

**RPi telemetry enrichment:** before forwarding `telemetry`, `fire`, or `rfid` events upstream to MQTT, the bridge injects `ip` (first non-loopback IPv4) and `hostname` (`os.hostname()`) into `event.data`. The Arduino never sends these.

**Ping/pong — Arduino↔RPi presence detection:** the Arduino sends `{"event":{"type":"system","action":"ping"}}` over Serial2 on startup and periodically. The RPi bridge replies with `pong` via serial only (not MQTT). The Arduino uses this to know the RPi is alive; LEDs show yellow until the first `pong` is received.

## Central Server

```bash
cd server
npm install   # deps: mqtt, ws
npm start     # dashboard at http://localhost:8080
```

### REST API

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/api/game` | Full game snapshot |
| `PATCH` | `/api/game` | Update `mode`, `timeLimit`, `scoreTarget` (only when not running) |
| `POST` | `/api/game/start` | Start round |
| `POST` | `/api/game/stop` | End round (reason: `manual`) |
| `POST` | `/api/game/reset` | Reset to idle |
| `PUT` | `/api/game/teams/:id` | Create/update team `{name, color, homeUid, tankIds[]}` |
| `DELETE` | `/api/game/teams/:id` | Remove team |
| `PUT` | `/api/game/bases/:uid` | Register CTF Solo home base `{tankId}` |
| `DELETE` | `/api/game/bases/:uid` | Remove base |
| `PUT` | `/api/game/treasures/:uid` | Register treasure `{label, points, action, actionValue}` |
| `DELETE` | `/api/game/treasures/:uid` | Remove treasure |
| `PUT` | `/api/game/checkpoints` | Replace race checkpoint list `{uids[]}` |
| `GET` | `/api/rfid` | All RFID actions |
| `POST` | `/api/rfid` | Add/update action `{uid, action, recipient, value}` |
| `DELETE` | `/api/rfid/:uid` | Remove action |
| `PATCH` | `/api/tanks/:id` | Set display name `{displayName}` (server-only, never sent to tank) |

Valid RFID `action` values: `ammo`, `health`, `speed`, `immune`, `maxspeed`, `minspeed`, `win`.  
Valid `recipient` values: `tank`, `others`, `teammate`, `all`.

### WebSocket messages (server → browser)

| `type` | Payload | When |
|---|---|---|
| `update` | `{tanks[]}` | Any tank event or offline detection |
| `log` | `{tankId, receivedAt, payload}` | Every MQTT message received |
| `rfid_actions` | `{actions}` | RFID table changes |
| `game_update` | `{game}` | Game state changes |
| `game_end` | `{game, reason}` | Round ends |
| `game_tick` | `{timeRemaining}` | Each second while running |
| `win` | `{tankId, recipient}` | `win` RFID action triggered |

On connect, the server immediately pushes `update`, `rfid_actions`, and `game_update`.

## Sending commands to a tank

```bash
mosquitto_pub -h broker.hivemq.com \
  -t "battledrome/tanks/{hostname}/commands" \
  -m '{"timestamp":0,"event":{"type":"command","param":"health","value":80}}'
```

Valid `param` values: `health`, `ammo`, `ammoLevel`, `fireSpeed`, `immunable`, `maxSpeed`, `minSpeed`.

## Key Constants

- MQTT broker: `broker.hivemq.com:1883` (public, no auth — replace for production)
- Heartbeat interval: 5 s; offline timeout: 15 s
- Serial baud (both ends): 115200
- RPi HTTP health port: 3000 | Dashboard port: 8080

## ⚠️ Hardware Limitation — Serial2 RX Buffer (64 bytes)

The ATmega2560's hardware UART RX buffer for `Serial2` is **64 bytes**. When the Arduino main loop is briefly busy (sending telemetry, SPI communication with RFID/PS2), incoming bytes accumulate in this buffer. Any message longer than 64 bytes that arrives during a busy window is silently truncated, producing a JSON parse error.

**Rule:** every message written by the RPi bridge to Serial2 **must be ≤ 64 bytes**.

The RPi bridge (`server.js`) enforces this by sending only `{"event":{...}}` — omitting the `timestamp` field — before writing to serial. The Arduino never reads `timestamp` from incoming messages, so no functional data is lost.

Measured sizes for current message types (bytes including `\r\n`):

| Message | Bytes |
|---|---|
| `pong` | 54 |
| `connected` | 59 |
| `command` (longest param `fireSpeed`) | 59 |

Do **not** add fields to serial-bound messages without re-checking the byte count. The MQTT payloads (tank → server direction) are not affected — those travel Arduino TX → RPi RX and can be any length.
