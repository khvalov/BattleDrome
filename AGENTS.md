# AGENTS.md

Read `CLAUDE.md` first — it contains the full architecture, pin mappings, protocol details, and game-state schema.

## Quick Reference

```bash
# Central server
cd server && npm install && npm start   # dashboard at http://localhost:8080

# Send a command to a tank
mosquitto_pub -h broker.hivemq.com \
  -t "battledrome/tanks/{hostname}/commands" \
  -m '{"timestamp":0,"event":{"type":"command","param":"health","value":80}}'
```

No test suite, linter, or CI exists in this repo. Verification is manual (physical hardware + MQTT broker).

## Critical Constraint: Serial2 RX Buffer (64 bytes)

Every JSON message sent from the RPi bridge to the Arduino **must be ≤ 64 bytes** (including `\r\n`). The RPi bridge strips `timestamp` before writing to serial to stay under this limit. Exceeding it causes silent truncation and JSON parse failures on the Arduino.

**Always re-check byte count** when adding or changing fields in server→tank messages.

## Repo Layout

| Path | What |
|---|---|
| `server/index.js` | Central server (MQTT + WebSocket + HTTP dashboard + RFID action table) |
| `eqipement/tanks/raspberry/server.js` | RPi bridge (serial ↔ MQTT) |
| `eqipement/tanks/wheely/megaPI/wheely.ino` | Wheely firmware (4-wheel Mecanum) |
| `eqipement/tanks/trinity/megaPi/trinity.ino` | Trinity firmware (2-wheel differential) |

Note the typo: the directory is `eqipement`, not `equipment`.

## Firmware Gotchas

- `#define IR_USE_AVR_TIMER3` **must** appear before `#include <IRremote.h>` — Timer1/2 are claimed by MeMegaPi motors, Timer4/5 by Servo.
- Dual IR receivers on A11/A10 are time-multiplexed every 200 ms via `IrReceiver.begin()` — the interval must exceed 68 ms (NEC frame length).
- `IrReceiver.stop()` before TX, `IrReceiver.start()` after — prevents self-reception.
- RFID uses SPI with SS=A6, RST=30; 5 s debounce per UID.
- Both firmwares share the same Serial2 JSON protocol and game-state variables; keep them in sync when changing the protocol.
- **Drive models differ:** Wheely uses Mecanum (LY forward/back + RX strafe + LX turn). Trinity uses tank-stick: LY = left track, RY = right track (no mixing).
- Speed and health are fully server-controlled — the firmware applies `maxSpeed`/`health` values received via commands with no local scaling or derivation.
