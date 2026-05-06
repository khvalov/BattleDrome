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
const VALID_MODES      = ['free_play', 'ctf_teams', 'ctf_solo', 'treasure_hunt', 'race'];

// ── Game state ─────────────────────────────────────────────────────────────────
// Modes:
//   free_play     — no rules, just RFID_ACTIONS table applies
//   ctf_teams     — teams capture each other's home RFID bases
//   ctf_solo      — each tank has a home RFID; capture opponents' bases
//   treasure_hunt — all configured RFIDs are treasures worth points + optional artifact
//   race          — tanks race to hit checkpoints; first to each scores a point
//
// status: idle → running → ended
const game = {
  mode:             'free_play',
  status:           'idle',     // idle | running | ended
  timeLimit:        240,        // seconds; 0 = no limit
  timeRemaining:    0,
  scoreTarget:      0,          // 0 = no score-based win condition
  scores:           {},         // { id: number }  — tankId or teamId depending on mode
  teams:            {},         // { teamId: { name, color, homeUid, tankIds[] } }
  bases:            {},         // { uid: tankId }  — ctf_solo home bases
  treasures:        {},         // { uid: { label, points, action, actionValue } }
  checkpoints:      [],         // [uid, ...]  — ordered for race
  takenCheckpoints: {},         // { uid: tankId }  — race: who took each checkpoint first
};

let gameTimer = null;

function gameSnapshot() {
  return {
    mode:             game.mode,
    status:           game.status,
    timeLimit:        game.timeLimit,
    timeRemaining:    game.timeRemaining,
    scoreTarget:      game.scoreTarget,
    scores:           { ...game.scores },
    teams:            JSON.parse(JSON.stringify(game.teams)),
    bases:            { ...game.bases },
    treasures:        JSON.parse(JSON.stringify(game.treasures)),
    checkpoints:      [...game.checkpoints],
    takenCheckpoints: { ...game.takenCheckpoints },
  };
}

function broadcastGame(msgType = 'game_update', extra = {}) {
  broadcast({ type: msgType, game: gameSnapshot(), ...extra });
}

function startRound() {
  if (game.status === 'running') return false;
  game.status           = 'running';
  game.scores           = {};
  game.takenCheckpoints = {};
  clearInterval(gameTimer);
  gameTimer = null;

  if (game.timeLimit > 0) {
    game.timeRemaining = game.timeLimit;
    gameTimer = setInterval(() => {
      game.timeRemaining = Math.max(0, game.timeRemaining - 1);
      broadcast({ type: 'game_tick', timeRemaining: game.timeRemaining });
      if (game.timeRemaining <= 0) endRound('time_up');
    }, 1000);
  } else {
    game.timeRemaining = 0;
  }

  broadcastGame();
  console.log(`[GAME] Round started — mode: ${game.mode}, timeLimit: ${game.timeLimit}s, scoreTarget: ${game.scoreTarget}`);
  return true;
}

function endRound(reason) {
  if (game.status !== 'running') return;
  clearInterval(gameTimer);
  gameTimer = null;
  game.status = 'ended';
  console.log(`[GAME] Round ended — reason: ${reason}`);
  broadcastGame('game_end', { reason });
}

function resetRound() {
  clearInterval(gameTimer);
  gameTimer = null;
  game.status           = 'idle';
  game.timeRemaining    = 0;
  game.scores           = {};
  game.takenCheckpoints = {};
  broadcastGame();
  console.log('[GAME] Round reset');
}

function addScore(id, delta) {
  game.scores[id] = (game.scores[id] || 0) + delta;
}

function getTeamOfTank(tankId) {
  for (const [teamId, team] of Object.entries(game.teams)) {
    if ((team.tankIds || []).includes(tankId)) return { teamId, team };
  }
  return null;
}

// Returns true if the game mode handled this RFID scan (prevents fallthrough to RFID_ACTIONS).
function handleGameRfid(scannerTankId, uid) {
  if (game.status !== 'running') return false;
  switch (game.mode) {
    case 'ctf_teams':     return handleCtfTeams(scannerTankId, uid);
    case 'ctf_solo':      return handleCtfSolo(scannerTankId, uid);
    case 'treasure_hunt': return handleTreasureHunt(scannerTankId, uid);
    case 'race':          return handleRace(scannerTankId, uid);
    default:              return false;  // free_play: fall through to RFID_ACTIONS
  }
}

