# ESP-IDF ADB client

An optional ESP-IDF component implementing a small Android Debug Bridge (ADB)
client over TCP. Applications can use it to authenticate to a network-enabled
ADB endpoint and open shell services without coupling the component to a
specific Android TV, ASR engine, or voice-command router.

## Status and scope

Version `0.1.0` supports classic ADB TCP connections, RSA-2048 challenge
authentication, and one serialized `shell:` stream at a time. It includes an
Android key-event helper. The component does not start Wi-Fi, discover devices,
pair over Wireless debugging, or provide a background reconnect manager.

Wireless debugging pairing (TLS), USB ADB, concurrent streams, file sync,
shell-v2 exit status, and automated device discovery are not implemented.
Network ADB is unencrypted by this component; use a trusted network and
authorize the client key only on devices you control.

The implementation targets ESP-IDF 6.0.2 or newer, using its PSA Crypto API,
and declares the currently supported ESP32-family targets. CI builds the
protocol test application for ESP32 and ESP32-P4, and the console example for
ESP32-P4. Other target/version combinations should be built in their
consuming project before deployment.

## Examples and tests

[`examples/ws-esp32-p4-wifi6-console-adb-dev`](examples/ws-esp32-p4-wifi6-console-adb-dev/README.md) is the user-facing
Wi-Fi and UART console example. It demonstrates connecting to an ADB endpoint,
running shell commands, and sending Android key events. It persists both the
Wi-Fi credentials and ADB identity in NVS; the example does not enable NVS
encryption, so production devices need an appropriate encryption/provisioning
setup.

[`examples/ws-esp32-p4-wifi6-android-tv-adb-dev`](examples/ws-esp32-p4-wifi6-android-tv-adb-dev/README.md)
is the provider-neutral Android TV remote example for the Waveshare ESP32-P4
WiFi6 board. It uses the same persisted Wi-Fi and ADB setup but adds named
key events, device information, dynamic installed-package listing, app launch
by package name, and raw shell access. It contains no hardcoded apps or
TV-specific product lists.

[`android-companion`](android-companion/README.md) is an optional,
read-only Android app prototype for capability discovery through public
Android APIs. It currently reports device features, TV inputs, and any
channels exposed by the standard TV provider. It does not control or modify
the TV and does not yet communicate with the ESP32.

[`tools-py`](tools-py/README.md) contains a Windows desktop table for reading
the companion's channel export over an explicitly selected ADB connection.
It can edit a local CSV draft, but does not write channel changes back to the
TV.

[`tools-web`](tools-web/README.md) provides a standalone local-first HTML
viewer/editor and optional Web Serial console. With the Android TV ADB example
and Android TV Companion `0.8.0`, it can request a read-only channel export
through the ESP32 and receive the CSV over USB. The browser does not connect
directly to the TV's ADB service or write channel data back to the tuner.
[`tools-py`](tools-py/README.md) remains available as a direct ADB desktop
export path while the Web Serial workflow is validated on hardware.

The [development progress record](ANDROID_TV_ADB_PROGRESS.md) distinguishes
verified board behavior from remaining integration tests.

`test_apps/protocol` is a separate Unity test application for protocol
encoding/decoding and RSA public-key serialization. Keep the tests: they
validate the component itself and are not made redundant by an example.
GitHub Actions is configured to build the test application for ESP32 and
ESP32-P4 and both Wi-Fi examples for ESP32-P4. The workflow validates
compilation only; running the Unity tests still requires flashing the test app
to a board.

On Windows, `test_apps/protocol/esp-build.bat` can select either target and
build, clean, flash, or monitor the test application:

```cmd
cd test_apps\protocol
esp-build.bat -t esp32 build
esp-build.bat -t esp32p4 build
esp-build.bat -t esp32p4 -p COM4 flash monitor
```

## Add the component

As a local component, add the repository to the project's component search path
or use the Component Manager:

```yaml
dependencies:
  Strg-Alt-Entf-0x00/esp-idf-adb:
    version: "^0.1.0"
```

