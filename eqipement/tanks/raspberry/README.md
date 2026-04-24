# Raspberry Pi Zero — Tank Bridge

The Raspberry Pi Zero acts as the network bridge between the MegaPi Arduino (UART) and the MQTT broker. It also runs a heartbeat and handles WiFi provisioning.

---

## Setup

**Step 1 — Enable UART**

In `raspi-config → Interfacing Options → Serial`:
- Disable the login shell over serial
- Enable the serial port hardware

**Step 2 — Install WiFi provisioning**

Install [wifi-connect](https://github.com/balena-os/wifi-connect) so the Pi can serve a captive portal when no WiFi is configured.

**Step 3 — Deploy server.js**

```bash
# Clone or copy server.js to /home/pi/
cd /home/pi
npm install serialport @serialport/parser-readline mqtt
```

**Step 4 — Create systemd service**

`/etc/systemd/system/iot-app.service`:

```ini
[Unit]
Description=BattleDrome Tank Bridge
After=network-online.target
Wants=network-online.target

[Service]
User=pi
WorkingDirectory=/home/pi
ExecStart=/usr/bin/node /home/pi/server.js
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable iot-app.service
sudo systemctl start iot-app.service
```

---

## What server.js does

- Opens `/dev/ttyS0` at 115200 baud (UART to MegaPi via flex cable)
- Connects to `broker.hivemq.com:1883` and publishes to `battledrome/tanks/{hostname}/events`
- Subscribes to `battledrome/tanks/{hostname}/commands` and forwards any received message to Arduino via UART
- Sends a `heartbeat` event every **5 seconds** so the central server knows the tank is alive
- Exposes an HTTP health endpoint on port **3000** (`GET /` → `200 OK`)
- On WiFi loss at startup, launches `wifi-connect --portal-ssid "MyDevice-Setup"`

---

## Sending commands to the tank

Any MQTT client can publish to the commands topic to update a game-state variable on the Arduino:

```bash
mosquitto_pub -h broker.hivemq.com \
  -t "battledrome/tanks/{hostname}/commands" \
  -m '{"timestamp":0,"event":{"type":"command","param":"health","value":80}}'
```

See `eqipement/README.md` for the full command spec.
