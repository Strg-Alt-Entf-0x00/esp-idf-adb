# Android TV ADB remote example

This ESP-IDF application uses the Waveshare ESP32-P4 WiFi6 board and its
ESP32-C6 Wi-Fi coprocessor to control a network-enabled Android TV or Android
device over classic ADB.

The example is intentionally provider-neutral and does not contain a list of
apps, streaming services, TV models or remote layouts. Apps are discovered
from the connected device, package names are supplied by the app list, and
key names map to Android's standard numeric key codes.

## Build

The verified configuration uses ESP-IDF 6.0.2:

```powershell
esp-build.bat
```

Flash and monitor from Windows:

```cmd
esp-build.bat -p COM4 flash monitor
```

## Console commands

```text
wifi setup
wifi status
wifi connect
wifi forget
tv-connect <host> [port]
tv-status
tv-disconnect
tv-key <name|android-keycode>
tv-volume <up|down> [steps 1-10]
tv-app-list
tv-app-launch <package.name>
tv-app-stop <package.name>
tv-device-info
tv-export
tv-shell <command>
```

Once Wi-Fi reports `connected`, continue with:

```text
tv-connect 192.168.1.50
tv-device-info
tv-export
tv-app-list
tv-app-launch com.example.discovered.package
tv-app-stop com.example.discovered.package
tv-key home
tv-volume up 3
tv-disconnect
```

Supported named keys are `home`, `back`, `up`, `down`, `left`, `right`,
`center`, `ok`, `menu`, `search`, `play-pause`, `volume-up`, `volume-down`,
`mute`, `power`, `assistant`, and `voice-assist`. These last two map to
Android's distinct `KEYCODE_ASSIST` and `KEYCODE_VOICE_ASSIST` codes. Numeric
Android key codes are accepted as well, so this example also supports keys
not included in the convenient name table.

The target must have classic network ADB enabled and authorize this ESP32's
RSA key. The identity is stored in NVS. Wi-Fi credentials are also stored in
NVS; neither is written to source or the firmware image.

`tv-export` requires Android TV Companion `0.8.0` with `READ_TV_LISTINGS`
granted. It requests a one-shot, read-only provider export, checks the TV's
generated CSV size (maximum 1 MiB), and streams it over UART using a framed
transfer. The companion also saves the CSV in the TV's Downloads folder.

Use `wifi setup` to enter the SSID and password interactively; it saves them
to NVS and starts the connection. Password characters are not echoed by the
firmware; disable local echo in the serial terminal if it is enabled. Use
`wifi status` to check the connection. Wi-Fi network scanning is not currently
implemented, so enter the SSID manually.

`tv-app-list` queries `pm list packages` on the connected device, including
system packages; no package IDs are hardcoded. A listed package is not
necessarily a user-launchable app: some packages have no launcher activity,
and launch behavior can vary by Android build. Shell output is limited to
16 KiB per command; larger responses are reported as incomplete rather than
silently treated as complete.

`tv-app-stop <package.name>` requests Android to force-stop a validated
package. Android/OEM policy can vary, and this ADB shell-v1 client cannot
confirm the stop through an exit status; verify the result on the device.

On Android TV builds that expose it, `tv-shell dumpsys tv_input` can inspect
framework input metadata without switching sources. Its output and labels are
manufacturer-specific and may not identify exact tuner standards such as
DVB-C or DVB-S; source switching is not currently a dedicated console command.

Useful read-only discovery commands on Android devices include:

```text
tv-shell getprop ro.product.manufacturer
tv-shell getprop ro.product.model
tv-shell getprop ro.build.version.release
tv-shell pm list packages
tv-shell dumpsys -l
tv-shell cmd -l
tv-shell service list
```

`dumpsys -l` lists diagnostic services; `cmd -l` and `service list` list
available services/interfaces, not guaranteed features or supported commands.
Their output varies by Android version and OEM. Query a specific `dumpsys`
service rather than dumping everything. See the Android
[`dumpsys` documentation](https://developer.android.com/tools/dumpsys).

`tv-volume up|down [steps]` requests between 1 and 10 discrete Android volume
key events. It does not set an absolute level or guarantee a particular
volume change; the TV controls its own volume behavior. A closed ADB shell
stream does not confirm that Android executed a key request or launched an
app, because this component does not implement shell-v2 exit status.

The first version uses an explicitly supplied host and port. It does not claim
automatic TV discovery: mDNS only helps when a device advertises a compatible
ADB service, and this needs validation with the actual TV. Wireless Debugging
pairing and its TLS connection flow are not supported by this classic ADB
client.

Classic ADB is unencrypted. Use it only on a trusted network and with devices
you control. `tv-shell` sends raw shell text and does not escape or sanitize
commands. Shell commands run synchronously until the remote stream closes, so
long-running commands (for example, continuous log output) can occupy the
serial console indefinitely; avoid them.

An optional read-only Android companion application is documented in
[`android-tv-companion`](../../android-tv-companion/README.md). It uses public
Android APIs to inspect TV inputs and the standard channel provider, and integrates
with the ESP32 firmware via `tv-export` to deliver channel listings over serial
without modifying TV tuner data or requiring root/OEM unlocking.
