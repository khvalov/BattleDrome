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
// Managed at runtime via the dashboard UI (GET/POST/DELETE /api/rfid).
// Schema per entry:
//   action    : 'ammo' | 'health' | 'speed' | 'immune' | 'win'
//   recipient : 'tank' | 'others' | 'teammate' | 'all'
//   value     : number — positive = increase, negative = decrease
//               (for 'immune': >0 = enable, ≤0 = disable)
//               (for 'win': value is ignored)
const RFID_ACTIONS = {
  // Example — uncomment and replace UIDs with your physical tags:
  // '26211603': { action: 'health', recipient: 'tank',   value:  20 }, // Medkit
  // 'A1B2C3D4': { action: 'ammo',   recipient: 'tank',   value:  50 }, // Ammo crate
  // 'B2C3D4E5': { action: 'immune', recipient: 'tank',   value:   1 }, // Shield
  // 'C3D4E5F6': { action: 'health', recipient: 'others', value: -20 }, // Landmine
  // 'D4E5F6A7': { action: 'win',    recipient: 'tank',   value:   0 }, // Win flag
};

// Map user-facing action names to Arduino command params
const ACTION_PARAM = {
  ammo:     'ammo',
  health:   'health',
  speed:    'fireSpeed',
  immune:   'immunable',
  maxspeed: 'maxSpeed',
  minspeed: 'minSpeed',
  win:      null,  // handled separately
};

// Valid ranges for each param (used for delta clamping)
const PARAM_RANGE = {
  ammo:      [0, 100],
  health:    [0, 100],
  fireSpeed: [1, 10],
  immunable: [0, 1],
  maxSpeed:  [1, 255],
  minSpeed:  [0, 255],
};

const VALID_ACTIONS    = ['ammo', 'health', 'speed', 'immune', 'maxspeed', 'minspeed', 'win'];
const VALID_RECIPIENTS = ['tank', 'others', 'teammate', 'all'];

// ── Tank state ─────────────────────────────────────────────────────────────────
const tanks = {};

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

function sendCommand(tankId, param, value) {
  const cmd = {
    timestamp: Math.floor(Date.now() / 1000),
    event: { type: 'command', param, value },
  };
  mqttClient.publish(
    `battledrome/tanks/${tankId}/commands`,
    JSON.stringify(cmd),
    { qos: 1 }
  );
  console.log(`[CMD] → ${tankId}: ${param} = ${value}`);
}

function applyRfidAction(scannerTankId, entry) {
  const { action, recipient, value } = entry;

  // ── Win condition ──────────────────────────────────────────────────────────
  if (action === 'win') {
    console.log(`[RFID] 🏆 Win triggered by ${scannerTankId} (recipient: ${recipient})`);
    broadcast({ type: 'win', tankId: scannerTankId, recipient });
    return;
  }

  const param = ACTION_PARAM[action];
  if (!param) return;

  // ── Resolve target tanks ───────────────────────────────────────────────────
  const allIds = Object.keys(tanks);
  let targetIds;
  switch (recipient) {
    case 'tank':     targetIds = [scannerTankId]; break;
    case 'others':   targetIds = allIds.filter(id => id !== scannerTankId); break;
    case 'all':      targetIds = allIds; break;
    case 'teammate':
      // Teams not yet implemented — applies to scanner only
      targetIds = [scannerTankId];
      console.log('[RFID] teammate recipient: teams not implemented, applying to scanner');
      break;
    default:         targetIds = [scannerTankId];
  }

  // ── Compute and send commands ──────────────────────────────────────────────
  for (const targetId of targetIds) {
    let newValue;

    if (param === 'immunable') {
      // Immune is boolean — positive enables, zero/negative disables
      newValue = value > 0 ? 1 : 0;
    } else {
      // Delta: add value to current, then clamp to valid range
      const rawCurrent = tanks[targetId]?.telemetry?.[param];
      if (rawCurrent === undefined) {
        console.warn(`[RFID] WARNING: ${param} not in telemetry for ${targetId} — defaulting current to 0`);
      }
      const current = rawCurrent ?? 0;
      const [min, max] = PARAM_RANGE[param] || [0, 100];
      newValue = Math.min(max, Math.max(min, current + value));
      console.log(`[RFID] ${param}: current=${current} delta=${value} → newValue=${newValue} (range ${min}–${max})`);
    }

    sendCommand(targetId, param, newValue);
  }
}