function handleCtfTeams(scannerTankId, uid) {
  // Find which team owns this home base
  let homeTeamId = null;
  for (const [teamId, team] of Object.entries(game.teams)) {
    if ((team.homeUid || '').toUpperCase() === uid) { homeTeamId = teamId; break; }
  }
  if (!homeTeamId) return false; // not a registered home base → fall through

  const scannerInfo = getTeamOfTank(scannerTankId);
  if (!scannerInfo) return true; // scanner has no team → consume event, no score

  if (scannerInfo.teamId === homeTeamId) {
    console.log(`[GAME] CTF Teams: ${scannerTankId} scanned own base — no capture`);
    return true;
  }

  addScore(scannerInfo.teamId, 1);
  console.log(`[GAME] CTF Teams: ${scannerInfo.teamId} captured ${homeTeamId}'s base! Score: ${game.scores[scannerInfo.teamId]}`);
  broadcastGame();
  return true;
}

function handleCtfSolo(scannerTankId, uid) {
  const ownerTankId = game.bases[uid];
  if (!ownerTankId) return false; // not a registered base → fall through

  if (ownerTankId === scannerTankId) {
    console.log(`[GAME] CTF Solo: ${scannerTankId} scanned own base — no capture`);
    return true;
  }

  addScore(scannerTankId, 1);
  console.log(`[GAME] CTF Solo: ${scannerTankId} captured ${ownerTankId}'s base! Score: ${game.scores[scannerTankId]}`);
  broadcastGame();
  return true;
}

function handleTreasureHunt(scannerTankId, uid) {
  const treasure = game.treasures[uid];
  if (!treasure) return false; // not a registered treasure → fall through

  const pts = Number(treasure.points) || 0;
  addScore(scannerTankId, pts);
  console.log(`[GAME] Treasure: ${scannerTankId} collected ${uid} (+${pts} pts). Total: ${game.scores[scannerTankId]}`);

  // Apply artifact effect if configured
  if (treasure.action && treasure.action !== 'none') {
    applyRfidAction(scannerTankId, { action: treasure.action, recipient: 'tank', value: treasure.actionValue || 0 });
  }

  const reachedTarget = game.scoreTarget > 0 && game.scores[scannerTankId] >= game.scoreTarget;
  broadcastGame();
  if (reachedTarget) endRound('score_target');
  return true;
}

