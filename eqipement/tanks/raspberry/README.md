# Raspberry Pi Zero — Tank Bridge

The Raspberry Pi Zero bridges the MegaPi Arduino (UART `Serial2`) and the MQTT broker. It forwards telemetry upstream and commands downstream, and keeps the tank visible on the dashboard via a periodic heartbeat.

---

## Setup

**Step 1 — Enable UART**

In `raspi-config → Interfacing Options → Serial`:
- Disable the login shell over serial
- Enable the serial port hardware

Then add the following line to `/boot/config.txt` and reboot:

```
enable_uart=1
```

> **Why this matters (RPi Zero W):** without `enable_uart=1`, GPIO14/15 are routed to the *mini UART* (`/dev/ttyS0`) whose TX baud rate drifts with the CPU core clock. The Arduino silently rejects the garbled bytes, so commands never apply even though `Serial write OK` is logged. `enable_uart=1` switches GPIO14/15 to the stable PL011 UART (`/dev/ttyAMA0`) and moves Bluetooth to the mini UART instead. `server.js` uses `/dev/serial0`, which is the system symlink that always follows this setting automatically.

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
| Serial port | `/dev/serial0` at 115200 baud (symlink → PL011 UART on GPIO14/15 with `enable_uart=1`) |
| Tank identity | Uses `os.hostname()` as the routing key in MQTT topics (stored as `TANK_ID`) |
| Publishes to | `battledrome/tanks/{hostname}/events` (`telemetry`, `fire`, `rfid`, `system`, `error`) |
| Subscribes to | `battledrome/tanks/{hostname}/commands` |
| Telemetry enrichment | Before forwarding `telemetry`, `fire`, or `rfid` events to MQTT, injects `ip` (first non-loopback IPv4) and `hostname` (`os.hostname()`) into `event.data` |
| Command forwarding | Any message received on the commands topic is written verbatim to Arduino via UART |
| Heartbeat | `system / heartbeat` published every **5 s** so the central server doesn't mark the tank offline |
| WiFi fallback | If no WiFi at startup, launches `wifi-connect --portal-ssid "MyDevice-Setup"` |
| Health endpoint | `GET http://<pi>:3000/` → `200 OK` |

---

## ⚠️ Hardware Limitation — Serial2 RX Buffer (64 bytes)

The ATmega2560's hardware UART RX buffer is **64 bytes**. Any message written to the serial port that exceeds 64 bytes is silently truncated when the Arduino main loop is briefly busy (telemetry TX, SPI reads), which corrupts the JSON and causes the command to be silently ignored.

`server.js` strips the `timestamp` field before every serial write, sending only `{"event":{...}}`. This keeps all message types well within the limit. **Do not add fields to serial-bound messages without verifying the resulting byte count stays below 64.**

---

## Sending commands to the tank

```bash
mosquitto_pub -h broker.hivemq.com \
  -t "battledrome/tanks/{hostname}/commands" \
  -m '{"timestamp":0,"event":{"type":"command","param":"health","value":80}}'
```

Valid `param` values: `health`, `ammo`, `ammoLevel`, `fireSpeed`, `immunable`.  
See `eqipement/README.md` for the full message spec.

---

## FPV Camera (mediamtx)

Each tank runs a Raspberry Pi Camera module streamed via [mediamtx](https://github.com/bluenviron/mediamtx) over WebRTC. The dashboard provides an FPV button on each tank card to view the live feed.

**Step 1 — Install mediamtx**

Download the ARM binary for Pi Zero:

```bash
cd /home/pi
wget https://github.com/bluenviron/mediamtx/releases/download/v1.19.2/mediamtx_v1.19.2_linux_armv6.tar.gz
tar xzf mediamtx_v1.19.2_linux_armv6.tar.gz
rm mediamtx_v1.19.2_linux_armv6.tar.gz
```

**Step 2 — Configure mediamtx**

Create or edit `/home/pi/mediamtx.yml`:

```yaml
paths:
  cam:
    source: rpiCamera
    rpiCameraWidth: 640
    rpiCameraHeight: 360
    rpiCameraFPS: 25
    rpiCameraBitrate: 2000000
    rpiCameraCodec: hardwareH264
```

The WebRTC player is then available at `http://<tank-ip>:8889/cam`.

**Step 3 — Create systemd service**

`/etc/systemd/system/mediamtx.service`:

```ini
[Unit]
Description=MediaMTX Camera Stream
After=network-online.target
Wants=network-online.target

[Service]
User=pi
WorkingDirectory=/home/pi
ExecStart=/home/pi/mediamtx /home/pi/mediamtx.yml
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable mediamtx.service
sudo systemctl start mediamtx.service
```

**Verify:**

```bash
systemctl status mediamtx.service
# Open http://<tank-ip>:8889/cam in a browser to confirm the stream
```
