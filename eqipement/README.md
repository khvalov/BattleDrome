# JSON Interaction Specification

All hardware communication — Arduino ↔ Raspberry Pi (UART) and Raspberry Pi ↔ Server (MQTT) — uses newline-delimited JSON (`\r\n`).

---

## Message Structure

```json
{ "timestamp": 1234567890, "event": { "type": "<type>", ... } }
```

`timestamp` — Unix epoch in seconds (`millis() / 1000` on Arduino, `Math.floor(Date.now() / 1000)` on Node.js).

---

## Event Types

### `system` — lifecycle events
```json
{ "timestamp": 1234567890, "event": { "type": "system", "action": "connected",  "value": 1 } }
{ "timestamp": 1234567890, "event": { "type": "system", "action": "heartbeat",  "value": 1 } }
```

### `error` — failure report
```json
{ "timestamp": 1234567890, "event": { "type": "error" } }
```

### `telemetry` — tank → server, every 500 ms
Full snapshot of physical and game state, emitted by the Arduino and enriched by the Raspberry Pi (adds `ip`, `hostname`) before publishing to MQTT.

```json
{
  "timestamp": 1234567890,
  "event": {
    "type": "telemetry",
    "data": {
      "tankId":    "WHLYA1",
      "tankType":  "wheely",
      "ip":        "192.168.1.42",
      "hostname":  "wheely-pi",
      "speed":     127,
      "health":    100,
      "ammo":      99,
      "ammoLevel": 3,
      "fireSpeed": 5,
      "immunable": false
    }
  }
}
```

| Field | Source | Type | Range | Description |
|---|---|---|---|---|
| `tankId` | Arduino | string | 6 chars | Unique tank identifier |
| `tankType` | Arduino | string | — | Tank model (`wheely`, …) |
| `ip` | RPi | string | — | RPi local IP address |
| `hostname` | RPi | string | — | RPi hostname |
| `speed` | Arduino | int | 0–255 | Average absolute PWM across all 4 motors |
| `health` | Arduino | int | 0–100 | Tank HP |
| `ammo` | Arduino | int | 0–100 | Remaining ammunition |
| `ammoLevel` | Arduino | int | 1–10 | Ammo power level |
| `fireSpeed` | Arduino | int | 1–10 | Minimum ms between shots |
| `immunable` | Arduino | bool | — | Whether the tank is immune to damage |

### `fire` — tank → server, on each shot
Sent immediately when the square button fires. The central server uses this to update the ammo counter without waiting for the next 500 ms telemetry tick.

```json
{
  "timestamp": 1234567890,
  "event": {
    "type": "fire",
    "data": {
      "tankId":    "WHLYA1",
      "tankType":  "wheely",
      "senderId":  "WHLYA1",
      "ammoLevel": 3,
      "ammo":      99,
      "ip":        "192.168.1.42",
      "hostname":  "wheely-pi"
    }
  }
}
```

| Field | Description |
|---|---|
| `senderId` | Attacker's `tankId` — echoed into IR packet for damage resolution |
| `ammoLevel` | Damage multiplier carried in the IR packet |
| `ammo` | Remaining rounds **after** this shot |

### `rfid` — tank → server, on each card scan
Sent when the MFRC522 reader detects a new card. Same UID is suppressed for 3 s (debounce). The server looks up the UID in its action table and, if a match is found, publishes a `command` back to the tank.

```json
{
  "timestamp": 1234567890,
  "event": {
    "type": "rfid",
    "data": {
      "tankId":   "WHLYA1",
      "tankType": "wheely",
      "uid":      "A1B2C3D4",
      "ip":       "192.168.1.42",
      "hostname": "wheely-pi"
    }
  }
}
```

| Field | Description |
|---|---|
| `uid` | Card UID as uppercase hex string (4–7 bytes, no spaces) |

### `command` — server → tank, via MQTT commands topic
Updates a single game-state variable on the Arduino. The Raspberry Pi subscribes to the commands topic and forwards matching messages to Arduino via UART.

```json
{ "timestamp": 0, "event": { "type": "command", "param": "health", "value": 80 } }
```

| `param` | Type | Range | Arduino behaviour |
|---|---|---|---|
| `health` | int | 0–100 | `constrain(value, 0, 100)` |
| `ammo` | int | 0–100 | `constrain(value, 0, 100)` |
| `ammoLevel` | int | 1–10 | `constrain(value, 1, 10)` |
| `fireSpeed` | int | 1–10 | `constrain(value, 1, 10)` |
| `immunable` | int | 0 / 1 | `value != 0` → bool |

---

## MQTT Topics

| Topic | Direction | Carries |
|---|---|---|
| `battledrome/tanks/{hostname}/events` | tank → server | `system`, `error`, `telemetry`, `fire`, `rfid` |
| `battledrome/tanks/{hostname}/commands` | server → tank | `command` |
