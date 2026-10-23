# PuckOff

Open firmware that turns a $5 nRF52840 board into a wireless receiver for the
Steam Controller (2026). USB host recognition and game support vary; having a
USB port is not enough to guarantee compatibility.

The current USB identities are for development, not shipping products. Generic
mode uses the **private-test-only, non-unique `1209:0001` PID**, which must not be
used on redistributed, sold, or manufactured devices. See
[USB modes and compatibility](docs/usb-modes.md) before choosing an image.

## Status

| Feature                                   | State        |
|-------------------------------------------|--------------|
| Controller link (2.4 GHz, 250 Hz)         | Working      |
| Generic USB HID gamepad                  | macOS HIDAPI and Brave input confirmed on the baseline; other hosts/games need testing |
| Up to 4 controllers per board (radio)     | In progress  |
| Wired Switch / Pokken-style USB HID      | Implemented, opt-in; hardware validation pending |
| Xbox / PlayStation console support       | Not implemented; no authentication support |
| Steam receiver mode (pairing from Steam)  | Planned      |
| Bluetooth to TVs, phones, Apple TV        | Planned      |
| Move a controller between many boards     | Planned      |

## Hardware

Any nRF52840 "Pro Micro" / nice!nano-compatible board with the Adafruit UF2
bootloader (SoftDevice S140 6.x layout).

## Install

1. Double-tap the board's **RST** pin to **GND** with a paperclip. A drive called
   `NICENANO` appears.
2. Copy `puckoff.uf2` onto it. The board reboots as **PuckOff Receiver**.

Later updates: type `bootloader` in the serial console instead of using a paperclip.

## Pairing (for now)

Open the board's serial port (115200 baud, any terminal) and enter your
controller's pairing record:

    pair set 0 <receiver-id> <controller-id> <serial>
    status

Pairing from Steam is planned and will replace this step.

## Build from source

Requires the nRF Connect SDK v3.4 (Zephyr 4.4).

    west build -b promicro_nrf52840/nrf52840/uf2 firmware

The output is `build/firmware/zephyr/zephyr.uf2`. Generic HID plus the serial
console remains the default. The experimental wired Switch image has **no
serial console**; build and recovery instructions are in
[USB modes](docs/usb-modes.md).

## License

Apache-2.0. Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
See [NOTICE](NOTICE) for trademark information.
