# USB modes and compatibility

PuckOff implements its own HID reports on Zephyr's **new USB device stack**.
Generic mode is the default. The second mode targets the publicly documented
wired Pokken-style protocol accepted by the original Nintendo Switch; it is
**opt-in and not yet hardware-verified in PuckOff**. There is no host
autodetection, Nintendo Pro Controller protocol, or Xbox/PlayStation
authentication implementation.

## What "works" means

These are separate milestones, not interchangeable claims:

| Milestone | Evidence needed |
|-----------|-----------------|
| USB enumeration | The host reads the device, configuration and HID report descriptors |
| Host input | The relevant API receives correct buttons, axes, releases and disconnects |
| Game compatibility | A named game, on a recorded OS/browser version, uses those controls correctly |

Baseline `76e830c` was tested on a nice!nano-compatible nRF52840: the radio
reported at 250 Hz, macOS HIDAPI read 13-byte generic reports, and Brave's
Gamepad API exposed input after focusing the page and pressing a button. Safari,
Apple Arcade, iPhone/iPad, and Switch were **not** verified by those observations.
The USB changes here have native software tests and board builds, not a new
physical-host result. Re-run the hardware procedure below before promoting a
mode to "verified."

Standards-based HID is a reasonable path for desktop applications that accept
generic gamepads, but it is not Windows XInput. Some games require a mapping or
another input API. Browser `mapping` may be empty rather than `"standard"`;
applications must not assume W3C standard button/axis indices.

## Profiles

| Property | Generic (default) | Wired Switch experiment |
|----------|-------------------|-------------------------|
| Selection | `CONFIG_PUCKOFF_USB_GENERIC` | `CONFIG_PUCKOFF_USB_SWITCH` |
| VID:PID | `1209:0001` (private-test-only) | `0f0d:0092` (third-party protocol identity) |
| Topology | HID plus CDC ACM serial console | One non-boot HID interface; no CDC |
| Interrupt IN | 13-byte reports, 1 ms polling endpoint | 8-byte reports, 64-byte endpoint, 5 ms polling |
| Interrupt OUT | None | 8-byte vendor reports, 64-byte endpoint, 5 ms polling |
| Report IDs | None | None |
| Buttons | 16 | 13 |
| Sticks | Four signed 16-bit axes | Four unsigned 8-bit axes |
| Triggers | Two analog 8-bit axes | Digital ZL/ZR at half travel or trigger click |
| D-pad | Hat 0..7 clockwise from up, 8 centered | Same |

**USB identity is not permission to ship a product.** The [pid.codes test-PID
policy][test-pid] says `1209:0001` is non-unique and reserved for private testing;
it must not be used on redistributed, sold, or manufactured devices. Source
references must carry that warning. Allocate a PuckOff product identity before
distributing hardware. `0f0d:0092` belongs to HORI, not PuckOff; it is used only
for this explicitly selected interoperability experiment. Neither identity is
an allocated commercial PuckOff PID. This work creates no firmware release.
USB strings identify PuckOff, not a genuine HORI product.

### Generic wire layout

| Byte offset | Encoding |
|-------------|----------|
| 0..1 | 16 little-endian button bits |
| 2 | Hat value in low nibble; high nibble zero |
| 3..10 | Little-endian `lx`, `ly`, `rx`, `ry`, each -32767..32767; positive Y down |
| 11..12 | Left/right triggers, 0..255 |

Button bits 0..15 are A, B, X, Y, LB, RB, View, Menu, L3, R3, Steam/Home,
QAM, L4, R4, L5, R5. `INT16_MIN` is saturated to the descriptor's -32767
minimum. Opposite D-pad directions cancel on each axis. Trigger clicks force
full travel. `gamepad_encode_generic()` serializes explicitly, preserving the
existing 13-byte/no-report-ID format without depending on CPU byte order.

The descriptor resets the hat's physical range before declaring the axes.
Otherwise HID global-item inheritance incorrectly leaves sticks and triggers
with the hat's 0..315-degree physical scale.

### Wired Switch wire layout

The format follows public descriptor observations and button assignments
documented by [Switch-Fightstick][wired-readme], not copied controller firmware.
The important changes are the complete report shape, mapping, single-interface
topology, endpoint sizes/poll intervals and output/control behavior, **not just
a VID/PID substitution**.

