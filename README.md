# loranet-temp-sensor

ESP32-DEVKITC-32UE reads a DS18B20 on GPIO 4 and publishes temperature to Mosquitto. A simple browser page shows the value via WebSockets.

## Hardware
- ESP32-DEVKITC-32UE (attach external 2.4 GHz antenna)
- DS18B20: DQ → GPIO 4, VCC → 3V3, GND → GND, 4.7 kΩ pull-up DQ→3V3

## Arduino
- Boards: ESP32 by Espressif Systems
- Libraries: OneWire, DallasTemperature, PubSubClient
- Sketch: `firmware/esp32_temp_mqtt/esp32_temp_mqtt.ino`
- Secrets: copy `config.h.example` → `config.h` and edit.
  - (This repo's `.gitignore` excludes `config.h`.)

## Mosquitto (Windows)
Put `broker/myconfig.conf` into `C:\Program Files\mosquitto\myconfig.conf` (or point the service to it).
Open firewall for 1883 and 9001.

Start broker (console):
```
"C:\Program Files\mosquitto\mosquitto.exe" -v -c "C:\Program Files\mosquitto\myconfig.conf"
```

## Web page
Host `www/temp.html` (and optionally `www/mqtt.min.js`) from your PC:
```
powershell -ExecutionPolicy Bypass -File .\scripts\serve_www.ps1
```
Then visit `http://<PC-IP>:8000/temp.html` from your phone on the same Wi-Fi.

The page tries CDN for `mqtt.min.js` and falls back to a local `mqtt.min.js` in the same folder if present.

## Test subscribe (CLI)
```
mosquitto_sub -h 192.168.1.126 -p 1883 -u Mot -P Buster02 -t "loranet/up/#" -v
```

## Repo creation (GitHub)
Initialize and push (with GitHub CLI):
```
git init
git add .
git commit -m "Initial commit: ESP32 temp → MQTT with WS dashboard"
gh repo create loranet-temp-sensor --public --source=. --remote=origin --push
```

Without GitHub CLI (replace YOURUSER and create an empty repo on GitHub first):
```
git init
git add .
git commit -m "Initial commit: ESP32 temp → MQTT with WS dashboard"
git branch -M main
git remote add origin https://github.com/YOURUSER/loranet-temp-sensor.git
git push -u origin main
```

## .gitignore
- excludes `firmware/esp32_temp_mqtt/config.h` (secrets)
- do not commit `broker/passwd` or `broker/acl`