Then add `esp-idf-adb` to the consuming component's `REQUIRES` list and include:

```c
#include "esp_adb.h"
```

The application must initialize its network interface and connect Wi-Fi (or
another IP network) before calling `esp_adb_connect()`.

## Minimal use

```c
esp_adb_config_t config = ESP_ADB_CONFIG_DEFAULT();
esp_adb_client_handle_t adb = NULL;
ESP_ERROR_CHECK(esp_adb_client_create(&config, &adb));

ESP_ERROR_CHECK(esp_adb_connect(adb, "192.168.1.50", ESP_ADB_DEFAULT_PORT));
ESP_ERROR_CHECK(esp_adb_send_key(adb, 3)); /* Android KEYCODE_HOME */

char output[256];
size_t output_length = 0;
ESP_ERROR_CHECK(esp_adb_shell(
    adb, "getprop ro.build.version.release",
    output, sizeof(output), &output_length));

ESP_ERROR_CHECK(esp_adb_client_destroy(adb));
```

Use shell commands only when they are trusted or correctly escaped by the
application. `esp_adb_shell()` sends a raw Android shell service string; it
does not quote or sanitize command text.

## Persistent authentication identity

The default configuration generates an in-memory RSA-2048 identity. It is
discarded when the client is destroyed, so the target may ask the user to
authorize the client again after reboot. For a stable identity, provide both
`load_private_key` and `store_private_key` callbacks. The callbacks exchange a
DER-encoded RSA private key; return `ESP_ERR_NOT_FOUND` from the load callback
when no key is stored yet. The storage backend is application-owned so projects
can select NVS encryption, secure storage, or another suitable mechanism.

Do not log, expose, or commit the private key. If storing it fails, client
creation returns that error instead of silently switching to a temporary key.
Creating a client can synchronously generate an RSA key when storage has no
identity; perform first-time initialization outside latency-sensitive audio
or inference tasks.

## API behavior

- Calls on one client are serialized with a mutex.
- `esp_adb_shell()` waits for the remote shell stream to close before returning.
- `esp_adb_shell_stream()` invokes its callback synchronously while holding the
  client lock; callbacks must not call back into the same client. A callback
  error is returned after the remaining ADB stream is drained.
- `response` is NUL-terminated when `response_size` is nonzero. Set it to
  `NULL` with a size of zero to discard output. A full response buffer is
  drained safely and reported as `ESP_ERR_INVALID_SIZE`.
- A failed or malformed protocol exchange closes the connection.
- `esp_adb_client_destroy()` disconnects and releases all client resources.

## Web app tests

Run the framed Web Serial transfer tests from the repository root:

```powershell
node --test .\tests\*.test.mjs
```

The Node tests use only built-in modules.

## Build protocol tests

The Unity test app checks ADB wire-header encoding/decoding, malformed headers,
RSA public-key serialization, and shell streaming callbacks. Build it for
either target from `test_apps/protocol` with the ESP-IDF extension or:

```powershell
cd test_apps/protocol
idf.py set-target esp32
idf.py build
idf.py set-target esp32p4
idf.py build
```

The test binary must be flashed to a board and run to execute Unity tests.
These tests do not replace integration testing against each supported Android
ADB implementation.

## Security and compatibility notes

- ADB RSA authentication uses SHA-1 as required by the classic ADB protocol.
  This is protocol compatibility, not a general recommendation to use SHA-1.
- Classic ADB authentication proves the client identity to the ADB server but
  does not encrypt the subsequent TCP stream.
- There is no subnet scan or implicit connection to discovered devices.
- The target must enable network ADB and approve the key. Consumer Android TV
  devices may require platform-specific pairing that this release does not
  implement.

## License

This project is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE).
- **Free for noncommercial use**: Personal, educational, research, and hobbyist projects are completely free.
- **Commercial use**: Not permitted without prior authorization. For commercial inquiries, please contact the author via GitHub.
