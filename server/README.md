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

Below the tank grid, a **Telemetry Log** panel shows all incoming MQTT messages in raw JSON format — newest first, colour-coded by event type (`fire` = orange, `telemetry` = blue, `system` = green, `error` = red). Keeps the last 100 entries. Has a Clear button.

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

## Dependencies

| Package | Purpose |
|:---|:---|
| `mqtt` | Subscribes to tank events from the broker |
| `ws` | WebSocket server — pushes updates to browsers |
