# Android Companion

An optional, read-only Android companion for the ESP32 ADB project. The first
prototype checks what the Android TV Framework exposes through public APIs:

- device and Android version information;
- Leanback, Live TV, and touchscreen feature flags;
- TV inputs, their framework types, and connection states;
- all channel rows visible through `TvContract.Channels`, with local search
  and sorting by channel number, name, or provider-returned order.

This is a capability-discovery prototype, not a remote-control bridge yet. It
does not communicate with the ESP32, tune a channel, change a source, or write
to the TV provider.
The framework type describes how an input service registered itself; it is
not proof that a physical tuner or connector is present. Unknown numeric types
are shown as vendor-specific rather than being guessed.

## Safety and privacy

- The app declares no network permission. It requests Android's read-only
  `READ_TV_LISTINGS` permission only after the user taps the channel-access
  button; it never prompts on launch.
- It has no background service and does not persist scan results automatically.
- A CSV is written only after the user explicitly requests export, either in
  the app or through the PC manager. On Android 10+ it is saved to the public
  Downloads folder; on older versions it uses the app-specific Documents
  folder. It contains the public TV-provider fields for each row. It is for
  viewing/editing and is not a TV-import format.
- Version `0.8.0` adds a one-shot, ADB-shell-only export receiver for the
  desktop manager. It does not launch the activity or run a background
  service; it uses the same user-granted, read-only channel permission.
- It displays the device build fingerprint and build identifiers locally to
  help distinguish model and firmware variants; it does not transmit them.
- Channel data is queried locally. It is not uploaded or transmitted by this
  app.
- It does not insert, update, delete, or tune TV data.
- If Android or the manufacturer denies access, the app reports that result;
  it does not attempt a permission bypass.
- Uninstalling the app removes the prototype without changing TV settings or
  channel data.

On the tested TCL/BeyondTV running Android 11, the app reported six Android
TV inputs and initially zero rows from the standard channel provider in
version `0.2.0`, without a TV-listings permission. After the user granted the
read-only permission in version `0.3.0`, the provider returned 1,123 channel
rows. Version `0.4.0` grouped the complete count by input ID: 1,114 rows
belong to the TCL tuner input, five have no input ID, and four other inputs
contribute one row each. This association does not establish that every row
is current, visible, unique, or a broadcast channel.

