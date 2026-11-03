#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
"""Ask a PuckOff board to reboot into its UF2 bootloader, without touching a pin.

The standalone wired Switch image has no serial console. It accepts an
eight-byte HID output report, and one exact value of that report (ASCII
"PuckOff" followed by 0x57) makes the board reboot into the Adafruit UF2
bootloader about 50 ms later. This script finds that board over USB HID and
sends the report, then waits for the UF2 drive to appear so the next image
can be copied onto it.

Images with a console (generic USB, BLE bridge) have the `bootloader` shell
command instead; pass --port to use it through the serial console.
"""

from __future__ import annotations

import argparse
import os
import sys
import time

SWITCH_VID, SWITCH_PID = 0x0F0D, 0x0092
BOOTLOADER_REPORT = bytes([ord(c) for c in "PuckOff"] + [0x57])
UF2_NAMES = ("NICENANO", "NICE_NANO", "PROMICRO", "UF2BOOT")


def find_uf2_drive() -> str | None:
    roots = ["/Volumes", "/media", "/run/media", "/mnt"]
    for root in roots:
        try:
            names = os.listdir(root)
        except OSError:
            continue
        for name in names:
            if name.upper().startswith(UF2_NAMES):
                path = os.path.join(root, name)
                if os.path.exists(os.path.join(path, "INFO_UF2.TXT")):
                    return path
    return None


def wait_for_drive(timeout_s: float) -> str | None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        path = find_uf2_drive()
        if path:
            return path
        time.sleep(0.5)
    return None


def via_hid(serial_number: str | None) -> int:
    try:
        import hid  # hidapi
    except ImportError:
        print("install hidapi first: python -m pip install -r tools/requirements.txt", file=sys.stderr)
        return 2
    matches = [d for d in hid.enumerate(SWITCH_VID, SWITCH_PID)
               if "PuckOff" in (d.get("product_string") or "")]
    if serial_number:
        matches = [d for d in matches if d.get("serial_number") == serial_number]
    if not matches:
        print("no PuckOff wired Switch board found over USB HID", file=sys.stderr)
        return 1
    if len(matches) > 1 and not serial_number:
        print("several boards found; pick one with --serial:", file=sys.stderr)
        for d in matches:
            print(f"  {d.get('serial_number')}", file=sys.stderr)
        return 1
    dev = hid.device()
    dev.open_path(matches[0]["path"])
    try:
        # Report ID 0 prefix: hidapi strips it for devices without report IDs.
        written = dev.write(b"\x00" + BOOTLOADER_REPORT)
    finally:
        dev.close()
    if written <= 0:
        print("the board did not accept the report", file=sys.stderr)
        return 1
    print("bootloader report sent")
    return 0


def via_serial(port: str) -> int:
    try:
        import serial
    except ImportError:
        print("install pyserial first: python -m pip install -r tools/requirements.txt", file=sys.stderr)
        return 2
    with serial.Serial(port, 115200, timeout=0.5) as s:
        s.dtr = True
        time.sleep(0.5)
        s.write(b"\x15bootloader\r")
        time.sleep(0.5)
    print("bootloader command sent")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", help="serial console of an image that has one (uses the bootloader command)")
    ap.add_argument("--serial", help="USB serial number when several Switch boards are attached")
    ap.add_argument("--wait", type=float, default=15.0, help="seconds to wait for the UF2 drive (0 = don't)")
    args = ap.parse_args()

    code = via_serial(args.port) if args.port else via_hid(args.serial)
    if code or args.wait <= 0:
        return code
    path = wait_for_drive(args.wait)
    if path:
        print(f"UF2 drive: {path}")
        return 0
    print("no UF2 drive appeared; the bootloader may have enumerated without mounting a disk,"
          " try unplugging and replugging the board or a physical double reset", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
