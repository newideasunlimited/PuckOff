# PuckOff

Open firmware that turns a $5 nRF52840 board into a wireless receiver for the
Steam Controller (2026), so it works on anything with a USB port.

Plug the board in, pair your controller, play.

## Status

| Feature                                   | State        |
|-------------------------------------------|--------------|
| Controller link (2.4 GHz, 250 Hz)         | Working      |
| Standard USB gamepad (PC, Mac, Linux, Android, browsers) | Working |
| Up to 4 controllers per board (radio)     | In progress  |
| Xbox / PlayStation / Switch style modes    | Planned      |
| Steam receiver mode (pairing from Steam)  | Planned      |
| Bluetooth to TVs, phones, Apple TV        | Planned      |
| Copy a pairing to any number of boards    | Host-assisted setup |

## Hardware

Any nRF52840 "Pro Micro" / nice!nano-compatible board with the Adafruit UF2
bootloader (SoftDevice S140 6.x layout).

## Install

1. Double-tap the board's **RST** pin to **GND** with a paperclip. A drive called
   `NICENANO` appears.
2. Copy `puckoff.uf2` onto it. The board reboots as **PuckOff Receiver**.

Later updates: type `bootloader` in the serial console instead of using a paperclip.

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

Requires the nRF Connect SDK v3.4 (Zephyr 4.4).

    west build -b promicro_nrf52840/nrf52840/uf2 firmware

The output is `build/firmware/zephyr/zephyr.uf2`.

## License

Apache-2.0. Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
See [NOTICE](NOTICE) for trademark information.
