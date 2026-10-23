# PuckOff

Open prototype firmware that turns a nice!nano-compatible nRF52840 board into
a wireless receiver for the Steam Controller (2026), exposing one generic USB
HID gamepad.

Baseline hardware checks cover one controller on macOS/Brave. Other hosts,
boards, consoles and the current reliability changes need hardware validation;
a USB port alone does not imply compatibility.

## Status

| Feature                                   | State        |
|-------------------------------------------|--------------|
| Controller link (2.4 GHz, 250 Hz target)  | Baseline demonstrated on one controller |
| Generic USB gamepad                     | Baseline demonstrated on macOS/Brave |
| Four stored bonds, one active controller | Implemented; not four simultaneous gamepads |
| Xbox / PlayStation / Switch style modes    | Planned      |
| Steam receiver mode (pairing from Steam)  | Planned      |
| Bluetooth to TVs, phones, Apple TV        | Planned      |
| Copy a pairing to any number of boards    | Host-assisted setup |

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

## Pairing and setup

Use the local [setup tool](docs/pairing.md) to copy a pairing from an official
2026 Steam Controller Puck, another PuckOff, or a private backup. No controller
IDs to type, no browser WebSerial requirement, and no limit on the number of
DIY receivers you can set up.

    python3 -m venv .venv
    source .venv/bin/activate
    python -m pip install -r tools/requirements.txt
    python tools/puckoff_setup.py devices

Every connection requires an explicitly selected port. Imports default to a
dry run; `--apply` enables writing, and occupied slots need explicit replacement
permission plus a private backup. The official puck is read-only and is never
reset or erased. Flash firmware containing the setup commands before using the
tool with a PuckOff board.

See [pairing instructions](docs/pairing.md) for the complete workflow and
[management protocol v1](docs/management.md) for app integration. The original
serial commands remain available for recovery. Native over-air pairing and
pairing directly from Steam are not implemented yet.

## Build from source

The default build locks nRF Connect SDK **v3.4.1** sources and Zephyr SDK
**1.0.1**. See [build and CI instructions](docs/build.md) for a fresh workspace
and [receiver reliability](docs/receiver.md) for host tests and hardware checks.

From an existing matching SDK workspace:

    west build --sysbuild -b promicro_nrf52840/nrf52840/uf2 \
      -d /path/to/PuckOff/build/receiver /path/to/PuckOff/firmware

The output is `build/receiver/firmware/zephyr/zephyr.uf2` in that checkout.

## Native Apple companion apps

The initial [macOS and iOS apps](apps/apple/README.md) provide native setup
guidance and in-app input diagnostics. macOS also implements explicit USB
discovery, read-only management-v1 status, and raw receiver HID diagnostics.
Status requires the newer management-v1 firmware contract; the original
receiver firmware does not provide it. The iOS app does not inject a controller
into other apps or make an unsupported USB receiver system-compatible.

## License

Apache-2.0. Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
See [NOTICE](NOTICE) for trademark information.
