#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate the default nice!nano-compatible board's application UF2."""

import argparse
from pathlib import Path
import struct

FLASH_START = 0x26000
FLASH_END = 0xEC000
FAMILY = 0xADA52840


def validate(data: bytes) -> int:
    if not data or len(data) % 512:
        raise ValueError("UF2 must contain complete 512-byte blocks")
    count = len(data) // 512
    address = FLASH_START
    for index in range(count):
        block = data[index * 512 : (index + 1) * 512]
        magic0, magic1, flags, target, size, number, total, family = struct.unpack_from(
            "<8I", block
        )
        if (magic0, magic1, struct.unpack_from("<I", block, 508)[0]) != (
            0x0A324655, 0x9E5D5157, 0x0AB16F30
        ):
            raise ValueError(f"block {index}: invalid UF2 magic")
        if flags != 0x2000 or family != FAMILY:
            raise ValueError(f"block {index}: not an nRF52840 flash image")
        if number != index or total != count:
            raise ValueError(f"block {index}: inconsistent block numbering")
        if not 0 < size <= 476 or size % 4:
            raise ValueError(f"block {index}: invalid payload size")
        if target != address or target + size > FLASH_END:
            raise ValueError(f"block {index}: outside contiguous application region")
        address += size
    return address - FLASH_START


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("uf2", type=Path)
    args = parser.parse_args()
    try:
        size = validate(args.uf2.read_bytes())
    except (OSError, ValueError) as error:
        parser.exit(1, f"{args.uf2}: {error}\n")
    print(f"{args.uf2}: valid nRF52840 UF2, {size} payload bytes at 0x{FLASH_START:x}")


if __name__ == "__main__":
    main()