Version `0.5.0` makes all provider rows viewable in a local, searchable list;
it no longer truncates display at 200 rows. The list can be sorted by channel
number, name, or the provider's original return order. The latter is not
guaranteed to be sorted. The standard provider has a `COLUMN_TYPE` broadcast
system field, but earlier CSV exports did not include it. Version `0.7.0`
also exports that field, service type, broadcast visibility, browsable, and
locked flags so the values supplied by the device can be inspected rather
than inferred from the input ID. The field may still be empty or generic if
the TV provider does not populate it. See Android's
[`TvContract.Channels`](https://developer.android.com/reference/android/media/tv/TvContract.Channels)
reference. Channel names are shown on the TV; an explicit CSV export also
saves them to Downloads. Individual names are not copied into development
logs. After inspection the app is force-stopped and the TV returns to its TCL
app. The inputs included AV (reported by the framework as Composite), HDMI 1,
HDMI 2, TV, and two Google Play Movies virtual inputs. The virtual inputs are
not evidence of additional broadcast sources. The framework input class
`Tuner` and each channel row's broadcast `COLUMN_TYPE` are separate metadata.

The version `0.7.0` TCL export contained 1,123 rows: 1,114 were reported as
`TYPE_DVB_S`, eight as `TYPE_PREVIEW`, and one as `TYPE_OTHER`. All 1,114 rows
from the TCL tuner input were reported as `TYPE_DVB_S`; this is the provider's
DVB-S classification, not proof that every row is current or receivable.
Provider service types were `EN_TCL_DTV` (890), `EN_TCL_RADIO` (141),
`EN_TCL_DATA` (83), and `SERVICE_TYPE_AUDIO_VIDEO` (9); the `EN_TCL_*` values
are reported verbatim and are not interpreted as a complete channel-status
taxonomy. All tuner rows were marked browsable and unlocked. Four
non-browsable rows came from other/unknown inputs. The broadcast visibility
field was blank on all 1,123 rows, so it provides no further distinction.

The CSV anomaly check found 40 tuner names consisting only of `.`, 12 blank
tuner names, and three tuner labels matching `.1.`/`.2.`/`.3.`. All were
marked browsable; the dot-only/blank categories also spanned TCL service types
`EN_TCL_DTV`, `EN_TCL_RADIO`, and `EN_TCL_DATA`. There were eight rows without
a channel number, no duplicate nonblank channel numbers, and no identical
number/name/input tuples. These metadata do not establish whether unusual
labels are stale, hidden from the TV UI, or otherwise invalid.

On API 30 and newer, the app offers an explicit runtime request for
`android.permission.READ_TV_LISTINGS`. AOSP Android 11 defines this as a
dangerous, read-only TV-listings permission; OEM implementations may differ.
The app does not request it until the user chooses the channel-access button,
and does not request any write permission. The physical TCL results above are
from versions `0.2.0` through `0.8.0`. Android 11 on the TCL recognized the
permission, and it remained granted across app stop, relaunch, and update. The
user can revoke it in Android's app permission settings; Android may also
auto-reset an unused permission. Only aggregate counts were read for the
`0.4.0`, `0.7.0`, and `0.8.0` validations; individual channel names are not
logged.
See the [AOSP Android 11 TV
Provider manifest](https://android.googlesource.com/platform/packages/providers/TvProvider/+/refs/heads/android11-release/AndroidManifest.xml)
for the permission declaration.

## PC export and TV import

The tested TCL does not provide an Android document picker. The explicit CSV
export therefore saves to `Downloads/tv-channel-list-<timestamp>.csv` on
Android 10+ (the TV runs Android 11). The CSV includes number, name, input ID,
broadcast type, service type, broadcast visibility, browsable, and locked
fields. The app adds a UTF-8 BOM, quotes CSV fields, and guards
spreadsheet-formula-like values. It requests no broad storage permission and
does not send the file over the network. After
connecting the authorized TV with ADB, copy the file to the PC with:

```powershell
adb -s <TV-IP>:5555 shell ls -t /sdcard/Download/tv-channel-list-*.csv
adb -s <TV-IP>:5555 pull /sdcard/Download/tv-channel-list-<timestamp>.csv .
```

Use the exact filename returned by the first command. Keep the ADB connection
on a trusted network and identify the TV explicitly; do not use an unspecified
default device.

The Windows [desktop manager](../../tools-py/README.md) can request this same
provider export through the `0.8.0` companion and pull it with the selected
ADB serial. Its one-shot receiver is protected by Android's
`android.permission.DUMP`; it does not launch the TV activity or bypass the
user's `READ_TV_LISTINGS` grant. This receiver is not a continuously running
service and the app still has no overlay permission.

The CSV is a convenient view/edit copy only. It is not the native TCL
channel-backup format and cannot be uploaded to the tuner by this app. The
standard `READ_TV_LISTINGS` access is read-only; there is no provider write,
tune, or import path in the companion.

ChanSort's current format list says that *some* TCL Android/Google TVs export
a `.tar` archive containing `database/cloneCRC.bin` and
`database/usedata/DtvData.db`. The exact format varies by model and firmware.
ChanSort's documentation also warns that its reverse-engineered formats are
not manufacturer-supported and use is at the user's risk. Do not feed this
app's CSV into ChanSort or import an edited file without first obtaining the
TV's own native USB backup and confirming that its exact format is supported.
See [ChanSort's TCL format notes](https://github.com/PredatH0r/ChanSort/blob/master/source/fileformats.md#tcl)
and [ChanSort's usage overview](https://github.com/PredatH0r/ChanSort/wiki/Home).

## Build

Open this directory in Android Studio or build it from PowerShell:

```powershell
.\gradlew.bat :app:assembleDebug :app:lintDebug
```

The debug APK is written to:

```text
app\build\outputs\apk\debug\app-debug.apk
```

The project uses Android Gradle Plugin 9.1.1 and the AGP built-in Kotlin
support. It has no third-party runtime libraries. `minSdk` is 21; it is
compiled and targeted against Android 16 (API 36). This stable toolchain is
compatible with the installed Android Studio 2025.3.2; Android 17 / API 37 is
still a preview and would require a newer Studio/SDK. The app is intended to
run on the TCL's Android 11 (API 30) system as well. This prototype is version
`0.8.0`.

## Install only on the intended TV

Do not install against an unverified ADB device. First confirm the target's
manufacturer, model, and Android version; this workstation may also have
other Android devices attached.

After confirming the TV's ADB identity and authorizing the host key on the TV,
use its explicit ADB serial rather than a global install command:

```powershell
adb devices -l
adb -s <TV-IP>:5555 install -r app\build\outputs\apk\debug\app-debug.apk
adb -s <TV-IP>:5555 shell monkey -p de.esp32adb.companion 1
```

To remove only this prototype:

```powershell
adb -s <TV-IP>:5555 uninstall de.esp32adb.companion
```

Do not enable OEM unlocking or root the TV for this app. If the TV asks to
authorize a new ADB host, approve it only if that host is expected.

## Design boundary

The Android APIs are an optional capability layer, not a replacement for the
ESP32's ADB baseline:

1. Use public Android APIs when they expose a capability consistently.
2. Keep ADB shell as a separately identified fallback, with explicit
   read-only/write boundaries.
3. Add vendor-specific adapters only when a device-specific behavior is
   confirmed and clearly labeled.
4. Report `available`, `denied`, `unsupported`, or `unknown` rather than
   silently treating missing data as a feature or a failure.

Only after the read-only prototype is tested on the TCL should we decide
whether to add a narrow, authenticated transport between the app and ESP32.
No generic arbitrary-shell endpoint should be added to the app.
