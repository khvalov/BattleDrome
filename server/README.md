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

### Pages

The dashboard has three top-level tabs:

| Tab | Purpose |
|:---|:---|
| **Dashboard** | Tank cards, Game panel (mode config, timer, scoreboard) |
| **Tag Reader** | Shows every RFID UID scanned by any tank — large monospace UID, which tank scanned it, timestamp, Copy button, and inline action Configure/Edit button. Also displays all configured RFID rules with edit/delete. |
| **Telemetry Log** | All incoming MQTT messages in raw JSON — newest first, colour-coded by event type. Filterable by tank and event type. Keeps last 100 entries. |

Both Tag Reader and Telemetry Log tabs show an unread-count badge when new events arrive while you're on another page.

### Game Modes

| Mode | Rules |
|:---|:---|
| **Free Play** | FFA. Each tank gets a home RFID. Killed tanks can drive but not shoot — return to home base to respawn with immunity. Wins/losses scoring. |
| **CTF — Teams** | Teams with shared home RFID. When a team's base is captured, the **entire team is eliminated** (all tanks stop). Last team standing wins. Timed: most captures wins if multiple teams alive. Dead tanks respawn at own base (unless team eliminated). |
| **CTF — Solo** | Each tank has its own home RFID (assigned like Free Play). Base captured = eliminated. Kill by shooting = eliminated (no respawn). Timed: most captures wins. Unlimited: last standing wins. |
| **Treasure Hunt** | Tanks collect points by scanning RFID tags. Each tank can only scan a tag once (other tanks can still scan it). Unregistered tags auto-register as 1 point. Optional shooting — hits reduce speed by 50% for 3s (no health damage). Highest score at time up wins. |
| **Race** | Tanks race through ordered RFID checkpoints (looping). Each completed loop = 1 lap. Wrong checkpoint = no advance. Unregistered tags auto-append to checkpoint order. Optional shooting — hits reduce speed by 50% for 3s (no health damage). Most laps wins. Score target = lap target. |

### Game REST API

| Method | Path | Body | Description |
|:---|:---|:---|:---|
| `PATCH` | `/api/game` | `{ mode, timeLimit, scoreTarget, immunityDuration, treasureShooting, raceShooting }` | Update game config |
| `POST` | `/api/game/start` | — | Start round |
| `POST` | `/api/game/stop` | — | Stop round |
| `POST` | `/api/game/reset` | — | Reset to idle |
| `PUT` | `/api/game/teams/:teamId` | `{ name, color, homeUid, tankIds }` | Create/update team |
| `DELETE` | `/api/game/teams/:teamId` | — | Delete team |
| `PUT` | `/api/game/free-bases/:tankId` | `{ uid }` | Set free play home base |
| `DELETE` | `/api/game/free-bases/:tankId` | — | Remove free play base |
| `PUT` | `/api/game/solo-bases/:tankId` | `{ uid }` | Set CTF solo home base |
| `DELETE` | `/api/game/solo-bases/:tankId` | — | Remove CTF solo base |

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
| `POST` | `/api/rfid` | `{ uid, action, operation, recipient, value }` | Add or update a rule |
| `DELETE` | `/api/rfid/:uid` | — | Remove a rule |

### Tank display name

| Method | Path | Body | Description |
|:---|:---|:---|:---|
| `PATCH` | `/api/tanks/:id` | `{ displayName }` | Set custom display name (server-only) |

Send `{ displayName: "" }` to clear back to the raw hostname.

---

## RFID action table

Rules are managed at runtime via the **Tag Reader** tab or the REST API above. Each scanned tag shows a Configure/Edit button inline.

### Schema

| Field | Type | Values |
|:---|:---|:---|
| `uid` | string | Uppercase hex, e.g. `A1B2C3D4` |
| `action` | string | `health` · `ammo` · `speed` · `immune` · `maxspeed` · `minspeed` · `ammopower` · `points` · `win` |
| `operation` | string | `add` · `reduce` · `set` (ignored for `immune` and `win`) |
| `recipient` | string | `tank` · `others` · `teammate` · `other_teams` · `all` |
| `value` | number | Amount to add, reduce, or set to (for `immune`: seconds of immunity) |

### Action mapping

| Action | Arduino param | Range | Notes |
|:---|:---|:---|:---|
| `health` | `health` | 0–100 | |
| `ammo` | `ammo` | 0–100 | |
| `speed` | `fireSpeed` | 1–10 | Shot cooldown |
| `immune` | `immunable` | bool | Value = seconds; server auto-removes |
| `maxspeed` | `maxSpeed` | 1–255 | |
| `minspeed` | `minSpeed` | 0–255 | |
| `ammopower` | `ammoLevel` | 1–10 | Damage per shot |
| `points` | *(server-only)* | — | Modifies `game.scores` |
| `win` | *(broadcast only)* | — | Dashboard overlay |

### Operation modes

- **add** — adds `value` to the tank's current telemetry value, clamped to valid range
- **reduce** — subtracts `|value|` from current, clamped to valid range
- **set** — sets the param to exactly `|value|`, clamped to valid range
- Legacy rules without `operation` default to `add`

### Recipients

| Recipient | Targets |
|:---|:---|
| `tank` | Scanning tank only (self) |
| `others` | All tanks except the scanner |
| `teammate` | Scanner's teammates in CTF Teams; falls back to self if no team |
| `other_teams` | All tanks NOT on the scanner's team; falls back to `others` if no team |
| `all` | Every online tank |

### Win action

When a `win` rule fires, the server broadcasts `{ type: "win", tankId, recipient }` to all connected browsers. A full-screen victory overlay appears with a contextual message. No command is sent to the tank.

---

## Dependencies

| Package | Purpose |
|:---|:---|
| `mqtt` | Subscribes to tank events from the broker |
| `ws` | WebSocket server — pushes updates to browsers |
