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
| `rfid` | tank→server | RFID tag scanned (uid); server looks up action and replies with `command` |
| `command` | server→tank | Update a game-state variable on the Arduino |

## Firmware Development

- Open `eqipement/tanks/wheely/megaPI/wheely.ino` in Arduino IDE or PlatformIO
- Required libraries: `MeMegaPi`, `MePS2` (Makeblock), **`ArduinoJson`** by Benoit Blanchon, **`MFRC522`** by Miguel Balboa, `SPI` (built-in) — all via Library Manager
- Upload target: MegaPi board (ATmega2560)
- `Serial` (115200) = USB debug; `Serial2` (115200) = Raspberry Pi UART via flex cable
- Motor port mapping: FL=PORT_12, FR=PORT_4, RL=PORT_9, RR=PORT_1
- Tank identity set via `TANK_ID` (≤6 chars) and `TANK_TYPE` constants at top of file
- IR transmit pin: `IR_TX_PIN` constant (default 3) — replace `fireIR()` body with your IR library call
- Square button fires: checks `ammo > 0` and `fireSpeed` ms cooldown; decrements ammo, sends `fire` event
- RFID reader on RST_PIN=30, SS_PIN=22 via SPI (MFRC522 library); sends `rfid` event with UID on card scan; 3 s debounce per UID

**Game-state variables (set via `command` messages):**

| Variable | Default | Range |
|---|---|---|
| `health` | 100 | 0–100 |
| `ammo` | 100 | 0–100 |
| `ammoLevel` | 3 | 1–10 |
| `fireSpeed` | 5 | 1–10 |
| `immunable` | false | bool |

## Raspberry Pi Setup

```bash
# Enable UART: raspi-config → Interfacing Options → Serial
npm install serialport @serialport/parser-readline mqtt
node server.js
```

Systemd service config is in `eqipement/tanks/raspberry/README.md`. WiFi provisioning uses `wifi-connect` (checked via `nmcli`).

## Central Server

```bash
cd server
npm install   # deps: mqtt, ws
npm start     # dashboard at http://localhost:8080
```

## Sending commands to a tank

```bash
mosquitto_pub -h broker.hivemq.com \
  -t "battledrome/tanks/{hostname}/commands" \
  -m '{"timestamp":0,"event":{"type":"command","param":"health","value":80}}'
```

Valid `param` values: `health`, `ammo`, `ammoLevel`, `fireSpeed`, `immunable`.

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