function handleRace(scannerTankId, uid) {
  if (!game.checkpoints.includes(uid)) return false; // not a checkpoint → fall through
  if (game.takenCheckpoints[uid]) return true;        // already taken → consume, no score

  game.takenCheckpoints[uid] = scannerTankId;
  addScore(scannerTankId, 1);
  console.log(`[GAME] Race: ${scannerTankId} hit checkpoint ${uid}. Score: ${game.scores[scannerTankId]}`);

  const allTaken      = game.checkpoints.every(cp => game.takenCheckpoints[cp]);
  const reachedTarget = game.scoreTarget > 0 && game.scores[scannerTankId] >= game.scoreTarget;
  broadcastGame();
  if (allTaken || reachedTarget) endRound(reachedTarget ? 'score_target' : 'all_checkpoints');
  return true;
}

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

  if (eventType === 'hit' && payload.event.data) {
    const { shooterAddr, damage, health: reportedHealth } = payload.event.data;

    // Try to identify the shooter by XOR-folding known tank IDs and matching addr
    let shooterId = null;
    for (const [id] of Object.entries(tanks)) {
      const fold = [...id].reduce((acc, c) => acc ^ c.charCodeAt(0), 0);
      if (fold === shooterAddr) { shooterId = id; break; }
    }

    const currentHealth = (tanks[tankId] || {}).telemetry?.health ?? 100;
    const newHealth     = Math.max(0, currentHealth - damage);

    console.log(
      `[HIT] ${tankId} hit by ${shooterId ? shooterId : `0x${shooterAddr.toString(16)}`}` +
      ` — damage: ${damage}, health: ${currentHealth} → ${newHealth}`
    );

    // Update local telemetry cache so subsequent hits compound correctly
    const prev = (tanks[tankId] || {}).telemetry || {};
    patch.telemetry = { ...prev, health: newHealth };

    // Send the authoritative health value back to the receiver
    sendCommand(tankId, 'health', newHealth);
  }

  if (eventType === 'rfid' && payload.event.data) {
    const uid = (payload.event.data.uid || '').toUpperCase();
    console.log(`[RFID] Tank ${tankId} scanned UID: ${uid}`);

    // Game mode handler takes priority; fall back to RFID_ACTIONS if not consumed
    const handledByGame = handleGameRfid(tankId, uid);
    if (!handledByGame) {
      const entry = RFID_ACTIONS[uid];
      if (entry) {
        console.log(`[RFID] UID ${uid} → action:`, entry);
        applyRfidAction(tankId, entry);
      } else {
        console.log(`[RFID] UID ${uid} — no action configured`);
      }
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

  // ── Game API ───────────────────────────────────────────────────────────────

  if (req.method === 'GET' && url === '/api/game') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(gameSnapshot()));
    return;
  }

  if (req.method === 'PATCH' && url === '/api/game') {
    let body;
    try { body = await readBody(req); } catch { res.writeHead(400); res.end('Bad JSON'); return; }
    if (game.status !== 'running') {
      if (body.mode !== undefined && VALID_MODES.includes(body.mode)) game.mode = body.mode;
      if (body.timeLimit !== undefined)  game.timeLimit   = Math.max(0, Number(body.timeLimit)  || 0);
      if (body.scoreTarget !== undefined) game.scoreTarget = Math.max(0, Number(body.scoreTarget) || 0);
    }
    broadcastGame();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true }));
    return;
  }

  if (req.method === 'POST' && url === '/api/game/start') {
    const ok = startRound();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok }));
    return;
  }

  if (req.method === 'POST' && url === '/api/game/stop') {
    endRound('manual');
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true }));
    return;
  }

  if (req.method === 'POST' && url === '/api/game/reset') {
    resetRound();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true }));
    return;
  }

  // Teams
  if (url.startsWith('/api/game/teams/')) {
    const teamId = decodeURIComponent(url.slice('/api/game/teams/'.length));

    if (req.method === 'PUT') {
      let body;
      try { body = await readBody(req); } catch { res.writeHead(400); res.end(); return; }
      game.teams[teamId] = {
        name:    String(body.name    || teamId),
        color:   String(body.color   || '#888888'),
        homeUid: String(body.homeUid || '').toUpperCase(),
        tankIds: Array.isArray(body.tankIds) ? body.tankIds : [],
      };
      broadcastGame();
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ok: true }));
      return;
    }

    if (req.method === 'DELETE') {
      delete game.teams[teamId];
      delete game.scores[teamId];
      broadcastGame();
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ok: true }));
      return;
    }
  }

  // CTF Solo bases
  if (url.startsWith('/api/game/bases/')) {
    const uid = decodeURIComponent(url.slice('/api/game/bases/'.length)).toUpperCase();

    if (req.method === 'PUT') {
      let body;
      try { body = await readBody(req); } catch { res.writeHead(400); res.end(); return; }
      game.bases[uid] = String(body.tankId || '');
      broadcastGame();
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ok: true }));
      return;
    }

    if (req.method === 'DELETE') {
      delete game.bases[uid];
      broadcastGame();
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ok: true }));
      return;
    }
  }

  // Treasures
  if (url.startsWith('/api/game/treasures/')) {
    const uid = decodeURIComponent(url.slice('/api/game/treasures/'.length)).toUpperCase();

    if (req.method === 'PUT') {
      let body;
      try { body = await readBody(req); } catch { res.writeHead(400); res.end(); return; }
      game.treasures[uid] = {
        label:       String(body.label       || uid),
        points:      Number(body.points)      || 0,
        action:      String(body.action       || 'none'),
        actionValue: Number(body.actionValue) || 0,
      };
      broadcastGame();
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ok: true }));
      return;
    }

    if (req.method === 'DELETE') {
      delete game.treasures[uid];
      broadcastGame();
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ok: true }));
      return;
    }
  }

  // Race checkpoints (full list replacement)
  if (req.method === 'PUT' && url === '/api/game/checkpoints') {
    let body;
    try { body = await readBody(req); } catch { res.writeHead(400); res.end(); return; }
    game.checkpoints = Array.isArray(body.uids)
      ? body.uids.map(u => String(u).toUpperCase())
      : [];
    broadcastGame();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true }));
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
  ws.send(JSON.stringify({ type: 'game_update', game: gameSnapshot() }));
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
