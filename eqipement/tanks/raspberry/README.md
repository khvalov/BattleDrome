# Raspberry Pi Zero — Tank Bridge

The Raspberry Pi Zero bridges the MegaPi Arduino (UART `Serial2`) and the MQTT broker. It forwards telemetry upstream and commands downstream, and keeps the tank visible on the dashboard via a periodic heartbeat.

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

| Behaviour | Detail |
|:---|:---|
| Serial port | `/dev/ttyS0` at 115200 baud (UART to MegaPi `Serial2`) |
| Tank identity | Uses `os.hostname()` as the routing key in MQTT topics (stored as `TANK_ID`) |
| Publishes to | `battledrome/tanks/{hostname}/events` (`telemetry`, `fire`, `system`, `error`) |
| Subscribes to | `battledrome/tanks/{hostname}/commands` |
| Telemetry enrichment | Before forwarding `telemetry` or `fire` events to MQTT, injects `ip` (first non-loopback IPv4) and `hostname` (`os.hostname()`) into `event.data` |
| Command forwarding | Any message received on the commands topic is written verbatim to Arduino via UART |
| Heartbeat | `system / heartbeat` published every **5 s** so the central server doesn't mark the tank offline |
| WiFi fallback | If no WiFi at startup, launches `wifi-connect --portal-ssid "MyDevice-Setup"` |
| Health endpoint | `GET http://<pi>:3000/` → `200 OK` |

---

## Sending commands to the tank

```bash
mosquitto_pub -h broker.hivemq.com \
  -t "battledrome/tanks/{hostname}/commands" \
  -m '{"timestamp":0,"event":{"type":"command","param":"health","value":80}}'
```

Valid `param` values: `health`, `ammo`, `ammoLevel`, `fireSpeed`, `immunable`.  
See `eqipement/README.md` for the full message spec.
