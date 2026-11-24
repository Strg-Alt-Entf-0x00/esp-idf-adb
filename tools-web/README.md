# TV Channel Manager Web App

A standalone, local-first HTML5 application for inspecting, searching, filtering, and editing Android TV channel lists exported via ADB.

## Features

- **Local-first & offline**: Runs entirely inside the browser without remote servers or external dependencies. Secured with strict Content Security Policy (`default-src 'none'`).
- **Web Serial connection**: Connects directly to the ESP32 USB serial console (115,200 baud) using the browser's native Web Serial API.
- **Framed TV export**: Triggers `tv-export` on the firmware and receives stream-framed UTF-8 CSV chunks (`TVCSV_BEGIN:<id>:<size>`, `TVCSV_END:<id>`, `TVCSV_ABORT:<id>`).
- **Data validation**: Validates byte counts, UTF-8 integrity, request IDs, and CSV column structure before loading data.
- **Search & filtering**: Real-time filtering by channel name, number, broadcast type (`DVB-T`, `DVB-C`, `DVB-S`), service type, visibility, and lock state.
- **CSV draft export**: Download modified drafts locally. *(Note: Android TV does not provide public tuner write-back APIs; this edits a local draft).*
- **Modern dark theme**: Clean, high-contrast dark user interface optimized for readability.

## Requirements

- A modern desktop Chromium-based browser supporting the Web Serial API (Google Chrome, Microsoft Edge, Brave, Opera).
- USB connection to an ESP32 running the `ws-esp32-p4-wifi6-android-tv-adb-dev` firmware.
- Android TV device with network ADB enabled and the `android-tv-companion` APK (version 0.8.0+) installed with `READ_TV_LISTINGS` permission granted.

## Usage

1. Open `tv_channel_manager.html` directly in your browser (`file:///...`).
2. Click **Connect ESP32 Serial** and select the ESP32's serial port at 115,200 baud.
3. Ensure the ESP32 is connected to your TV (`tv-connect <tv-ip>`).
4. Click **Export TV Channels**. The firmware triggers the companion export, validates the file size, and streams the data over Web Serial into the table.
5. Search, reorder, or filter the channels, and click **Download CSV draft** to save your changes locally.

## Testing

The Web Serial parser and stream framing logic are validated using automated unit tests run with Node.js built-in test runner:

```powershell
node --test tests/web_channel_transfer.test.mjs
```