| Byte offset | Encoding |
|-------------|----------|
| 0..1 | Little-endian buttons: Y, B, A, X, L, R, ZL, ZR, Minus, Plus, L3, R3, Home; upper three bits zero |
| 2 | Hat 0..7, 8 centered; high nibble zero |
| 3..6 | Left X/Y, right X/Y, 0..255; center 128; positive Y down |
| 7 | Vendor byte, always zero |

Face buttons keep their **physical positions**: Steam A (south) becomes Switch
B; Steam B (east) becomes A; Steam X (west) becomes Y; Steam Y (north) becomes X.
LB/RB map to L/R, View/Menu to Minus/Plus, and Steam to Home. QAM, back buttons,
touchpads and touch sensors are not exposed. The conservative 13-button
descriptor does not declare Capture. No rumble, player LEDs, motion, NFC,
battery reporting, console wakeup, or Switch 2 compatibility is claimed.

Both control `SET_REPORT(Output, ID 0)` and interrupt OUT accept exactly eight
bytes. They retain an opaque vendor report for `GET_REPORT(Output, ID 0)` but
do not drive any actuator. Malformed control reports stall; malformed interrupt
OUT packets are logged and discarded. This is not Nintendo's `0x80` USB
handshake / `0x01` subcommand / `0x30` input Pro Controller protocol.

The [captured original descriptors][wired-descriptors] use one HID interface,
64-byte endpoints and 5 ms polling. PuckOff keeps its own strings and the
existing 200 mA configuration power declaration, not the captured 500 mA value.
Zephyr allocates endpoint numbers (normally IN `0x81`, OUT `0x01` for the
standalone profile), rather than copying the original OUT `0x02` address.
Host interoperability with these differences still needs the physical test;
software descriptor checks cannot establish it.

## Apple: what the public evidence actually says

