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
- Tracks each tank's online/offline state — a tank is marked **offline** after **15 seconds** of silence
- Persists the latest telemetry snapshot per tank (speed, health, ammo, ammo level, fire speed, immune status)
- Serves the dashboard at `/` and pushes live updates to all connected browsers via WebSocket
- The browser auto-reconnects if the WebSocket drops

---

## Dashboard

Each tank appears as a card showing:

| Stat | Range | Source |
|:---|:---|:---|
| Speed | 0–255 | Avg PWM across 4 motors |
| Health | 0–100 | Game state variable |
| Ammo | 0–100 | Game state variable |
| Ammo Level | 1–10 | Game state variable |
| Fire Speed | 1–10 | Game state variable |
| Immune | yes/no | Game state variable |

Cards turn green and pulse when a tank comes online, and fade when offline.

---

## Sending commands to a tank

Publish to the tank's commands topic — the Raspberry Pi bridge will forward it to the Arduino over UART:

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
| `mqtt` | MQTT client (subscribes to tank events) |
| `ws` | WebSocket server (pushes updates to browsers) |
