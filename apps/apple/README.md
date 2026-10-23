# Native Apple companion apps

An initial SwiftUI **macOS and iOS companion**, not a universal controller bridge.
The apps start unconnected, with no demo input or automatic device access.
This is independent Apache-2.0 code; there are no third-party package dependencies.

## Implemented capabilities

| Capability | macOS | iOS / iPadOS |
|---|---|---|
| OS-supported controller input inside this app | GameController, opt-in diagnostics | GameController, opt-in diagnostics |
| Receiver discovery | I/O Kit USB/CDC metadata, explicit scan and selection | Not implemented / no generic CDC transport |
| Receiver status | One-shot, read-only management-v1 serial session | Not available |
| Early receiver raw input | Non-exclusive I/O Kit HID, selected USB location, 13-byte report | Not available |
| Setup guidance and platform limitations | Native UI | Native UI |
| Bond import/export/clear, native pairing, flashing | Not implemented | Not implemented |
| System-wide virtual controller output | Not implemented | Not available to an ordinary app |

The macOS transports are real implementations, not mocks. Their hardware paths
have **not** been exercised by this change. Build success and fixture tests are
not evidence of end-to-end controller compatibility.

The USB filter is the current experimental **VID `1209`, PID `0001`**, not an
assigned production identity or proof of authenticity. Raw input additionally
matches the selected USB location and usage page `1`, usage `5`. It requires a
13-byte, report-ID-free input report matching `firmware/src/gamepad.h`.
GameController and raw HID are deliberately separate views: a generic USB HID
device need not be exposed by GameController.

## Build

Use an already installed **Xcode 26 or newer / Swift 6.2 or newer**, with the
macOS and iOS SDKs, and **XcodeGen 2.44 or newer**. Swift 6.2 is required for
main-actor-isolated HID teardown. Deployment targets are macOS 14 and iOS 17.
No setup command here downloads Xcode, installs a driver, or accesses an account.

The verified toolchain was Xcode 27.1, Swift 6.4, XcodeGen 2.46.0, macOS SDK 27.0,
and iOS SDK 27.1. Use the same XcodeGen version for identical project generation.
The generated project is ignored; `project.yml` is its source of truth.

From the repository root:

```sh
xcode-select -p
swift --version
xcodebuild -showsdks
xcodegen --version

cd apps/apple
swift test
xcodegen generate

xcodebuild -project PuckOff.xcodeproj -scheme PuckOffMac \
  -destination 'generic/platform=macOS' -derivedDataPath DerivedData/mac \
  CODE_SIGNING_ALLOWED=NO build

xcodebuild -project PuckOff.xcodeproj -scheme PuckOffiOS \
  -destination 'generic/platform=iOS Simulator' \
  -derivedDataPath DerivedData/ios-simulator CODE_SIGNING_ALLOWED=NO build

xcodebuild -project PuckOff.xcodeproj -scheme PuckOffiOS \
  -destination 'generic/platform=iOS' -derivedDataPath DerivedData/ios-device \
  CODE_SIGNING_ALLOWED=NO build
```

These are unsigned development builds, not installers or App Store submission
configurations. The macOS development target is intentionally not sandboxed to
allow public POSIX serial access. There are no signing teams, provisioning
settings, privileged helpers, virtual-HID entitlements, or DriverKit extensions.
The unsigned iOS device build compiles but is not provisioned to install on an
iPhone. Signing and distribution are outside this slice.

`PuckOffCore` contains Foundation-only report/protocol logic. `swift test` uses
fixtures and mocked transports; it does not enumerate controllers, scan USB,
open device files, or launch either app. If a required Apple SDK is missing,
do not report its UI as built. Standalone non-Apple Swift toolchains can work on
the core, but this change does not claim a Linux validation run.

## Management-v1 dependency

Firmware at the original `76e830c` revision has only the legacy interactive
commands; it **does not** provide the management-v1 endpoint. Reading status
requires the pairing/setup firmware work. The Apple app never falls back to
`pair show`, `status`, `watch`, `pair set`, `pair clear`, or `bootloader`.

Before **each** request, the app:

1. Drains pending input until a 50 ms quiet interval, with a bounded byte budget.
2. Sends **Ctrl-U alone** (`15` hex), without CR.
3. Requires Zephyr's clear-to-end-of-screen acknowledgment: `ESC[J` or `ESC[0J`.
4. Drains again before sending the request below.

If cancellation is not acknowledged, no command terminator is sent for that
request. A user assertion of a fresh restart is not used as proof. This avoids
accidentally completing a stale shell command when echo/metakeys are disabled.
The firmware must enable shell echo, metakeys, and VT100 commands; muted shells
are refused. Other serial tools should be closed, but that advice alone is not
a safety guarantee.

The coordinator observed `Ctrl-U -> ESC[J` on receiver v0.1.0.
`Tests/PuckOffCoreTests/Fixtures/shell-cancel.json` records only those non-sensitive
bytes and their provenance. The Apple-app worker did not access that hardware.
The JSON info/status fixtures are entirely synthetic.

