# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

BattleDrome is a hybrid physical-digital tank arena. Physical Mecanum-wheel robots fight in real space while a Node.js server governs gameplay state (HP, ammo, armor, speed). IR blasters handle in-game attacks; RFID tags on the field apply buffs/debuffs.

## Architecture

**Communication chain:**
```
PS2 Bluetooth remote → MegaPi (ATmega2560) → UART (115200 baud) → Raspberry Pi Zero → MQTT → Central Server
```

**Three layers:**
1. **Firmware** (`eqipement/tanks/wheely/megaPI/wheely.ino`) — Arduino/C++ on MegaPi. Reads PS2 joysticks, drives Mecanum wheels via PWM (0–255). Mecanum kinematics: LY=forward, LX=strafe, RX=rotate.
2. **Tank bridge** (`eqipement/tanks/raspberry/server.js`) — Node.js on Raspberry Pi Zero. Bridges MegaPi serial ↔ MQTT broker (`broker.hivemq.com:1883`, topic `battledrome/events`). Exposes HTTP health check on port 3000. On WiFi loss, launches `wifi-connect` captive portal.
3. **Central server** (`server/`) — **Not yet implemented.** Will subscribe to MQTT, run game logic, and issue commands back to tanks.

**Message format** (all hardware communication):
```json
{ "timestamp": 1234567890, "event": { "type": "system|error", "action": "...", "value": "..." } }
```

## Raspberry Pi Setup

```bash
# On Raspberry Pi Zero
# Enable UART via raspi-config → Interfacing Options → Serial
npm install serialport @serialport/parser-readline mqtt
node server.js
```

Systemd service config is documented in `eqipement/tanks/raspberry/README.md`. WiFi provisioning uses `wifi-connect` (checked via `nmcli`).

## Firmware Development

- Open `eqipement/tanks/wheely/megaPI/wheely.ino` in Arduino IDE or PlatformIO
- Required libraries: `MeMegaPi`, `MePS2` (Makeblock libraries)
- Upload target: MegaPi board (ATmega2560)
- Motor port mapping: FL=PORT_12, FR=PORT_4, RL=PORT_9, RR=PORT_1
- Deadzone: 20 units; speed cap: 255 PWM

## Key Constants

- Tank defaults: MaxHP=100, MaxArmor=100, MaxAmmo=100, DamagePerShot=10, Speed=0–255
- MQTT broker: `broker.hivemq.com:1883` (public, no auth — replace for production)
- Serial baud: 115200
- HTTP health port: 3000
