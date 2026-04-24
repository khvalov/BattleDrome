# JSON Interaction Specification

All hardware communication — Arduino ↔ Raspberry Pi (UART) and Raspberry Pi ↔ Server (MQTT) — uses newline-delimited JSON (`\r\n`).

---

## Message Structure

```json
{ "timestamp": 1234567890, "event": { "type": "<type>", ... } }
```

### `timestamp`
Unix epoch in seconds (`Math.floor(Date.now() / 1000)` / `millis() / 1000`).

---

## Event Types

### `system`
Generic system lifecycle event. Used for connection announcements and heartbeats.

```json
{ "timestamp": 1234567890, "event": { "type": "system", "action": "connected", "value": 1 } }
{ "timestamp": 1234567890, "event": { "type": "system", "action": "heartbeat", "value": 1 } }
```

### `error`
Failure report. No `action` or `value` required.

```json
{ "timestamp": 1234567890, "event": { "type": "error" } }
```

### `telemetry`  _(tank → server, every 500 ms)_
Snapshot of the tank's current physical and game state.

```json
{
  "timestamp": 1234567890,
  "event": {
    "type": "telemetry",
    "data": {
      "speed":     127,
      "health":    100,
      "ammo":      100,
      "ammoLevel": 3,
      "fireSpeed": 5,
      "immunable": false
    }
  }
}
```

| Field | Type | Range | Description |
|---|---|---|---|
| `speed` | int | 0–255 | Average absolute PWM across all 4 motors |
| `health` | int | 0–100 | Tank HP |
| `ammo` | int | 0–100 | Remaining ammunition |
| `ammoLevel` | int | 1–10 | Ammo power level |
| `fireSpeed` | int | 1–10 | Fire rate level |
| `immunable` | bool | — | Whether the tank is currently immune to damage |

### `command`  _(server → tank, via MQTT commands topic)_
Updates a single game-state variable on the Arduino. The Raspberry Pi forwards this to Arduino via UART.

```json
{ "timestamp": 0, "event": { "type": "command", "param": "health", "value": 80 } }
```

| `param` | Type | Range |
|---|---|---|
| `health` | int | 0–100 |
| `ammo` | int | 0–100 |
| `ammoLevel` | int | 1–10 |
| `fireSpeed` | int | 1–10 |
| `immunable` | int | 0 = false, 1 = true |

---

## MQTT Topics

| Topic | Direction | Content |
|---|---|---|
| `battledrome/tanks/{hostname}/events` | tank → server | `system`, `error`, `telemetry` |
| `battledrome/tanks/{hostname}/commands` | server → tank | `command` |
