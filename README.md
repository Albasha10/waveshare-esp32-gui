# Waveshare ESP32-S3 Web Server (ESP32_GUI) – Modbus TCP / MQTT / TagoIO

Firmware and web pages for the **Waveshare ESP32-S3-POE-ETH-8DI-8DO** board used at SHRDC Malaysian Smart Factory 4.0.
The board reads an Omron NX1P2 PLC over **Modbus TCP** (Ethernet), shows the data on its own web pages, publishes it over **MQTT** (for Node-RED) and sends it directly to **TagoIO** over HTTPS.

## Features
- **Modbus TCP client** (W5500 Ethernet): PLC IP/port/unit ID, polling interval, Test Connection, station preset (22 signals), batched reads
- **Dashboard** with up to 10 configurable metric cards
- **KPI page**: choose which signal means running / start / stop / cycle start / cycle end / reject; efficiency formula typed by the user (availability, quality, OEE)
- **Signal Status page**: board DI/DO and PLC signals with time in current state
- **TagoIO page**: device token, region, per-signal send switch, ON/OFF words, TagoIO variable names
- **MQTT**: per-signal publish switch (Node-RED flow in `extras/`)
- Also: RS485 Modbus, SD card logging, Telegram bot (from the original firmware)

Pages: Dashboard · KPI · Signal Status · IoT Config · Modbus TCP · TagoIO · SD Card · Telegram

## Folder layout
| Path | What it is |
| --- | --- |
| `ESP32_GUI/ESP32_GUI.ino` | Firmware (Arduino sketch) |
| `ESP32_GUI/data/` | Web pages and config files that go into the board's LittleFS |
| `ESP32_GUI/secrets.example.h` | Template for the Wi-Fi credentials |
| `ESP32_GUI/data/users.example.json` | Template for the web login accounts |
| `extras/nodered_station_flow.json` | Node-RED flow (MQTT → Dashboard 2.0 + TagoIO) |
| `CHANGELOG.md` | Every change made, newest first |

## First-time setup (credentials are not in git)
1. Copy `ESP32_GUI/secrets.example.h` to `ESP32_GUI/secrets.h` and fill in the Wi-Fi name and password.
2. Copy `ESP32_GUI/data/users.example.json` to `ESP32_GUI/data/users.json` and set the login username/password.

## Build and flash
**Arduino IDE 2** with the ESP32 core 3.3.x. Board settings:
- Board: **ESP32S3 Dev Module**
- Flash Size: **4MB**
- Partition Scheme: **Huge APP (3MB No OTA/1MB SPIFFS)**
- USB CDC On Boot: **Enabled**

Libraries: WebSockets (Links2004), PubSubClient, ModbusMaster, ArduinoJson (v7), UniversalTelegramBot.

### Web pages (LittleFS)
Arduino IDE 2 has no "Sketch Data Upload", so build the image and flash it with esptool:
```
mklittlefs -c ESP32_GUI/data -b 4096 -p 256 -s 0xE0000 littlefs.bin
esptool.py --chip esp32s3 --port <PORT> write_flash 0x310000 littlefs.bin
```
Upload the sketch first, then the LittleFS image. Re-flash the image whenever a file in `data/` changes.

## Default network (mini station)
| Device | Address |
| --- | --- |
| Omron NX1P2 PLC | 192.168.250.7, port 502, unit ID 1 |
| Waveshare board (Ethernet) | 192.168.250.10 |
| TagoIO (Europe region) | https://api.eu-w1.tago.io/data |

Station map: coils 0–19 (ES0, mode selectors, push buttons, sensors B1–B4, lamps, relays, tower light), holding registers 0–1 (analog inputs). Load it with **Load station preset** on the Modbus TCP page.

## Open items
- TagoIO page status line can be blank after saving a token (check after next flash)
- Confirm which PLC signal should mean "machine running" on the KPI page
- Enter the ideal cycle time to show OEE

Author: Fateh Albasha (intern, Oct 2026), based on the original SHRDC ESP32_GUI firmware.
