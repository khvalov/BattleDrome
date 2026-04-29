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
| Speed | 0–255 | Blue |
| Health | 0–100 | Green |
| Ammo | 0–100 | Yellow |
| Ammo Level | 1–10 | Amber |
| Fire Speed | 1–10 | Red-orange |
| Immune | badge (YES / NO) | Green / grey |

Cards gain a green border and pulse animation when a tank comes online, and fade when offline.

Below the tank grid, a **Telemetry Log** panel shows all incoming MQTT messages in raw JSON format — newest first, colour-coded by event type (`fire` = orange, `telemetry` = blue, `system` = green, `error` = red, `rfid` = same orange as fire). Keeps the last 100 entries. Has a Clear button.

---

## Sending commands to a tank

Publish to the tank's commands topic — the Raspberry Pi bridge forwards it to the Arduino over UART:

```bash
mosquitto_pub -h broker.hivemq.com \
  -t "battledrome/tanks/{hostname}/commands" \
  -m '{"timestamp":0,"event":{"type":"command","param":"health","value":80}}'
```

Valid `param` values: `health`, `ammo`, `ammoLevel`, `fireSpeed`, `immunable`.

---

## RFID action table

Edit the `RFID_ACTIONS` object at the top of `server/index.js` to assign effects to physical tags:

```js
const RFID_ACTIONS = {
  'A1B2C3D4': { param: 'health',    value: 100 },  // Medkit
  'B2C3D4E5': { param: 'ammo',      value: 100 },  // Ammo crate
  'C3D4E5F6': { param: 'immunable', value: 1   },  // Shield
  'D4E5F6A7': { param: 'fireSpeed', value: 1   },  // Nitro
};
```

UIDs are uppercase hex strings with no spaces (e.g. `A1B2C3D4`). Use the dashboard telemetry log to discover unknown tag UIDs — they appear as `rfid` events. When a known UID is scanned the server logs the match and immediately publishes the configured `command` to the tank.

---

## Dependencies

| Package | Purpose |
|:---|:---|
| `mqtt` | Subscribes to tank events from the broker |
| `ws` | WebSocket server — pushes updates to browsers |
