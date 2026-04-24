Step 1: 
Following Raspberry documentation (raspi-config) enable UART port 

Step 2: [tbd]
Install [Wifi-client](https://github.com/balena-os/wifi-connect)  to be able to connect to your wifi via web interface. 

Step 3:
Clone server.js and npm.json and install it 

Step 4: 
create iot-app.service file in /etc/systemd/system 
```
[Unit]
Description=IoT App
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
5: enable and start service: 
```
sudo systemctl daemon-reload
sudo systemctl enable iot-app.service
sudo systemctl start iot-app.service
```
