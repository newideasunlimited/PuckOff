# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
"""Compile real bond/console code against in-memory settings and host locks."""

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


HERE = Path(__file__).parent
ROOT = HERE.parents[1]


class FirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise RuntimeError("A host C compiler is required for pairing firmware tests.")
        cls.temporary = tempfile.TemporaryDirectory(prefix="puckoff-pairing-c-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.binary = Path(cls.temporary.name) / "pairing-tests"
        result = subprocess.run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-Wno-unused-function", "-pthread",
            "-D_POSIX_C_SOURCE=200809L", "-I", str(HERE / "shims"),
            str(HERE / "test_firmware.c"), "-o", str(cls.binary),
        ], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run(
            [str(self.binary), case], capture_output=True, timeout=20
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        return result.stdout

    def test_firmware_storage_validation_transactions_and_concurrency(self):
        for case in ("storage", "validation", "transactions", "load-errors", "concurrency", "console"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_info_and_status_match_shared_host_fixtures_byte_for_byte(self):
        for op in ("info", "status"):
            with self.subTest(op=op):
                response = self.run_case(op)
                self.assertEqual(response, (HERE / "fixtures" / f"setup-{op}.txt").read_bytes())
                self.assertLessEqual(len(response.rstrip(b"\n")), 1024)
                self.assertEqual(json.loads(response[10:])["op"], op)

    def test_export_matches_portable_backup_slots(self):
        response = self.run_case("export")
        expected = json.loads((HERE / "fixtures" / "bonds.json").read_bytes())
        self.assertEqual(json.loads(response[10:])["slots"], expected["slots"])


if __name__ == "__main__":
    unittest.main()
