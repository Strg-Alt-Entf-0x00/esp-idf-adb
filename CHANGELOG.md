# Changelog

## Unreleased

- Added a Waveshare ESP32-P4 WiFi6 Android TV remote example using the onboard
  ESP32-C6 over ESP-Hosted/SDIO with dynamic package listing, device information,
  bounded volume requests, generic key input, and framed channel export.
- Made Wi-Fi status shared across the console and event task with atomic state,
  and clarified that an ADB shell close does not confirm Android action success.
- Added console-managed Wi-Fi setup and persistent Wi-Fi credentials and ADB
  RSA identity in NVS; passwords are entered at a hidden prompt.
- Added CI builds for the protocol test app on ESP32 and ESP32-P4 and for the
  Android TV Wi-Fi example on ESP32-P4.
- Improved error handling for PSA private-key export and NVS initialization.
- Documented that the example's NVS data is not encrypted by default and that
  production provisioning must configure encryption separately.
- Added a standalone local-first HTML5 channel CSV viewer/editor with a dark
  default theme, tabbed workspace layout, virtual TV remote controls, dynamic app launcher,
  single connect toggle with firmware handshake verification, Web Serial console, and framed
  ESP32 channel-list export.
- Lowered stale close stream log level in ADB client to debug to eliminate spurious warnings
  during rapid sequential shell executions.
- Added an Android TV companion application with a one-shot, DUMP-protected
  receiver for headless read-only export requests triggered via `tv-export`.

## 0.1.0 - 2026-10-24

- Initial ESP-IDF component extraction.
- Classic ADB TCP handshake with RSA-2048 authentication and pluggable key
  persistence.
- Serialized legacy `shell:` commands and numeric Android key events.
- ADB packet header unit tests and component usage documentation.