Wire requests are ASCII, with a fresh 1-16 character `[A-Za-z0-9_-]` correlation
ID and a trailing CR:

```text
setup info <id>\r
setup status <different-id>\r
```

Replies are newline-terminated lines beginning exactly `PUCKOFF/1 ` followed by
JSON, at most **1024 bytes excluding CR/LF**. The client requires a matching
`id`, matching `op`, and boolean `ok`. Unrelated prompt, echo, log, and stale-ID
lines are ignored, not displayed. Each request, including its cancellation
handshake, has a shared **3-second deadline**. Each drain, acknowledgment, and
reply receive phase is limited to **16 KiB**; the serial read buffer is 256 bytes.
No phase extends the deadline. Failure, cancellation, or success closes the port
and attempts to restore its original settings; cleanup failures are surfaced.

The `info` response must report:

```json
{
  "id": "<id>", "op": "info", "ok": true,
  "protocol": 1, "product": "PuckOff Receiver", "firmware": "dev",
  "slots": 4, "record_bytes": 24,
  "capabilities": ["status", "export", "import", "clear"],
  "native_pairing": false
}
```

Only the `status` capability is required by this read-only client. Firmware
and other receiver-provided strings are not retained in the UI model.
`status` contains `storage_ready` and all four unique slots `0...3`, each with
`slot`, `used`, `connected`, `powered_off`, `rssi`, `polls`, `replies`, `reports`,
and `crc_errors`. Counters are unsigned 32-bit; RSSI is `-255...0` dBm, where `0`
means unavailable. `storage_ready: false` is explicitly shown as unready, even
when `ok` is true. `ok: false` is an error, not a successful empty snapshot.

The status view is a timestamped snapshot, **not** an open connection or a
claim that a controller is still connected. No bond records are requested.
The shared protocol has other capabilities, but they do not authorize the app
to import, overwrite, export, or clear anything.

## Public Apple API boundaries

These boundaries were checked against Apple's documentation on 2026-10-22:

| Public API | What it supports and what it does not establish |
|---|---|
| [GCController](https://developer.apple.com/documentation/gamecontroller/gccontroller) | Reads controllers the OS exposes to this app. Controller snapshots are local input objects, not system-wide device registration. A generic HID descriptor alone does not guarantee support. |
| [GCVirtualController](https://developer.apple.com/documentation/gamecontroller/gcvirtualcontroller) | Displays software controls over the creating app's game. It is not a way for an ordinary iOS companion to inject input into unrelated apps. |
| [I/O Kit HID Manager](https://developer.apple.com/library/archive/technotes/tn2187/_index.html) | Public macOS device matching, hot-plug callbacks, and input reports. This app uses non-exclusive access and does not create a device. |
| [macOS serial device access](https://developer.apple.com/library/archive/documentation/DeviceDrivers/Conceptual/WorkingWSerial/WWSerial_SerialDevs/SerialDevices.html) | I/O Kit locates the device file; POSIX `termios` handles serial I/O. Opening the port/asserting DTR can affect hardware control lines, so reading requires confirmation. |
| [External Accessory](https://developer.apple.com/documentation/externalaccessory) | Supports approved MFi accessory protocols; it is not unrestricted generic USB CDC access for this iPhone app. |
| [DriverKit](https://developer.apple.com/documentation/driverkit) and [virtual HID entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.hid.virtual.device) | Driver-based virtual HID is separate entitlement-dependent work. DriverKit's documented platforms include macOS and M-series iPads, not a general iPhone escape hatch. No driver is included or installed here. |
| [Core Bluetooth](https://developer.apple.com/documentation/corebluetooth) | Could support a future documented receiver service. A custom BLE input/management connection does not turn an app into a system-wide gamepad. This slice has no BLE transport or Bluetooth scanning. |

## Validation and remaining hardware work

Unit tests cover report size/ID, little-endian signed axes, all sixteen buttons,
hat directions, normalization, framing/chunk boundaries, response byte limits,
correlation, schema rejection, privacy-safe errors, storage-unready status,
probe-before-status ordering, and refusal to send CR without cancellation
acknowledgment. Mock channels cover missing echo, unrelated escapes, drain
failures, repeated handshakes, and shared request deadlines.

Native macOS, iOS Simulator, and iOS device-SDK builds have been checked without
launching the apps or connecting to hardware. No physical receiver or iPhone
integration is claimed. Hardware testing is reserved for the coordinator.

For a later authorized hardware pass, check discovery/selection, unplug during
status and HID monitoring, cancellation/permission failures, muted-shell refusal,
management-v1 status, raw inputs against firmware values, and OS-supported
GameController input on each target OS. The app must remain honest when a
controller is absent or unsupported. Diagnostics stop on loss of foreground;
no input, USB serial paths, receiver/controller identifiers, or raw serial
transcripts are persisted or uploaded.
