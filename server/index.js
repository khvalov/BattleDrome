const fs = require('fs');
const http = require('http');
const path = require('path');
const mqtt = require('mqtt');
const { WebSocketServer } = require('ws');

const MQTT_BROKER = 'mqtt://broker.hivemq.com:1883';
const TANK_TOPIC = 'battledrome/tanks/+/events';
const OFFLINE_TIMEOUT_MS = 15_000;
const HTTP_PORT = process.env.PORT || 8080;

// ── RFID action table ──────────────────────────────────────────────────────────
// Map each RFID tag UID (uppercase hex, no spaces) to a command sent back to
// the tank that scanned it. Add your physical tag UIDs here.
//
// Available params: health (0-100), ammo (0-100), ammoLevel (1-10),
//                   fireSpeed (1-10), immunable (0 or 1)
const RFID_ACTIONS = {
  // 'A1B2C3D4': { param: 'health',    value: 100 },  // Medkit  — full heal
  // 'B2C3D4E5': { param: 'ammo',      value: 100 },  // Ammo crate — full reload
  // 'C3D4E5F6': { param: 'immunable', value: 1   },  // Shield  — enable immunity
  // 'D4E5F6A7': { param: 'fireSpeed', value: 1   },  // Nitro   — max fire rate
  // 'E5F6A7B8': { param: 'ammoLevel', value: 10  },  // Power-up — max damage
};

// ── Tank state ─────────────────────────────────────────────────────────────────
const tanks = {}; // { tankId: { online, lastSeen, lastEvent, telemetry } }

function setTank(tankId, patch) {
  tanks[tankId] = Object.assign(tanks[tankId] || {}, patch);
}

function snapshot() {
  return Object.entries(tanks).map(([id, t]) => ({ id, ...t }));
}

// ── MQTT ───────────────────────────────────────────────────────────────────────
const mqttClient = mqtt.connect(MQTT_BROKER);

mqttClient.on('connect', () => {
  console.log('MQTT connected to', MQTT_BROKER);
  mqttClient.subscribe(TANK_TOPIC, { qos: 1 });
});

mqttClient.on('message', (topic, raw) => {
  // topic: battledrome/tanks/{tankId}/events
  const tankId = topic.split('/')[2];
  if (!tankId) return;

  let payload;
  try { payload = JSON.parse(raw.toString()); } catch { return; }

  const now = Date.now();
  const patch = { online: true, lastSeen: now, lastEvent: payload };
  const eventType = payload?.event?.type;

  // Merge telemetry data into persistent tank state
  if (eventType === 'telemetry' && payload.event.data) {
    patch.telemetry = { ...((tanks[tankId] || {}).telemetry || {}), ...payload.event.data };
  }

  // Fire events update ammo count immediately without waiting for next telemetry
  if (eventType === 'fire' && payload.event.data) {
    const prev = (tanks[tankId] || {}).telemetry || {};
    patch.telemetry = { ...prev, ammo: payload.event.data.ammo };
  }

  // RFID scan: log to console and dispatch action if configured
  if (eventType === 'rfid' && payload.event.data) {
    const uid = (payload.event.data.uid || '').toUpperCase();
    console.log(`[RFID] Tank ${tankId} scanned UID: ${uid}`);

    const action = RFID_ACTIONS[uid];
    if (action) {
      console.log(`[RFID] UID ${uid} → applying action:`, action);
      const cmd = {
        timestamp: Math.floor(Date.now() / 1000),
        event: { type: 'command', param: action.param, value: action.value },
      };
      mqttClient.publish(
        `battledrome/tanks/${tankId}/commands`,
        JSON.stringify(cmd),
        { qos: 1 }
      );
    } else {
      console.log(`[RFID] UID ${uid} — no action configured`);
    }
  }

  setTank(tankId, patch);
  broadcast({ type: 'update', tanks: snapshot() });
  broadcast({ type: 'log', tankId, receivedAt: now, payload });
});

mqttClient.on('error', (err) => console.error('MQTT error:', err.message));

// ── Offline detection ──────────────────────────────────────────────────────────
setInterval(() => {
  const now = Date.now();
  let changed = false;
  for (const [, tank] of Object.entries(tanks)) {
    if (tank.online && now - tank.lastSeen > OFFLINE_TIMEOUT_MS) {
      tank.online = false;
      changed = true;
    }
  }
  if (changed) broadcast({ type: 'update', tanks: snapshot() });
}, 5_000);

// ── HTTP ───────────────────────────────────────────────────────────────────────
const indexHtml = path.join(__dirname, 'public', 'index.html');

const httpServer = http.createServer((req, res) => {
  if (req.method === 'GET' && (req.url === '/' || req.url === '/index.html')) {
    res.writeHead(200, { 'Content-Type': 'text/html' });
    res.end(fs.readFileSync(indexHtml));
  } else {
    res.writeHead(404);
    res.end('Not found');
  }
});

// ── WebSocket ──────────────────────────────────────────────────────────────────
const wss = new WebSocketServer({ server: httpServer });
const clients = new Set();

wss.on('connection', (ws) => {
  clients.add(ws);
  ws.send(JSON.stringify({ type: 'update', tanks: snapshot() }));
  ws.on('close', () => clients.delete(ws));
  ws.on('error', () => clients.delete(ws));
});

function broadcast(data) {
  const msg = JSON.stringify(data);
  for (const ws of clients) {
    if (ws.readyState === ws.OPEN) ws.send(msg);
  }
}

httpServer.listen(HTTP_PORT, () => {
  console.log(`Dashboard: http://localhost:${HTTP_PORT}`);
});
