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
- `battledrome/tanks/{hostname}/events` — tank → server (telemetry, heartbeat, errors)
- `battledrome/tanks/{hostname}/commands` — server → tank (param update commands)

**Message types:**

| `event.type` | Direction | Purpose |
|---|---|---|
| `system` | any | Generic system action (`connected`, `heartbeat`) |
| `error` | tank→server | Failure report |
| `telemetry` | tank→server | Tank stats snapshot (speed, health, ammo, …) |
| `command` | server→tank | Update a game-state variable on the Arduino |

## Firmware Development

- Open `eqipement/tanks/wheely/megaPI/wheely.ino` in Arduino IDE or PlatformIO
- Required libraries: `MeMegaPi`, `MePS2` (Makeblock), **`ArduinoJson`** (Library Manager)
- Upload target: MegaPi board (ATmega2560)
- `Serial` (115200) = USB debug; `Serial2` (115200) = Raspberry Pi UART via flex cable
- Motor port mapping: FL=PORT_12, FR=PORT_4, RL=PORT_9, RR=PORT_1

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
