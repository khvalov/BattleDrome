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
// /dev/serial0 is the system symlink to whichever UART is wired to GPIO14/15.
// On RPi Zero W with enable_uart=1 in /boot/config.txt this resolves to the
// stable PL011 UART (/dev/ttyAMA0). Without that flag it resolves to the
// mini UART (/dev/ttyS0) whose TX baud rate drifts with the CPU clock and
// causes the Arduino to reject incoming bytes.
const serial = new SerialPort({ path: '/dev/serial0', baudRate: 115200 });
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
  serial.write(json + '\r\n', (err) => {
    if (err) console.error('Serial write error:', err.message);
  });
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
  mqttClient.subscribe(COMMAND_TOPIC, { qos: 1 }, (err, granted) => {
    if (err) {
      console.error('MQTT subscribe error:', err.message);
    } else {
      console.log('Subscribed to', granted[0].topic, 'qos', granted[0].qos);
    }
  });
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
  serial.write(raw + '\r\n', (err) => {
    if (err) console.error('Serial write error:', err.message);
    else      console.log('Serial write OK');
  });
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
    const type   = msg?.event?.type;
    const action = msg?.event?.action;

    // Ping-pong: Arduino polls us to detect RPi presence.
    // Reply immediately via serial; do not publish to MQTT.
    if (type === 'system' && action === 'ping') {
      sendSerial(buildEvent('system', 'pong', 1));
      return;
    }

    // Enrich telemetry and fire events with RPi-side meta
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
