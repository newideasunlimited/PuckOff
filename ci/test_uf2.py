# SPDX-License-Identifier: Apache-2.0
import struct
import unittest

from check_uf2 import FAMILY, FLASH_END, FLASH_START, validate


def block(number=0, count=1, address=FLASH_START):
    data = bytearray(512)
    struct.pack_into(
        "<8I", data, 0, 0x0A324655, 0x9E5D5157, 0x2000,
        address, 256, number, count, FAMILY
    )
    struct.pack_into("<I", data, 508, 0x0AB16F30)
    return data


class UF2Tests(unittest.TestCase):
    def test_valid(self):
        self.assertEqual(validate(block()), 256)
        self.assertEqual(validate(block(0, 2) + block(1, 2, FLASH_START + 256)), 512)

    def test_truncated(self):
        for data in (b"", block()[:-1], block() + b"x"):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                validate(data)

    def test_wrong_fields(self):
        for offset, value in (
            (0, 0), (4, 0), (8, 0), (12, FLASH_START - 256),
            (12, FLASH_END), (16, 0), (16, 477), (20, 1),
            (24, 2), (28, 0), (508, 0),
        ):
            data = block()
            struct.pack_into("<I", data, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                validate(data)

    def test_noncontiguous(self):
        with self.assertRaises(ValueError):
            validate(block(0, 2) + block(1, 2, FLASH_START + 512))


if __name__ == "__main__":
    unittest.main()
