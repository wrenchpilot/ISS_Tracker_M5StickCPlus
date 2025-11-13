# ISS Tracker — M5StickC Plus

A compact ISS tracker project for the M5StickC Plus (ESP32-based) using a simple web UI. The sketch and a small web interface are included in this repository so the device can show status and be configured from a browser on your local network.

## What this repository contains

- `ISS_Tracker_M5StickCPlus.ino` — Main Arduino sketch for the M5StickC Plus.
- `user_settings.h` — Project configuration: Wi‑Fi credentials, API keys and other user-editable settings.
- `data/` — Static web files (served from LittleFS/SPIFFS):
  - `index.html` — Main web UI.
  - `setup.html` — Setup/configuration UI.
  - `app.js`, `setup.js` — JavaScript for the web UI.
  - `style.css` — Styling for the web pages.

> Note: The web files in `data/` are intended to be uploaded to the device file system (LittleFS / SPIFFS) alongside the sketch.

## Features

- Runs on M5StickC Plus (ESP32) hardware
- Serves a small web UI for configuration and status via the onboard Wi‑Fi
- Editable settings in `user_settings.h` for Wi‑Fi, API tokens, and behavior

## Requirements

- macOS, Windows, or Linux development machine
- Arduino IDE (or PlatformIO) with ESP32 (M5StickC) board support installed
- M5StickC Plus (ESP32) device
- USB cable to program the device
- Libraries referenced by the sketch (install via Arduino Library Manager or PlatformIO):
  - M5StickC / M5Stack (or the library used by the sketch)
  - WiFi / WiFiClient / HTTPClient (standard ESP32 networking libraries)
  - Any JSON library the sketch uses (e.g., ArduinoJson) — the sketch will #include those headers if required

If you open the sketch and the compiler reports missing libraries, install them through the Arduino IDE > Sketch > Include Library > Manage Libraries... or via PlatformIO's lib_deps.

- ## Setup — Arduino IDE (recommended quick start)

### Initial setup — device AP (first boot)

- On first boot (or when no Wi‑Fi is configured) the device may start in Access Point (AP) mode and broadcast a Wi‑Fi network so you can configure it. Connect your computer or phone to that network before trying to open the device web UI on your local LAN.

- How to proceed:

  1. Open the Serial Monitor at the baud rate used by the sketch and watch the startup messages. The device commonly prints the AP SSID (network name) and any default password or the IP address to use while in AP mode.

  2. On your computer or phone, join the device's AP network (for example an SSID like `ISS-Setup-xxxx` or similar — check the serial output for the exact name).

  3. With your device connected to the board's AP, point your browser to the device IP shown in serial output. Common AP-mode IP addresses for ESP32 devices are `http://192.168.4.1` or the IP printed by the device. The setup page is usually available at `/setup.html` (for example `http://192.168.4.1/setup.html`).

  4. Use the setup UI to enter your home Wi‑Fi SSID and password. After saving, the device will attempt to join your network and will print its assigned local IP address to the serial monitor — then use that IP to access the main UI (e.g. `index.html`).

Note: Exact AP SSID, default passwords, and IP addresses depend on the sketch. If the sketch defines a custom AP name or password in `user_settings.h`, consult that file or the serial output.

- Install ESP32 board support in Arduino IDE:

  - File > Preferences, add the ESP32 boards URL if you haven't already: <https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json>
  - Tools > Board > Boards Manager → search for "esp32" and install.
  - Select the appropriate M5StickC / ESP32 board under Tools > Board.

- Install required libraries using the Library Manager (see "Requirements").

- Edit `user_settings.h` and set your Wi‑Fi SSID, password, and any API keys or preferences.

- Upload the web files in `data/` to the device filesystem (LittleFS / SPIFFS):

  - Install the "ESP32 Sketch Data Upload" (LittleFS) plugin or use PlatformIO's uploadfs. After installation, use Tools > "ESP32 Sketch Data Upload" to upload the `data/` folder to the board.

  - If you use PlatformIO, add a `platformio.ini` entry for filesystem upload and use `platformio run --target uploadfs` (see PlatformIO docs for LittleFS/LFS upload).

- Compile and upload `ISS_Tracker_M5StickCPlus.ino` to your M5StickC Plus.

- Open the Serial Monitor (Tools > Serial Monitor) at the baud rate used in the sketch. The device will typically print its local IP address after connecting to Wi‑Fi.

- Once the device has joined your Wi‑Fi you can usually browse to `http://iss.local/` (mDNS). If your network or client does not support mDNS, use the printed IP address instead — for example `http://192.168.1.123` — to access the web UI (`index.html`) or `http://<device-ip>/setup.html` to view the setup page.

## Customization

- Edit `user_settings.h` to change Wi‑Fi credentials and other configuration options before uploading.
- Update the web files in `data/` to change the web UI. After changing them, re-upload the `data/` filesystem image to the device.

## Troubleshooting

- If the device does not connect to Wi‑Fi, double-check SSID/password in `user_settings.h` and the serial monitor output.
- If libraries are missing, the compiler will list them in the error output — install those via Arduino Library Manager or PlatformIO.
- If you can't reach the web UI, ensure the device obtained an IP address (check serial monitor) and that your computer is on the same LAN.

## Development notes

- The `data/` folder is intended for LittleFS/SPIFFS. Do NOT include `data/` in standard firmware uploads — use the filesystem upload tool.
- Keep `user_settings.h` out of public repositories if it contains secrets (Wi‑Fi credentials or API keys). Consider adding it to `.gitignore`.
