# Bluetooth transport groundwork

## BLE bridge (experimental, needs hardware testing)

`ble-bridge.conf` builds a receiver that sends Steam Controller input to a
Bluetooth LE host. The controller link and Bluetooth share the radio: each
controller exchange runs in its own MPSL timeslot (`src/radio_timeslot.c`),
and the SoftDevice Controller keeps priority for its connection events. Slots
Bluetooth takes are counted as deferred polls, exactly like the default image.

- While a Bluetooth host is connected, input goes to it. Otherwise it goes to
  the USB gamepad, so the same board still works plugged into a computer.
- The board advertises as `PuckOff` and remembers up to four hosts. Use
  `ble status` and `ble forget_hosts` in the serial console.
- The USB serial console and the setup tool work as in the default image.
- Pairing, security and the report format match the diagnostic below.
- Expect added latency in Bluetooth mode. Neither latency nor reliability
  next to an active Bluetooth connection has been measured on hardware yet.

```sh
west build --sysbuild -b promicro_nrf52840/nrf52840/uf2 \
  -d build/ble-bridge firmware -- -DEXTRA_CONF_FILE=ble-bridge.conf \
  -DEXTRA_DTC_OVERLAY_FILE=ble-bridge.overlay
```

### 32 kHz clock

The bridge selects the calibrated RC oscillator (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC`)
instead of the board default, an external 32.768 kHz crystal. Many Pro Micro
nRF52840 clones have no such crystal. The default USB image still runs on them
only because Zephyr's two-stage LFXO start keeps the RC oscillator running
while it waits for a crystal that never arrives. The SoftDevice Controller
waits for that crystal as well, so the first bridge image enumerated on USB
and then stopped responding completely: no console, no advertising, no reset.
The RC oscillator costs a little sleep-clock accuracy, which a USB-powered
receiver can afford, and works on boards with or without the crystal.

### Boot guard

The bridge also enables `CONFIG_PUCKOFF_BOOT_GUARD` (`firmware/Kconfig.guard`,
`src/boot_guard.c`). Before the kernel starts it arms the hardware watchdog
(10 s by default) and records how far each boot gets in retained RAM. The
system work queue feeds the watchdog, so a halted fatal error, a spin with
interrupts locked or a wedged work queue all end in a watchdog reset instead
of a dead board that still shows up on USB. What happens next depends on how
far the previous boot got:

| Previous boot reached | After a watchdog reset |
|-----------------------|------------------------|
| `ready` | Normal boot; safe mode after three in a row |
| `main`, `usb`, `bluetooth`, `radio` | **Safe mode**: USB gamepad and console only, Bluetooth and the controller radio off |
| `kernel`, `bootloader`, or no record | Straight to the UF2 drive, with no watchdog |

The record lives in 128 bytes of RAM reserved by `ble-bridge.overlay`
(`retained0`, 0x20000080) because the Adafruit bootloader runs on every reset
and zeroes its own RAM from 0x20008000 up; a record inside the kernel's normal
RAM did not survive the trip. Kernel fatal errors and MPSL / SoftDevice
Controller assertions are written into the same record before the watchdog
resets the board, so a crash is reported, not just a hang.

The `bootguard` console command prints the reset cause, the stage and detail
the previous boot reached, any recorded fault, the streak of watchdog resets,
and whether this boot is in safe mode. Safe mode ends with any non-watchdog reset, such as unplugging the board.
The nRF52840 watchdog cannot be stopped and keeps counting through a soft
reset, so with the guard enabled the `bootloader` command stops feeding it and
lets the watchdog perform the reset; the drive appears within the timeout, and
the bootloader never starts with a watchdog still running. Nothing here
modifies the bootloader: the guard uses the same retained GPREGRET value the
`bootloader` command always used.

The guard is off in the default USB image, whose UF2 is unchanged. Host tests
in `tests/receiver/test_boot_guard.c` cover the policy, watchdog register
setup, the feed, and both ways to reach the drive.

### TV remote mode

The same board can act as a TV remote instead of a gamepad. The BLE HID
report map (`src/ble/hid_map.c`) is a composite of four input reports: the USB
gamepad report under Report ID 1, a keyboard (ID 2), a relative mouse (ID 3)
and a consumer control with two usages (ID 4). All four are always present, so
a host never has to re-pair when the mode changes; the mode decides which
reports carry input and the others stay released.

| Controller | Remote mode sends |
|------------|-------------------|
| D-pad, left stick | Arrow keys |
| A | Enter |
| B | AC Back (Android/Fire TV profile) or Escape (Apple profile) |
| X | Play/Pause |
| Y | Mute |
| Steam | AC Home |
| Menu | Menu |
| View | AC Search |
| LB / RB | Previous / next track |
| Left / right trigger (half travel or click) | Volume down / up |
| Right trackpad | Pointer (about 1000 counts per swipe); click = left button |
| Right stick | Pointer, speed by deflection |
| Left trackpad, vertical | Scroll wheel; click = right button |

The consumer report carries two usages at a time, volume first. The mapping
lives in `src/ble/remote.c` as pure functions with host tests
(`firmware/tests/ble/remote_test.c`); pointer motion accumulates between
Bluetooth reports instead of being coalesced, so a steady swipe is not lost.

The mode is remembered **per host** (`src/ble/modes.c`, four hosts, stored
under the `puckoff/ble_modes` setting): a TV can stay a remote while a phone
stays a gamepad. Unknown hosts get the gamepad. To switch, either hold
**Steam + Menu for two seconds** on the controller (the chord itself is not
forwarded) or type `ble mode remote` / `ble mode gamepad` on the console while
the host is connected. `ble remote apple` makes B send Escape for Apple TV
style hosts. `ble status` shows the active mode, and `ble forget_hosts` clears
the remembered modes with the host bonds.

Whether a given TV accepts a composite HID device, maps AC Home/AC Back the
way its own remote does, or shows a pointer for the mouse report has to be
observed on that TV; none of it is implied by the descriptor.

Hardware checklist for a first test: copy a pairing onto the board, confirm
the controller still works over USB, then pair `PuckOff` from a TV or phone and
check that buttons, sticks and triggers respond, the link survives 10 minutes
of play, and input returns to USB after the Bluetooth host disconnects.


The default image remains the USB gamepad with exclusive proprietary RF. The
opt-in **BLE-only diagnostic** advertises `PuckOff BLE diagnostic` and implements
an encrypted HID input service. It does **not** receive Steam Controller input,
run the raw radio driver, or generate simulated controller motion.

## What the diagnostic provides

- One BLE peripheral connection, one persistent host bond, HID service UUID
  `0x1812`, gamepad appearance `0x03c4`, and the generic gamepad's report map.
- Input report reference ID **0**, type **1**. Notifications contain exactly
  **13 bytes**, without a report-ID prefix: little-endian buttons, hat, four
  little-endian signed axes, and two trigger bytes.
- An initially neutral report (hat **8**, not up), readable reports, encrypted
  notifications/CCCD writes, suspend/resume, and advertising after disconnect.
- A single latest-state mailbox, with an 8 ms send opportunity and two configured
  ATT TX buffers. Backpressure retains the latest state, not an unbounded FIFO.
  Notification submission is not proof that a host consumed input.
- USB CDC diagnostic commands. USB enumeration, DTR, a computer, and router
  networking are not prerequisites for BLE advertising.

Pairing uses encrypted **Just Works** (security level 2), not authenticated MITM
protection. Any nearby host may attempt to connect. There is no allowlist,
physical pairing button policy, management GATT protocol, Web Bluetooth service,
or network connection. The nRF52840 board has no Wi-Fi.

This is not Bluetooth qualification or a compatibility certification. Implementing
the HID service/HOGP wire model does not establish that iOS, Safari, tvOS, or
Apple's Game Controller framework accepts this as a supported game controller.
No such support is claimed. Device Information does not publish an invented
vendor-owned PnP identity. The diagnostic USB serial identity remains the
repository's temporary pid.codes test VID/PID; obtain allocated IDs before release.

## Build

Checked against nRF Connect SDK **3.4.1** and its Zephyr **4.4.2** checkout. Activate
the SDK environment and use a separate build directory for each configuration:

```sh
west build -b promicro_nrf52840/nrf52840/uf2 \
  -d build/ble-diagnostic firmware -- -DCONF_FILE=ble-diagnostic.conf
west build -b promicro_nrf52840/nrf52840/uf2 \
  -d build/usb-default firmware
```

The SDK's sysbuild output is
`build/ble-diagnostic/firmware/zephyr/zephyr.uf2`. Never distribute this as the
default receiver image. Keep the diagnostic label when copying it.

The board target preserves application start **0x26000**, application end
**0xec000**, settings at **0xec000..0xf4000**, and the UF2 bootloader above
**0xf4000**. The S140 6.x-compatible bootloader layout does not mean S140 is
running. This build links the current SoftDevice Controller and MPSL inside
the application; it does not invoke an old SoftDevice or overwrite its reserved
flash region.

## Diagnostic use

On a BLE host, scan for the diagnostic name, connect/pair, discover the HID
service, and subscribe to its Input Report. Discovery alone is not proof of
gamepad support. On a USB-connected diagnostic serial shell:

```text
ble status
ble buttons 0001
ble neutral
ble forget_host
bootloader
```

`buttons` is explicitly **manual test input**, not a controller decoder or a
background generator. It sets a hexadecimal 16-bit mask with neutral axes; use
`neutral` to release it. `forget_host` requires the host to be disconnected,
removes only the `bt` host bond, and does not erase proprietary controller
pairing records. The host may also need to forget its cached bond.

`bootloader` preserves the default image's software UF2 reentry: write `0x57`
to `NRF_POWER->GPREGRET`, then cold-reboot. Keep a data-capable USB connection
available when using this command for unattended recovery. It is not a BLE
remote-update service.

Test preparation requires an nRF52840 board with the named UF2 layout, a stable
USB power source, and a BLE central able to inspect encrypted GATT/HID traffic.
Host HID recognition and actual RF timing require physical testing. Building
does not prove either. Only the coordinating session performs device tests.

## Radio ownership and the coexistence boundary

`CONFIG_PUCKOFF_TRANSPORT_USB_RAW_RF` is the default. CMake rejects it if either
Bluetooth or MPSL is enabled. The diagnostic selects a different entry point
and does not compile `radio.c`, `link.c`, `bond.c`, `main.c`, `usb.c`, or the
receiver console. Bluetooth cannot quietly be added alongside the raw driver.

The installed SDK's MPSL timeslot API is the prospective shared-radio mechanism:

- `mpsl_timeslot_session_open()` / `mpsl_timeslot_request()` manage requests.
  The first request is `MPSL_TIMESLOT_REQ_TYPE_EARLIEST`; periodic requests may
  subsequently use `MPSL_TIMESLOT_REQ_TYPE_NORMAL`.
- Only granted windows permit application access to RADIO and TIMER0. On nRF52,
  PPI **19** is made available within that window, not outside it. The other
  reserved PPI channels **30/31** remain stack-owned.
- START, RADIO, and TIMER0 signals run at priority 0. No Zephyr scheduling,
  logging, Bluetooth, decoding, or blocking calls belong there.
- A timer/hardware cutoff and cleanup before the grant ends are mandatory.
  Overstaying has undefined behavior and can fault MPSL.
- Blocked/cancelled grants are normal outcomes; neither a requested 4 ms cadence
  nor nominal BLE connection parameters guarantee a 250 Hz controller rate.

The existing direct RADIO polling driver is not a timeslot backend. A real
bridge requires a single copied TX-plus-optional-RX transaction, bounded admission,
hardware deadline enforcement, an IRQ state machine, and completion transfer to
thread context. Merely placing the existing send/listen calls inside an MPSL
callback is unsafe.

SDK references (API facts only; implementation here is independently written):

- [MPSL timeslot API and constraints](https://github.com/nrfconnect/sdk-nrfxlib/blob/bb715183a57ac501907b63df2079c3fbcd067641/mpsl/include/mpsl_timeslot.h)
- [MPSL timeslot documentation](https://github.com/nrfconnect/sdk-nrfxlib/blob/bb715183a57ac501907b63df2079c3fbcd067641/mpsl/doc/timeslot.rst)
- [MPSL reserved hardware](https://github.com/nrfconnect/sdk-nrfxlib/blob/bb715183a57ac501907b63df2079c3fbcd067641/mpsl/include/mpsl_hwres.h)
- [NCS MPSL initialization](https://github.com/nrfconnect/sdk-nrf/blob/b20f8619ba9a5530f8c34b0a130d829947cfe55d/subsys/mpsl/init/mpsl_init.c)
- [Zephyr GATT API](https://docs.zephyrproject.org/latest/connectivity/bluetooth/api/gatt.html)

## Host-side checks

```sh
cmake -S firmware/tests/ble -B build/ble-tests
cmake --build build/ble-tests
ctest --test-dir build/ble-tests --output-on-failure
```

These exercise the actual latest-report state implementation: neutral wire
shape, encryption/subscription gates, 10,000 coalesced updates, suspend/resume,
reconnection, and stale-state release. They do not emulate the Bluetooth
controller, radio timing, host HID acceptance, or a physical Steam Controller.

These two deliberately unsafe builds must fail with the raw-radio ownership
error, not produce an image:

```sh
west build -b promicro_nrf52840/nrf52840/uf2 -d build/reject-raw-bt firmware -- \
  -DEXTRA_CONF_FILE=tests/ble/unsafe-bluetooth.conf
west build -b promicro_nrf52840/nrf52840/uf2 -d build/reject-raw-mpsl firmware -- \
  -DEXTRA_CONF_FILE=tests/ble/unsafe-mpsl.conf
```