[Apple's general controller guidance][apple-general] says that **some**
controllers can connect using a data-capable USB cable; it does not promise that
every HID gamepad, controller identity, or transport works on every Apple device.
[Apple's PlayStation guidance][apple-ps] specifically documents USB connections
for DualShock 4, DualSense, DualSense Edge and Access controllers. Its
[Xbox guidance][apple-xbox] lists supported Bluetooth models and describes
Bluetooth pairing. Neither document is a specification for implementing a
compatible replacement by changing identifiers.

There are also different software input paths:

- macOS HIDAPI/IOHID can read a generic HID device without that device necessarily
  becoming an Apple `GCController`.
- Current public [WebKit macOS HID-provider source][webkit-hid] matches generic
  joystick/gamepad HID usages and coexists with a Game Controller-framework
  provider. Thus "Safari requires Xbox/PS/Switch impersonation" is too broad.
  This source is architectural evidence, not a PuckOff Safari test result or a
  guarantee about a released Safari version.
- Native games, including Apple Arcade titles, may use Apple's Game Controller
  framework. [Apple describes][apple-framework] `GCController` connection and
  input events separately from device enumeration. The specific game must
  support and correctly map the controller.
- A macOS HID or browser result does not establish iPhone/iPad USB support.
  The adapter, host mode/power, OS version, transport and app all matter.

Accordingly, neither PuckOff mode is advertised as Apple Arcade/iPhone-ready.
The wired Switch profile is **not an Apple compatibility profile**. There is
no claim of PS5 or Xbox console support without their authentication; no
proprietary firmware, signing keys, or authentication material is included.

## Report and control-transfer behavior

`usb_start()` and `usb_send_gamepad()` remain the application API. Producers
queue a latest-state snapshot without blocking. One work item submits a
separate, aligned buffer; it cannot be reused until Zephyr's input-completion
callback, including cancellation during disconnect/reconfiguration. Callbacks
do not submit recursively. A failed submit keeps the latest state, retries
after at least 4 ms while configured, and logs the failure/recovery. This also
covers a final neutral report when no new radio packets arrive.

Intermediate snapshots can coalesce; this is not a lossless button-event FIFO.
`GET_REPORT` reads the latest complete snapshot, not a partially updated
in-flight buffer. Only declared type/ID combinations are accepted, short reads
are bounded, unsupported requests stall, and zero-length reads are rejected.
Both profiles support HID idle timing in 4 ms units, with report ID 0 only.
Boot protocol is not advertised. Suspend pauses submissions; resume and
reconfiguration force the latest state to be sent again. There is no remote
wakeup claim.

Repeated identical encoded input is now suppressed at idle duration zero; a
nonzero `SET_IDLE` requests periodic duplicates even without radio input.
Therefore a stationary controller's USB report count is **not** a 250 Hz radio
throughput measurement. A completed/cancelled transfer callback in this Zephyr
API does not include a delivery status; physical host receipt still needs
host-side observation.

## Build, test and recover

Use the project's nRF Connect SDK environment (NCS 3.4 / Zephyr 4.4). From the
repository root, keep output directories separate:

```sh
west build -b promicro_nrf52840/nrf52840/uf2 -d build/usb-generic firmware
west build -b promicro_nrf52840/nrf52840/uf2 -d build/usb-switch firmware -- \
  -DEXTRA_CONF_FILE=modes/switch.conf \
  -DEXTRA_DTC_OVERLAY_FILE=modes/switch.overlay
```

The images are respectively
`build/usb-generic/firmware/zephyr/zephyr.uf2` and
`build/usb-switch/firmware/zephyr/zephyr.uf2`. Both use the same flash/partition
layout. Do not flash an erase-all image when retaining existing pairing data.
Compile-time guards reject a mismatched profile/overlay and the board's
automatic CDC initializer. The Switch fragment disables that board default
explicitly: disabling the CDC class alone is insufficient on this board.

For an **unattended development experiment only**, a third image automatically
returns to the Adafruit UF2 bootloader after 120 seconds:

```sh
west build -b promicro_nrf52840/nrf52840/uf2 -d build/usb-switch-test firmware -- \
  -DEXTRA_CONF_FILE="modes/switch.conf;modes/switch-test.conf" \
  -DEXTRA_DTC_OVERLAY_FILE=modes/switch.overlay
```

Its artifact is `build/usb-switch-test/firmware/zephyr/zephyr.uf2`. The
`CONFIG_PUCKOFF_USB_TEST_RECOVERY` option is off in both normal profiles and
limited to this board's UF2 target. It uses the same `GPREGRET = 0x57` and cold
reboot mechanism as the existing console `bootloader` command, without adding
CDC to the Switch descriptors. The timer starts at `usb_start()`, not on host
connection, and needs no host request. On a computer, wait for `NICENANO` to
reappear before restoring the known-good generic image. Bootloader enumeration
does **not** guarantee that macOS mounts a writable UF2 volume: the coordinator
has observed a baseline software reboot enumerate the bootloader without a
mounted disk. Host re-enumeration troubleshooting or physical reset may still
be required. This is **not a playing
image**: it always reboots after two minutes. A power cycle restarts the timer.
An application timer cannot recover a board that never boots or whose
kernel/workqueue faults; preserve physical reset access and the original board.

The host-only tests compile the real report encoders and `usb.c`, using the
SDK's actual HID-item definitions plus a fake USB/workqueue boundary. They do
not enumerate, open, or flash a physical board:

```sh
cmake -S tests/usb -B build/usb-tests -DZEPHYR_BASE="$ZEPHYR_BASE" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build/usb-tests
ctest --test-dir build/usb-tests --output-on-failure
```

Tests cover descriptor bit lengths/usages/ranges, golden report bytes, each
button, all 16 D-pad combinations, all 65,536 stick values, trigger thresholds,
short/invalid control transfers, output lengths, idle timing, suspend, failed-submit
coalescing, final-neutral retries, reconnect while a buffer is owned, early
completion and a completion racing the stack's buffer-pool release. These are
software checks, not USB electrical or console acceptance tests. The separate
recovery-image test verifies no reboot before 120 seconds and the bootloader
magic/cold-reboot call at the deadline; it does not claim a physical remount.

### Physical procedure (coordinator/operator only)

1. Start with an already paired board on the generic image. Keep a known-good
   generic UF2 available for recovery. Do not put pairing records in logs, PRs,
   screenshots, or bug reports.
2. Enter the UF2 bootloader by double-tapping RST to GND, and copy the chosen
   image to the `NICENANO` drive. A generic image also permits the serial
   `bootloader` command; the Switch image does not. No board access was
   performed by the software-test worker. Use the explicitly named
   `usb-switch-test` image for a two-minute, automatic-return experiment if
   unattended software recovery is needed; do not risk the only known-good
   board on a CDC-less image without a recovery route.
3. On a desktop, first record the VID:PID, interface list and HID descriptor.
   Generic mode should retain CDC plus HID; Switch mode should expose only
   one HID interface with IN/OUT endpoints. Record OS version, adapter/hub,
   board and exact image revision. Enumeration alone is only milestone one.
4. Read HID input while exercising every button, D-pad direction, stick and
   trigger. Expect 13-byte generic or 8-byte Switch **wire** reports, without a
   report-ID byte. HIDAPI request buffers can reserve a leading zero for
   report ID 0; do not confuse that API convention with the wire layout.
   Confirm hat 8 at rest, sticks 0 (generic) or 128 (Switch), and zero buttons
   and triggers after release. Stop moving the controller and distinguish
   change-only reporting from loss of the radio link.
5. Hold a button and power off/disconnect the controller; confirm neutral input
   arrives. Repeat USB unplug/replug and host sleep/wake with a button held,
   then released, looking for stuck controls, stale reports or corrupted axes.
   If a raw-control test tool is available, test input ID 0 with lengths 1,
   the full report size and 64; unsupported feature/nonzero-ID requests must
   stall. Exercise idle duration 0 and 8 ms.
6. Test Brave/Chromium and Safari separately: focus a secure-context or localhost
   gamepad test page, click it, then press a controller button. Inspect
   `navigator.getGamepads()` for `id`, `mapping`, `buttons`, and `axes`, and
   confirm changing values and releases, not just a `gamepadconnected` event.
   Check macOS Game Controllers settings/a `GCController` diagnostic separately,
   then test a named native game. Repeat independently on iPhone/iPad if those
   platforms are a target; a Mac result is not a substitute.
7. For the opt-in image, connect to an original Switch/Switch OLED dock's USB
   port. Use **Controllers > Change Grip/Order**, press L+R (Steam LB+RB),
   and test controller buttons/sticks and a named game. Remember that Switch
   confirm A is the Steam controller's physical B button in this positional
   mapping. Record the console firmware, dock/adapter and game. Leave
   untested features and Switch 2 explicitly unknown.
8. To change pairing or recover, double-tap RST to GND and restore the
   known-good generic UF2. The standalone Switch image intentionally has no
   serial setup console.

## Provenance

Report encoders, state handling and tests are original Apache-2.0 PuckOff code,
Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors. Protocol
field values and host behavior were researched from public documentation;
no other controller project's implementation or binary was copied or ported.

- [USB HID 1.11 specification][hid-spec]: report items, control requests, idle
  behavior, report IDs and field encoding.
- [Captured HORI descriptors][wired-descriptors] and [protocol discussion][wired-readme],
  pinned at `af49d54640834c40ad9cc73d8281e8d869abc4b3`. The public
  [button assignment definitions][wired-buttons] provide the wire-bit facts;
  PuckOff uses its own mapping from the existing generic report.
- Apple Support and WebKit sources below are evidence about host paths, not
  vendor certification or endorsement of PuckOff.

[test-pid]: https://pid.codes/1209/0001/
[hid-spec]: https://www.usb.org/sites/default/files/hid1_11.pdf
[wired-readme]: https://github.com/progmem/Switch-Fightstick/blob/af49d54640834c40ad9cc73d8281e8d869abc4b3/README.md
[wired-descriptors]: https://github.com/progmem/Switch-Fightstick/blob/af49d54640834c40ad9cc73d8281e8d869abc4b3/HORI_Descriptors
[wired-buttons]: https://github.com/progmem/Switch-Fightstick/blob/af49d54640834c40ad9cc73d8281e8d869abc4b3/Joystick.h
[apple-general]: https://support.apple.com/en-us/111099
[apple-ps]: https://support.apple.com/en-us/111100
[apple-xbox]: https://support.apple.com/en-us/111101
[apple-framework]: https://developer.apple.com/videos/play/wwdc2021/10081/
[webkit-hid]: https://github.com/WebKit/WebKit/blob/3f86837a09d0482c52d03bf2c8b53997288af8fc/Source/WebCore/platform/gamepad/mac/HIDGamepadProvider.mm
