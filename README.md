# PuckOff

Open prototype firmware that turns a nice!nano-compatible nRF52840 board into
a wireless receiver for the Steam Controller (2026), exposing one USB HID
gamepad.

Baseline hardware checks cover one controller on macOS/Brave. Other hosts,
boards, consoles and the current reliability changes need hardware validation;
a USB port alone does not imply compatibility.

The current USB identities are for development, not shipping products. Generic
mode uses the **private-test-only, non-unique `1209:0001` PID**, which must not be
used on redistributed, sold, or manufactured devices. See
[USB modes and compatibility](docs/usb-modes.md) before choosing an image.

## Status

| Feature                                   | State        |
|-------------------------------------------|--------------|
| Controller link (2.4 GHz, 250 Hz target)  | Baseline demonstrated on one controller |
| Generic USB HID gamepad                  | macOS HIDAPI and Brave input confirmed on the baseline; other hosts/games need testing |
| Four stored bonds, one active controller | Implemented; not four simultaneous gamepads |
| Wired Switch / Pokken-style USB HID      | Implemented, opt-in; hardware validation pending |
| Xbox / PlayStation console support       | Not implemented; no authentication support |
| Steam receiver mode (pairing from Steam)  | Planned      |
| Bluetooth to TVs, phones, Apple TV        | Planned      |
| Move a controller between many boards     | Planned      |

## Hardware

Build target: `promicro_nrf52840/nrf52840/uf2`, using an Adafruit UF2
bootloader with the SoftDevice S140 6.x memory layout. Check your particular
board and bootloader before flashing; not every nRF52840 board is interchangeable.

## Install

1. Double-tap the board's **RST** pin to **GND** with a paperclip. On a matching
   bootloader, wait for the `NICENANO` drive to mount; USB re-enumeration alone
   does not guarantee a mounted drive.
2. Copy a matching `puckoff.uf2` onto it. The board reboots as **PuckOff Receiver**.

The **Receiver** GitHub Actions workflow produces a commit-specific UF2
artifact, not a release. Unzip it before copying the firmware, and confirm the
board/bootloader layout above.

Later updates: type `bootloader` in the serial console instead of using a paperclip.
If the drive does not mount, see the [recovery notes](docs/build.md#recovery).

## Pairing (for now)

Open the board's serial port (115200 baud, any terminal) and enter your
controller's pairing record:

    pair set 0 <receiver-id> <controller-id> <serial>
    status

Pairing from Steam is planned and will replace this step.

## Build from source

The default build locks nRF Connect SDK **v3.4.1** sources and Zephyr SDK
**1.0.1**. See [build and CI instructions](docs/build.md) for a fresh workspace
and [receiver reliability](docs/receiver.md) for host tests and hardware checks.

From an existing matching SDK workspace:

    west build --sysbuild -b promicro_nrf52840/nrf52840/uf2 \
      -d /path/to/PuckOff/build/receiver /path/to/PuckOff/firmware

The output is `build/receiver/firmware/zephyr/zephyr.uf2` in that checkout.
Generic HID plus the serial console remains the default. The experimental
wired Switch image has **no serial console**; build and recovery instructions
are in [USB modes](docs/usb-modes.md).

## License

Apache-2.0. Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
See [NOTICE](NOTICE) for trademark information.
