# Android TV Web Suite

A standalone, local-first HTML5 application providing a virtual remote control, dynamic app launcher, live serial console, and channel list manager for Android TV devices controlled via ESP32 ADB.

## Features

- **Local-first & offline**: Runs entirely inside the browser without remote servers, telemetry, or external dependencies. Secured with strict Content Security Policy (`default-src 'none'`).
- **State-aware Web Serial connection**: Connects directly to the ESP32 USB serial console (115,200 baud) with an adaptive icon button colored dynamically by state (Red = disconnected, Amber = verifying, Green = verified). Automatically verifies Android TV ADB firmware signatures to reject foreign or unverified devices.
- **Unified monochrome UI**: Clean, professional design with consistent monochrome vector icons and native tooltips across all controls.
- **Docked bottom serial console**: Full-width persistent bottom terminal featuring a scrollable live log window, directly followed by the interactive command prompt and quick action icon buttons (Send, Clear).
- **Tabbed workspace**:
  - **TV Channels**: Triggers framed TV export from the companion app, displays responsive channel statistics, real-time search & filters (`DVB-T`, `DVB-C`, `DVB-S`, service types, lock states), and local CSV draft download.
  - **Remote & Apps**: Sleek D-Pad navigation, universal Android TV media keys (Home, Back, Volume, Mute, Voice Assist), and dynamic package listing with one-click app launch/stop.
- **Framed TV export**: Triggers `tv-export` on the firmware and receives stream-framed UTF-8 CSV chunks (`TVCSV_BEGIN:<id>:<size>`, `TVCSV_END:<id>`, `TVCSV_ABORT:<id>`).
- **Data validation**: Validates byte counts, UTF-8 integrity, request IDs, and CSV column structure before loading data.
- **Modern dark theme**: Clean, high-contrast user interface optimized for readability.

## Requirements

- A modern desktop Chromium-based browser supporting the Web Serial API (Google Chrome, Microsoft Edge, Brave, Opera).
- USB connection to an ESP32 running the `ws-esp32-p4-wifi6-android-tv-adb-dev` firmware.
- Android TV device with network ADB enabled and the `android-tv-companion` APK (version 0.8.0+) installed with `READ_TV_LISTINGS` permission granted (for channel exports).

## Usage

1. Open `android_tv_suite.html` directly in your browser (`file:///...`) or `index.html`.
2. Click **Connect to ESP32** and select the ESP32's serial port at 115,200 baud.
3. Once verified, connect the ESP32 to your TV (`tv-connect <tv-ip>`).
4. Switch tabs to control your TV, launch apps, inspect logs, or export channels.

## Testing

The Web Serial parser, stream framing logic, tab navigation, and UI state toggles are validated using automated unit tests run with Node.js built-in test runner:

```powershell
node --test tests/web_channel_transfer.test.mjs
```
