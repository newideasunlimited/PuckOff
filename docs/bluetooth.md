# Bluetooth transport groundwork

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
```

`buttons` is explicitly **manual test input**, not a controller decoder or a
background generator. It sets a hexadecimal 16-bit mask with neutral axes; use
`neutral` to release it. `forget_host` requires the host to be disconnected,
removes only the `bt` host bond, and does not erase proprietary controller
pairing records. The host may also need to forget its cached bond.

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
