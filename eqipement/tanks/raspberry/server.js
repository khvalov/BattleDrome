const { execSync } = require('child_process');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');
const http = require('http');
const mqtt = require('mqtt');

// ── Serial port ────────────────────────────────────────────────────────────────
const serial = new SerialPort({ path: '/dev/ttyS0', baudRate: 115200 });
const parser = serial.pipe(new ReadlineParser({ delimiter: '\r\n' }));

// ── MQTT ───────────────────────────────────────────────────────────────────────
const mqttClient = mqtt.connect('mqtt://broker.hivemq.com:1883');
const MQTT_TOPIC = 'battledrome/events';

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

// ── MQTT events ────────────────────────────────────────────────────────────────
mqttClient.on('connect', () => {
  console.log('MQTT connected');
  const connected = isWifiConnected();
  const payload = connected
    ? buildEvent('system', 'connected', 1)
    : buildEvent('error');
  broadcast(payload);
});

mqttClient.on('error', (err) => {
  console.error('MQTT error:', err.message);
  broadcast(buildEvent('error'));
});

// ── Serial incoming JSON handler ───────────────────────────────────────────────
parser.on('data', (line) => {
  try {
    const msg = JSON.parse(line.trim());
    console.log('Serial received:', msg);
    // Forward incoming serial events to MQTT
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
