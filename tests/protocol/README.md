# ADB Protocol Unity Test Suite

This test application validates the core ADB protocol implementation, wire-packet framing, RSA public key export, and shell stream handling using the Unity test framework.

## Test Coverage

- **Wire-Packet Encoding & Decoding**: Little-endian integer conversion, header checksums, magic word validation (`~command`).
- **Malformed Packet Rejection**: Rejection of corrupted magic words, out-of-bounds payload lengths, invalid CRC checksums, and NULL pointers.
- **Delayed CLOSE Packet Handling**: Verification that delayed/stale `CLOSE` packets from prior streams are properly identified and filtered without dropping active streams.
- **RSA Authentication Key Formatting**: Generation and formatting of Android-compatible ADB public key structures (Base64 encoding, space separator, client identity, NUL termination).
- **Shell Stream Callbacks**: Synchronous data delivery, stream draining on error, and buffer boundary enforcement.

## Building and Flashing

Targeting standard **ESP32**:

```cmd
esp-build.bat -t esp32 build
esp-build.bat -t esp32 -p COM4 flash monitor
```

Targeting **ESP32-P4**:

```cmd
esp-build.bat -t esp32p4 build
esp-build.bat -t esp32p4 -p COM4 flash monitor
```

Or using standard `idf.py`:

```powershell
idf.py set-target esp32
idf.py build
idf.py -p COM4 flash monitor
```
