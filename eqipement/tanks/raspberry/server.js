const { execSync } = require('child_process');
const os = require('os');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');
const http = require('http');
const mqtt = require('mqtt');

// ── Local IP helper ────────────────────────────────────────────────────────────
function getLocalIP() {
  for (const ifaces of Object.values(os.networkInterfaces())) {
    for (const iface of ifaces) {
      if (iface.family === 'IPv4' && !iface.internal) return iface.address;
    }
  }
  return '0.0.0.0';
}

// ── Serial port ────────────────────────────────────────────────────────────────
const serial = new SerialPort({ path: '/dev/ttyS0', baudRate: 115200 });
const parser = serial.pipe(new ReadlineParser({ delimiter: '\r\n' }));

// ── MQTT ───────────────────────────────────────────────────────────────────────
const TANK_ID = os.hostname();
const mqttClient = mqtt.connect('mqtt://broker.hivemq.com:1883');
const MQTT_TOPIC    = `battledrome/tanks/${TANK_ID}/events`;
const COMMAND_TOPIC = `battledrome/tanks/${TANK_ID}/commands`;

// ── Helpers ────────────────────────────────────────────────────────────────────
function isWifiConnected() {
  try {
    const output = execSync('nmcli -t -f DEVICE,TYPE,STATE device').toString();
    return output.split('\n').some(line => {
      const [, type, state] = line.split(':');
      return type === 'wifi' && state === 'connected';
    });
  } catch { return false; }
}

function buildEvent(type, action = null, value = null) {
  const event = { type };
  if (type === 'system') {
    event.action = action;
    event.value = value;
  }
  return {
    timestamp: Math.floor(Date.now() / 1000),
    event,
  };
}

function sendSerial(payload) {
  const json = JSON.stringify(payload);
  serial.write(json + '\r\n');
  console.log('Serial sent:', json);
}

function publishMqtt(payload) {
  const json = JSON.stringify(payload);
  mqttClient.publish(MQTT_TOPIC, json, { qos: 1 }, (err) => {
    if (err) console.error('MQTT publish error:', err.message);
    else console.log('MQTT published:', json);
  });
}

function broadcast(payload) {
  sendSerial(payload);
  publishMqtt(payload);
}

// ── Heartbeat ──────────────────────────────────────────────────────────────────
const HEARTBEAT_INTERVAL_MS = 5_000;
let heartbeatTimer = null;

function startHeartbeat() {
  if (heartbeatTimer) return;
  heartbeatTimer = setInterval(() => {
    publishMqtt(buildEvent('system', 'heartbeat', 1));
  }, HEARTBEAT_INTERVAL_MS);
}

function stopHeartbeat() {
  clearInterval(heartbeatTimer);
  heartbeatTimer = null;
}

// ── MQTT events ────────────────────────────────────────────────────────────────
mqttClient.on('connect', () => {
  console.log('MQTT connected');
  mqttClient.subscribe(COMMAND_TOPIC, { qos: 1 });
  const connected = isWifiConnected();
  const payload = connected
    ? buildEvent('system', 'connected', 1)
    : buildEvent('error');
  broadcast(payload);
  startHeartbeat();
});

mqttClient.on('message', (topic, message) => {
  if (topic !== COMMAND_TOPIC) return;
  // Forward command from MQTT to Arduino via serial
  const raw = message.toString().trim();
  console.log('Command received, forwarding to Arduino:', raw);
  serial.write(raw + '\r\n');
});

mqttClient.on('error', (err) => {
  console.error('MQTT error:', err.message);
  broadcast(buildEvent('error'));
  stopHeartbeat();
});

// ── Serial incoming JSON handler ───────────────────────────────────────────────
parser.on('data', (line) => {
  try {
    const msg = JSON.parse(line.trim());
    console.log('Serial received:', msg);
    // Enrich telemetry and fire events with RPi-side meta
    const type = msg?.event?.type;
    if ((type === 'telemetry' || type === 'fire' || type === 'rfid') && msg.event.data) {
      msg.event.data.ip       = getLocalIP();
      msg.event.data.hostname = TANK_ID;
    }
    publishMqtt(msg);
  } catch (err) {
    console.error('Invalid JSON from serial:', line.trim());
    sendSerial(buildEvent('error'));
  }
});

serial.on('error', (err) => console.error('Serial error:', err.message));

// ── WiFi setup ─────────────────────────────────────────────────────────────────
if (!isWifiConnected()) {
  console.log('No WiFi connection, starting captive portal...');
  execSync('wifi-connect --portal-ssid "MyDevice-Setup"', { stdio: 'inherit' });
}

// ── HTTP server ────────────────────────────────────────────────────────────────
const server = http.createServer((req, res) => {
  res.writeHead(200, { 'Content-Type': 'text/plain' });
  res.end('OK');
});

server.listen(3000, () => console.log('Server running on port 3000'));
