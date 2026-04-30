# BattleDrome Central Server

Real-time dashboard and game-state hub for all tanks in the arena.

---

## Quick Start

```bash
npm install
npm start
# Open http://localhost:8080
```

Set `PORT` env var to override the default port 8080.

---

## What it does

- Connects to `broker.hivemq.com:1883` and subscribes to `battledrome/tanks/+/events`
- Identifies tanks by the `{hostname}` segment of the MQTT topic
- Merges incoming `telemetry` payloads into persistent per-tank state — values survive between packets
- `fire` events update the ammo counter immediately without waiting for the next 500 ms telemetry cycle
- Marks a tank **offline** after **15 seconds** of silence; back online on the next message
- On `rfid` events: logs the UID and tank to console, looks up the UID in `RFID_ACTIONS`, and publishes a `command` back to the tank if a match is found
- Broadcasts a `log` message to all connected browsers for every MQTT message received
- Serves the dashboard HTML at `/` and pushes live updates via WebSocket; browser auto-reconnects on drop

---

## Dashboard

Each tank appears as a card with:

| Stat | Bar range | Colour |
|:---|:---|:---|
| Speed | 0–maxSpeed | Blue |
| Health | 0–100 | Green |
| Ammo | 0–100 | Yellow |
| Ammo Level | 1–10 | Amber |
| Fire Speed | 1–10 | Red-orange |
| Max Speed | 0–255 | Teal |
| Min Speed | 0–255 | Dark teal |
| Immune | badge (YES / NO) | Green / grey |

Cards gain a green border and pulse animation when a tank comes online, and fade when offline.

**Tank display name:** click the ✎ pencil icon on any card to set a custom display name (shown instead of the raw hostname). The raw ID is shown as a small grey label alongside. The name is stored server-side only — never sent to the tank hardware.

Below the tank grid, a **Telemetry Log** panel shows all incoming MQTT messages in raw JSON format — newest first, colour-coded by event type (`fire` = orange-red, `telemetry` = blue, `system` = green, `error` = red, `rfid` = amber). Keeps the last 100 entries. Has a Clear button.

---

## Sending commands to a tank

Publish to the tank's commands topic — the Raspberry Pi bridge forwards it to the Arduino over UART:

```bash
mosquitto_pub -h broker.hivemq.com \
  -t "battledrome/tanks/{hostname}/commands" \
  -m '{"timestamp":0,"event":{"type":"command","param":"health","value":80}}'
```

Valid `param` values: `health`, `ammo`, `ammoLevel`, `fireSpeed`, `immunable`, `maxSpeed`, `minSpeed`.

---

## REST API

### RFID action rules

| Method | Path | Body | Description |
|:---|:---|:---|:---|
| `GET` | `/api/rfid` | — | Return all configured rules |
| `POST` | `/api/rfid` | `{ uid, action, recipient, value }` | Add or update a rule |
| `DELETE` | `/api/rfid/:uid` | — | Remove a rule |

### Tank display name

| Method | Path | Body | Description |
|:---|:---|:---|:---|
| `PATCH` | `/api/tanks/:id` | `{ displayName }` | Set custom display name (server-only) |

Send `{ displayName: "" }` to clear back to the raw hostname.

---

## RFID action table

Rules are managed at runtime via the dashboard UI or the REST API above.

### Schema

| Field | Type | Values |
|:---|:---|:---|
| `uid` | string | Uppercase hex, e.g. `A1B2C3D4` |
| `action` | string | `ammo` · `health` · `speed` · `immune` · `maxspeed` · `minspeed` · `win` |
| `recipient` | string | `tank` · `others` · `teammate` · `all` |
| `value` | number | Delta applied to current value (positive = increase, negative = decrease) |

### Action mapping

| Action | Arduino param | Range |
|:---|:---|:---|
| `ammo` | `ammo` | 0–100 |
| `health` | `health` | 0–100 |
| `speed` | `fireSpeed` | 1–10 |
| `immune` | `immunable` | boolean (>0 = enable) |
| `maxspeed` | `maxSpeed` | 1–255 |
| `minspeed` | `minSpeed` | 0–255 |
| `win` | *(broadcast only)* | — |

The server reads the tank's current telemetry value, adds the delta, clamps to the valid range, and sends the resulting absolute value as a `command`.

### Win action

When a `win` rule fires, the server broadcasts `{ type: "win", tankId, recipient }` to all connected browsers. A full-screen victory overlay appears with a contextual message. No command is sent to the tank.

---

## Dependencies

| Package | Purpose |
|:---|:---|
| `mqtt` | Subscribes to tank events from the broker |
| `ws` | WebSocket server — pushes updates to browsers |
