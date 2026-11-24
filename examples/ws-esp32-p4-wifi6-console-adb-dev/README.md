# ADB console example

This ESP-IDF application uses the Waveshare ESP32-P4 WiFi6 board's onboard
ESP32-C6 Wi-Fi coprocessor over ESP-Hosted/SDIO and provides a UART console
for connecting to an Android device's classic network ADB server. It is the
user-facing integration example; protocol unit tests are kept separately in
`test_apps/protocol`.

## Configure and build

Use ESP-IDF 6.0.2 or newer. From this directory:

```powershell
esp-build.bat
```

The project defaults select ESP32-P4, ESP-Hosted over SDIO, and the Waveshare
P4 WiFi6 board pin configuration. Wi-Fi credentials are not build-time
configuration: enter them from the UART console so they never go into
`sdkconfig` or the repository. The board's ESP32-C6 must have compatible
ESP-Hosted coprocessor firmware installed.

The Windows helper also supports clean builds and flashing:

```cmd
esp-build.bat clean-build
esp-build.bat -p COM4 flash monitor
```

Alternatively, run `idf.py build` in an activated ESP-IDF environment.

## Console commands

```text
wifi-set <ssid>
wifi status
wifi connect
wifi-forget
adb-connect <host> [port]
adb-status
adb-key <android-keycode>
adb-shell <command>
adb-disconnect
```

Example:

```text
wifi-set MyNetwork
Wi-Fi password (input hidden; Enter for open network): [type password]
wifi status
adb-connect 192.168.1.50
adb-shell "getprop ro.build.version.release"
adb-key 3
adb-disconnect
```

The default ADB port is 5555. The target must have classic network ADB enabled
and authorize the client's RSA key. The example stores its DER-encoded RSA
private key in NVS, so the same identity is used after reboot and the target
does not need to authorize a newly generated key each time.

`wifi-set` stores the SSID and password in the ESP-IDF default NVS partition
and connects using the saved credentials after reboot. The password is read
at a hidden prompt (not in the command line/history); use `wifi-forget` to
remove it. `wifi-set` starts the connection automatically; `wifi connect`
retries after a failure. Keep terminal local echo disabled while entering the
password.

Both Wi-Fi credentials and the ADB private key are stored in NVS without NVS
encryption. They are not included in the repository or firmware image, but
someone with physical access to the flash may be able to read them. Production
devices should enable and provision NVS/flash encryption according to their
manufacturing and recovery process. If NVS reports corruption or an incompatible
version, this example stops rather than erasing saved credentials and identity.

The console shows command results and actionable errors. Debug-level logs are
disabled by default, and neither Wi-Fi passwords nor private ADB keys are
printed. `adb-shell` sends raw shell text and does not escape or sanitize it;
use only commands you trust. Classic network ADB is unencrypted, so use a
trusted network and authorize the client only on devices you control.
