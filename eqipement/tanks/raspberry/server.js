const { execSync } = require('child_process');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');
const http = require('http');

const port = new SerialPort({
  path: '/dev/ttyS0',
  baudRate: 115200,
});

const parser = port.pipe(new ReadlineParser({ delimiter: '\r\n' }));

function isWifiConnected() {
  try {
    const output = execSync('nmcli -t -f DEVICE,TYPE,STATE device').toString();
    return output.split('\n').some(line => {
      const [, type, state] = line.split(':');
      return type === 'wifi' && state === 'connected';
    });
  } catch { return false; }
}

parser.on('data', (line) => {
  const cmd = line.trim().toUpperCase();
  if (cmd === 'AT+CONNECTED?') {
    const connected = isWifiConnected();
    port.write(`+CONNECTED: ${connected ? '1' : '0'}\r\n`);
    port.write('OK\r\n');
  }
});

port.on('error', (err) => {
  console.error('Serial error:', err.message);
});

// Initial WiFi setup if not connected
if (!isWifiConnected()) {
  console.log('No WiFi connection, starting captive portal...');
  execSync('wifi-connect --portal-ssid "MyDevice-Setup"', { stdio: 'inherit' });
}

// Start your HTTP server
const server = http.createServer((req, res) => {
  res.writeHead(200, { 'Content-Type': 'text/plain' });
  res.end('OK');
});

server.listen(3000, () => {
  console.log('Server running on port 3000');
});