mqttClient.on('message', (topic, raw) => {
  const tankId = topic.split('/')[2];
  if (!tankId) return;

  let payload;
  try { payload = JSON.parse(raw.toString()); } catch { return; }

  const now = Date.now();
  const patch = { online: true, lastSeen: now, lastEvent: payload };
  const eventType = payload?.event?.type;

  if (eventType === 'telemetry' && payload.event.data) {
    patch.telemetry = { ...((tanks[tankId] || {}).telemetry || {}), ...payload.event.data };
  }

  if (eventType === 'fire' && payload.event.data) {
    const prev = (tanks[tankId] || {}).telemetry || {};
    patch.telemetry = { ...prev, ammo: payload.event.data.ammo };
  }

  if (eventType === 'rfid' && payload.event.data) {
    const uid = (payload.event.data.uid || '').toUpperCase();
    console.log(`[RFID] Tank ${tankId} scanned UID: ${uid}`);

    const entry = RFID_ACTIONS[uid];
    if (entry) {
      console.log(`[RFID] UID ${uid} → action:`, entry);
      applyRfidAction(tankId, entry);
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

function readBody(req) {
  return new Promise((resolve, reject) => {
    let body = '';
    req.on('data', chunk => body += chunk);
    req.on('end', () => { try { resolve(JSON.parse(body)); } catch { reject(); } });
  });
}

const httpServer = http.createServer(async (req, res) => {
  const url = req.url.split('?')[0];

  // ── Dashboard ──────────────────────────────────────────────────────────────
  if (req.method === 'GET' && (url === '/' || url === '/index.html')) {
    res.writeHead(200, { 'Content-Type': 'text/html' });
    res.end(fs.readFileSync(indexHtml));
    return;
  }

  // ── RFID API ───────────────────────────────────────────────────────────────
  if (url === '/api/rfid') {
    if (req.method === 'GET') {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(RFID_ACTIONS));
      return;
    }

    if (req.method === 'POST') {
      let body;
      try { body = await readBody(req); } catch {
        res.writeHead(400); res.end('Invalid JSON'); return;
      }
      const { uid, action, recipient, value } = body;

      if (!uid || !VALID_ACTIONS.includes(action) || !VALID_RECIPIENTS.includes(recipient)) {
        res.writeHead(422); res.end('Invalid fields'); return;
      }

      const key = String(uid).toUpperCase();
      RFID_ACTIONS[key] = { action, recipient, value: Number(value) || 0 };
      console.log(`[RFID] Action saved: ${key} →`, RFID_ACTIONS[key]);
      broadcast({ type: 'rfid_actions', actions: RFID_ACTIONS });

      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ok: true, uid: key }));
      return;
    }
  }

  // ── Tank display name API ──────────────────────────────────────────────────
  // PATCH /api/tanks/:id  { "displayName": "Red Dragon" }
  // Display name is server-only — never sent to the tank hardware.
  if (url.startsWith('/api/tanks/') && req.method === 'PATCH') {
    const tankId = decodeURIComponent(url.slice('/api/tanks/'.length));
    let body;
    try { body = await readBody(req); } catch {
      res.writeHead(400); res.end('Invalid JSON'); return;
    }
    const name = String(body.displayName || '').trim().slice(0, 32);
    setTank(tankId, { displayName: name || null });
    broadcast({ type: 'update', tanks: snapshot() });
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true }));
    return;
  }

  if (url.startsWith('/api/rfid/') && req.method === 'DELETE') {
    const uid = url.slice('/api/rfid/'.length).toUpperCase();
    if (RFID_ACTIONS[uid]) {
      delete RFID_ACTIONS[uid];
      console.log(`[RFID] Action deleted: ${uid}`);
      broadcast({ type: 'rfid_actions', actions: RFID_ACTIONS });
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ok: true }));
    } else {
      res.writeHead(404); res.end('Not found');
    }
    return;
  }

  res.writeHead(404); res.end('Not found');
});

// ── WebSocket ──────────────────────────────────────────────────────────────────
const wss = new WebSocketServer({ server: httpServer });
const clients = new Set();

wss.on('connection', (ws) => {
  clients.add(ws);
  ws.send(JSON.stringify({ type: 'update', tanks: snapshot() }));
  ws.send(JSON.stringify({ type: 'rfid_actions', actions: RFID_ACTIONS }));
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
