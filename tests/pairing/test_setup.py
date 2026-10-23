# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
"""Only synthetic records and in-memory serial streams. Never opens a device."""

from contextlib import contextmanager, redirect_stdout, redirect_stderr
import io
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import puckoff_setup as setup


FIXTURES = Path(__file__).parent / "fixtures"
A = bytes.fromhex("4433221188776655") + b"SYNTHETIC-A".ljust(16, b"\0")
B = bytes.fromhex("3d2c1b0a73625140") + b"SYNTHETIC-B".ljust(16, b"\0")
EMPTY = (None,) * 4
SOURCE = (A, B, None, None)
SOURCE_PORT = "/dev/synthetic-source"
TARGET_PORT = "/dev/synthetic-target"
LINE_CLEAR = json.loads((FIXTURES / "line-clear.json").read_bytes())


def fixture_response(name):
    return json.loads((FIXTURES / name).read_bytes()[len(setup.PREFIX):])


class FakeStream:
    def __init__(self, records=EMPTY, *, kind="puckoff"):
        self.records = tuple(records)
        self.kind = kind
        self.pending = bytearray()
        self.writes = []
        self.ops = []
        self.clear_ack = True
        self.clear_reply = bytes.fromhex(LINE_CLEAR["reply_hex"])
        self.chunk_size = 256
        self.errors = {}
        self.drop = set()
        self.hook = None
        self.reply_hook = None
        self.prefix = b""

    def read(self, size):
        size = min(size, self.chunk_size)
        result = bytes(self.pending[:size])
        del self.pending[:size]
        return result

    def write(self, data):
        self.writes.append(data)
        if data == b"\x15":
            if self.clear_ack:
                self.pending.extend(self.clear_reply)
            return len(data)
        if self.kind == "valve":
            assert data == b"bonds -h\r", "No other command is allowed on a Valve source"
            self.ops.append("bonds -h")
            self.pending.extend((FIXTURES / "valve-bonds.txt").read_bytes().replace(b"\n", b"\r\n"))
            return len(data)
        assert data.startswith(b"setup ") and data.endswith(b"\r")
        _, op, token, *args = data.decode("ascii").strip().split(" ")
        self.ops.append(op)
        if self.hook:
            self.hook(op, args)
        response = {"id": token, "op": op, "ok": True}
        if op in self.errors:
            response.update(ok=False, error=self.errors[op])
        elif op == "info":
            response = {**fixture_response("setup-info.txt"), **response}
        elif op == "status":
            response = {**fixture_response("setup-status.txt"), **response}
        elif op == "export":
            response["slots"] = setup.snapshot_document(self.records)["slots"]
        else:
            slot = int(args[0])
            candidate = setup.parse_record(args[1]) if op == "import" else None
            expected = None if args[-1] == "empty" else setup.parse_record(args[-1])
            if self.records[slot] != expected:
                response.update(ok=False, error="conflict")
            else:
                self.records = tuple(candidate if i == slot else old for i, old in enumerate(self.records))
                response["slot"] = slot
        if op in self.drop:
            return len(data)
        if self.reply_hook:
            response = self.reply_hook(response)
        payload = response if isinstance(response, bytes) else (
            setup.PREFIX + json.dumps(response, separators=(",", ":")).encode("ascii") + b"\r\n"
        )
        self.pending.extend(self.prefix + payload)
        return len(data)


class PairingTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="puckoff-pairing-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.serial_guard = patch.object(setup, "serial_module", side_effect=AssertionError("No hardware access"))
        self.serial_guard.start()
        self.addCleanup(self.serial_guard.stop)

    def wire(self, stream):
        return setup.Wire(stream, timeout=0.05)

    def cli(self, *argv, streams=None):
        self.opens = []

        @contextmanager
        def fake_open(port, kind):
            self.opens.append((port, kind))
            if streams is None or port not in streams or streams[port].kind != kind:
                raise setup.SetupError("wrong_device", "Synthetic device mismatch.")
            yield self.wire(streams[port])

        out, err = io.StringIO(), io.StringIO()
        with patch.object(setup, "open_wire", fake_open), redirect_stdout(out), redirect_stderr(err):
            code = setup.main(["--json", *map(str, argv)])
        self.assertEqual(err.getvalue(), "")
        return code, json.loads(out.getvalue())

    def import_args(self, *extra):
        return ("import", "--file", FIXTURES / "bonds.json", "--source-slot", "0",
                "--port", TARGET_PORT, "--slot", "0", *extra)


class RecordTests(PairingTest):
    def test_asymmetric_official_mapping_and_fixture_parity(self):
        records = setup.parse_valve_bonds((FIXTURES / "valve-bonds.txt").read_bytes())
        self.assertEqual(records, SOURCE)
        self.assertEqual(records[0][:4], bytes.fromhex("44332211"))
        self.assertEqual(records[0][4:8], bytes.fromhex("88776655"))
        self.assertEqual(setup.load_snapshot(FIXTURES / "bonds.json"), SOURCE)
        self.assertEqual(setup.load_snapshot(FIXTURES / "empty.json"), EMPTY)

    def test_full_width_and_empty_serial(self):
        for serial in (b"", b"ABCDEFGHIJKLMNOP"):
            self.assertEqual(setup.validate_record(A[:8] + serial.ljust(16, b"\0")), A[:8] + serial.ljust(16, b"\0"))

    def test_invalid_record_bytes(self):
        bad = [
            A[:-1], A + b"\0", bytes(24), bytes(4) + A[4:], A[:4] + bytes(4) + A[8:],
            A[:8] + b"BAD\n".ljust(16, b"\0"),
            A[:8] + b"BAD\xff".ljust(16, b"\0"),
            A[:8] + b"A\0B".ljust(16, b"\0"),
            A[:8] + b"WITH SPACE".ljust(16, b"\0"),
        ]
        for record in bad:
            with self.subTest(record_length=len(record)), self.assertRaises(setup.SetupError):
                setup.validate_record(record)

    def test_hex_record_validation(self):
        for value in ("", A.hex() + "00", "0x" + A.hex()[2:], "g" * 48, True, 123, [A.hex()]):
            with self.subTest(value_type=type(value).__name__), self.assertRaises(setup.SetupError):
                setup.parse_record(value)
        self.assertEqual(setup.parse_record(A.hex().upper()), A)
        self.assertIsNone(setup.parse_record(None))

    def test_snapshot_shape_and_duplicate_keys(self):
        with self.assertRaises(setup.SetupError):
            setup.parse_json(b'{"version":1,"version":1}')
        with self.assertRaises(setup.SetupError):
            setup.parse_json(b'{"version":NaN}')
        slots = setup.snapshot_document(SOURCE)["slots"]
        malformed = [slots[:3], slots + [slots[0]], "slots", None]
        for change in (
            {**slots[0], "slot": True},
            {**slots[0], "slot": 1},
            {**slots[0], "serial": "SYNTHETIC-LEAK"},
            {"slot": 0, "record": "0" * 48},
        ):
            malformed.append([change, *slots[1:]])
        for value in malformed:
            with self.subTest(value_type=type(value).__name__), self.assertRaises(setup.SetupError):
                setup.parse_slots(value)

    def test_backup_version_is_not_a_boolean(self):
        for version in (True, 0, 2, "1"):
            path = self.directory / "invalid.json"
            doc = setup.snapshot_document(SOURCE)
            doc["version"] = version
            path.write_text(json.dumps(doc))
            with self.assertRaises(setup.SetupError):
                setup.load_snapshot(path)

    def test_valve_terminal_codes_crlf_and_prompt_echo(self):
        data = (FIXTURES / "valve-bonds.txt").read_bytes()
        data = data.replace(b"\n", b"\r\n").replace(b"bonds -h", b"dongle~$ bonds -h")
        data = b"\x1b[32m" + data + b"\x1b[0m"
        self.assertEqual(setup.parse_valve_bonds(data), SOURCE)

    def test_valve_rejects_partial_ambiguous_or_unknown_output(self):
        original = (FIXTURES / "valve-bonds.txt").read_bytes()
        bad = [
            original.replace(b"ibex0_ibex_uuid", b"ibex0_proteus_uuid"),
            original.replace(b"ibex0_ibex_uuid", b"ibex4_ibex_uuid"),
            original.replace(b"0x11223344", b"0x112233445"),
            original.replace(b"0x11223344", b"0x00000000"),
            original.replace(b"0x11223344", b"+11223344"),
            original.replace(b"SYNTHETIC-A", b"ABCDEFGHIJKLMNOPQ"),
            original.replace(b"SYNTHETIC-A", b"BAD\x00VALUE"),
            original.replace(b"SYNTHETIC-A", b"SYNTHETIC-A "),
            original.replace(b"dongle~$", b"puckoff:~$"),
            original.replace(b"bonds -h\n", b""),
            original.replace(b"0x11223344", b"0x1122\r3344"),
            original + original,
            original[:-15],
            b"x" * (setup.RECEIVE_LIMIT + 1),
        ]
        for index, data in enumerate(bad):
            with self.subTest(case=index), self.assertRaises(setup.SetupError):
                setup.parse_valve_bonds(data)

    def test_private_backup_atomic_no_clobber(self):
        target = self.directory / "backup.json"
        setup.save_snapshot(target, SOURCE)
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o600)
        self.assertEqual(setup.load_snapshot(target), SOURCE)
        with self.assertRaises(setup.SetupError) as error:
            setup.save_snapshot(target, EMPTY)
        self.assertEqual(error.exception.code, "file_exists")
        self.assertEqual(setup.load_snapshot(target), SOURCE)
        self.assertEqual(sorted(p.name for p in self.directory.iterdir()), ["backup.json"])

    def test_backup_refuses_symlink_target(self):
        target = self.directory / "backup.json"
        target.symlink_to(self.directory / "not-created")
        with self.assertRaises(setup.SetupError):
            setup.save_snapshot(target, SOURCE)
        self.assertFalse((self.directory / "not-created").exists())

    def test_file_size_and_nonregular_paths(self):
        path = self.directory / "oversize.json"
        path.write_bytes(b"x" * (setup.FILE_LIMIT + 1))
        with self.assertRaises(setup.SetupError):
            setup.load_snapshot(path)
        pipe = self.directory / "not-a-backup"
        os.mkfifo(pipe)
        with self.assertRaises(setup.SetupError):
            setup.read_file(pipe)
        link = self.directory / "link"
        link.symlink_to(path)
        with self.assertRaises(setup.SetupError):
            setup.read_file(link)


class TransportTests(PairingTest):
    def test_longest_guarded_request_fits_firmware_receive_buffers(self):
        stream = FakeStream((A, None, None, None))
        self.wire(stream).request("import", "0", B.hex(), A.hex())
        command_size = len(stream.writes[-1])
        config = (ROOT / "firmware" / "prj.conf").read_text()
        for name in ("SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE", "SHELL_CMD_BUFF_SIZE"):
            size = re.search(rf"^CONFIG_{name}=(\d+)$", config, re.MULTILINE)
            self.assertIsNotNone(size)
            self.assertLess(command_size, int(size[1]))

    def test_baseline_clear_fixture_and_explicit_zero_parameter(self):
        self.assertEqual(bytes.fromhex(LINE_CLEAR["request_hex"]), b"\x15")
        for reply in (bytes.fromhex(LINE_CLEAR["reply_hex"]), b"\x1b[0J"):
            stream = FakeStream()
            stream.clear_reply = reply
            stream.chunk_size = 1
            self.assertTrue(self.wire(stream).request("info")["ok"])

    def test_invalid_requests_never_touch_the_stream(self):
        stream = FakeStream()
        for request in (
            ("bootloader",), ("info\rbootloader",), ("info", "extra"),
            ("import", "0", A.hex(), "empty\rbootloader"),
            ("import", "4", A.hex(), "empty"), ("clear", "-1", "empty"),
            ("import", "0", "not-a-record", "empty"),
        ):
            with self.subTest(op=request[0]), self.assertRaises(setup.SetupError):
                self.wire(stream).request(*request)
        self.assertEqual(stream.writes, [])

    def test_fresh_tokens_fragmentation_and_ignored_noise(self):
        stream = FakeStream()
        stream.chunk_size = 1
        stale = fixture_response("setup-info.txt")
        stream.prefix = b"\x1b[32mpuckoff:~$ setup info old\x1b[0m\r\n" + (
            setup.PREFIX + json.dumps(stale).encode() + b"\n"
        )
        with patch.object(setup.secrets, "token_hex", return_value="fedcba9876543210"):
            info = self.wire(stream).request("info")
        self.assertEqual(info["id"], "fedcba9876543210")
        self.assertEqual(stream.writes, [b"\x15", b"setup info fedcba9876543210\r"])

    def test_no_cr_without_line_clear_acknowledgment(self):
        stream = FakeStream()
        stream.clear_ack = False
        with self.assertRaises(setup.SetupError) as error:
            self.wire(stream).request("info")
        self.assertEqual(error.exception.code, "unsafe_console")
        self.assertEqual(stream.writes, [b"\x15"])

    def test_valve_has_one_allowlisted_query_and_no_fallback(self):
        stream = FakeStream(kind="valve")
        self.assertEqual(self.wire(stream).valve_bonds(), SOURCE)
        self.assertEqual(stream.writes, [b"\x15", b"bonds -h\r"])

    def test_bounded_unknown_and_malformed_frames(self):
        hooks = [
            lambda reply: {**reply, "op": "other"},
            lambda reply: {**reply, "ok": "true"},
            lambda reply: {**reply, "ok": False, "error": []},
            lambda reply: {**reply, "ok": False, "error": "unrecognized"},
            lambda reply: setup.PREFIX + b'{"id":invalid}\n',
            lambda reply: setup.PREFIX + b" " * setup.LINE_LIMIT + b"{}\n",
            lambda reply: b"x" * (setup.LINE_LIMIT * 4 + 1),
            lambda reply: b"noise\n" * setup.RECEIVE_LIMIT,
        ]
        for hook in hooks:
            stream = FakeStream()
            stream.reply_hook = hook
            with self.subTest(hook=hooks.index(hook)), self.assertRaises(setup.SetupError):
                self.wire(stream).request("info")

    def test_stale_response_cannot_satisfy_request(self):
        stream = FakeStream()
        stream.reply_hook = lambda reply: {**reply, "id": "not-the-request"}
        with self.assertRaises(setup.SetupError) as error:
            self.wire(stream).request("info")
        self.assertEqual(error.exception.code, "timeout")

    def test_status_types_privacy_and_counter_limits(self):
        for field, value in (
            ("slot", True), ("rssi", 1), ("rssi", -256),
            ("polls", 0x100000000), ("replies", -1), ("reports", True),
            ("connected", "yes"), ("controller_id", "SYNTHETIC-LEAK"),
        ):
            stream = FakeStream()

            def corrupt(reply):
                if reply["op"] == "status":
                    reply["slots"][0][field] = value
                return reply

            stream.reply_hook = corrupt
            with self.subTest(field=field), self.assertRaises(setup.SetupError):
                setup.Receiver(self.wire(stream)).status()
        stream = FakeStream()
        stream.reply_hook = lambda reply: (
            {**reply, "storage_ready": False} if reply["op"] == "status" else reply
        )
        self.assertFalse(setup.Receiver(self.wire(stream)).status()["storage_ready"])

    def test_incompatible_firmware_never_gets_an_export(self):
        for field, value in (
            ("protocol", True), ("protocol", 2), ("slots", 8),
            ("product", "Steam Controller Puck"), ("record_bytes", 25),
            ("capabilities", ["status"]), ("capabilities", [{}]),
        ):
            stream = FakeStream()
            stream.reply_hook = lambda reply: {**reply, field: value}
            with self.subTest(field=field), self.assertRaises(setup.SetupError):
                setup.Receiver(self.wire(stream))
            self.assertEqual(stream.ops, ["info"])

    def test_descriptor_selection_refuses_before_open(self):
        with patch.object(setup, "list_devices", return_value=[
            {"port": TARGET_PORT, "kind": "valve"}
        ]):
            with self.assertRaises(setup.SetupError):
                with setup.open_wire(TARGET_PORT, "puckoff"):
                    self.fail("wrong device opened")

    def test_device_list_does_not_open_or_expose_serials(self):
        def port(device, vid, pid, product, manufacturer):
            return SimpleNamespace(
                device=device, vid=vid, pid=pid, product=product, manufacturer=manufacturer,
                serial_number="SYNTHETIC-SECRET", hwid="SYNTHETIC-SECRET",
            )

        ports = [
            port("/dev/test-a", 0x1209, 1, "PuckOff Receiver", "PuckOff"),
            port("/dev/test-b", 0x28DE, 0x1304, "Steam Controller Puck", "Valve Software"),
            port("/dev/test-c", 0x28DE, 0x1142, "Steam Controller", "Valve Software"),
            port("/dev/test-d", 0x1209, 1, "Unrelated device", "PuckOff"),
        ]
        fake = SimpleNamespace(tools=SimpleNamespace(list_ports=SimpleNamespace(comports=lambda: ports)))
        with patch.object(setup, "serial_module", return_value=fake):
            devices = setup.list_devices()
        self.assertEqual([d["kind"] for d in devices], ["puckoff", "valve", "unsupported", "unsupported"])
        self.assertNotIn("SYNTHETIC-SECRET", json.dumps(devices))


class WorkflowTests(PairingTest):
    def test_offline_plan_requires_no_port_dependency(self):
        code, result = self.cli(
            "import", "--file", FIXTURES / "bonds.json", "--source-slot", "0",
            "--target-file", FIXTURES / "empty.json", "--slot", "3",
        )
        self.assertEqual(code, 0)
        self.assertFalse(result["applied"])
        self.assertEqual(result["change"], "add")
        self.assertEqual(self.opens, [])

    def test_offline_apply_refused(self):
        code, result = self.cli(
            "import", "--file", FIXTURES / "bonds.json", "--source-slot", "0",
            "--target-file", FIXTURES / "empty.json", "--slot", "0", "--apply",
        )
        self.assertEqual(code, 1)
        self.assertEqual(result["error"], "offline_only")
        self.assertEqual(self.opens, [])

    def test_import_default_and_explicit_dry_run_never_write(self):
        for options in ((), ("--dry-run",)):
            stream = FakeStream()
            code, result = self.cli(*self.import_args(*options), streams={TARGET_PORT: stream})
            self.assertEqual(code, 0)
            self.assertFalse(result["applied"])
            self.assertEqual(stream.records, EMPTY)
            self.assertEqual(stream.ops, ["info", "export"])

    def test_import_empty_slot_with_exact_guard_and_readback(self):
        stream = FakeStream()
        code, result = self.cli(*self.import_args("--apply"), streams={TARGET_PORT: stream})
        self.assertEqual(code, 0)
        self.assertTrue(result["applied"])
        self.assertEqual(stream.records, (A, None, None, None))
        self.assertEqual(stream.ops, ["info", "export", "import", "export"])
        self.assertIn((A.hex() + " empty\r").encode(), stream.writes[5])
        self.assertNotIn(A.hex(), json.dumps(result))
        self.assertNotIn("SYNTHETIC-A", json.dumps(result))

    def test_occupied_and_missing_backup_fail_before_mutation(self):
        for options, error in (
            (("--apply",), "occupied"),
            (("--apply", "--replace"), "backup_required"),
        ):
            stream = FakeStream((B, None, None, None))
            code, result = self.cli(*self.import_args(*options), streams={TARGET_PORT: stream})
            self.assertEqual(code, 1)
            self.assertEqual(result["error"], error)
            self.assertEqual(stream.records[0], B)
            self.assertNotIn("import", stream.ops)

    def test_replacement_backs_up_before_compare_and_swap(self):
        backup = self.directory / "before.json"
        stream = FakeStream((B, None, None, None))

        def require_backup(op, args):
            if op == "import":
                self.assertEqual(setup.load_snapshot(backup), (B, None, None, None))
                self.assertEqual(args[-1], B.hex())

        stream.hook = require_backup
        code, result = self.cli(
            *self.import_args("--apply", "--replace", "--backup", backup),
            streams={TARGET_PORT: stream},
        )
        self.assertEqual(code, 0)
        self.assertEqual(result["change"], "replace")
        self.assertEqual(stream.records, (A, None, None, None))

    def test_backup_collision_or_io_failure_prevents_write(self):
        for exists in (False, True):
            backup = self.directory / ("existing.json" if exists else "missing/backup.json")
            if exists:
                backup.write_text("DO NOT OVERWRITE")
            stream = FakeStream((B, None, None, None))
            code, result = self.cli(
                *self.import_args("--apply", "--replace", "--backup", backup),
                streams={TARGET_PORT: stream},
            )
            self.assertEqual(code, 1)
            self.assertEqual(result["error"], "file_exists" if exists else "file_io")
            self.assertNotIn("import", stream.ops)
            if exists:
                self.assertEqual(backup.read_text(), "DO NOT OVERWRITE")

    def test_duplicate_controller_and_empty_source_refused(self):
        stream = FakeStream((None, A, None, None))
        code, result = self.cli(*self.import_args("--apply"), streams={TARGET_PORT: stream})
        self.assertEqual((code, result["error"]), (1, "duplicate"))
        self.assertNotIn("import", stream.ops)
        with self.assertRaises(setup.SetupError):
            setup.plan_import(EMPTY, 0, EMPTY, 0, False)

    def test_identical_import_is_idempotent_without_flash_write(self):
        stream = FakeStream((A, None, None, None))
        code, result = self.cli(*self.import_args("--apply"), streams={TARGET_PORT: stream})
        self.assertEqual(code, 0)
        self.assertEqual(result["change"], "none")
        self.assertEqual(stream.ops, ["info", "export"])

    def test_concurrent_change_conflicts_without_overwrite(self):
        stream = FakeStream()

        def concurrent_change(op, args):
            if op == "import":
                stream.records = (B, None, None, None)

        stream.hook = concurrent_change
        code, result = self.cli(*self.import_args("--apply"), streams={TARGET_PORT: stream})
        self.assertEqual((code, result["error"]), (1, "conflict"))
        self.assertEqual(stream.records[0], B)
        self.assertEqual(stream.ops.count("import"), 1)

    def test_storage_error_propagates_without_success(self):
        stream = FakeStream()
        stream.errors["import"] = "storage_io"
        code, result = self.cli(*self.import_args("--apply"), streams={TARGET_PORT: stream})
        self.assertEqual((code, result["error"]), (1, "storage_io"))
        self.assertEqual(stream.records, EMPTY)

    def test_lost_confirmation_never_retries_a_possible_write(self):
        stream = FakeStream()
        stream.drop.add("import")
        code, result = self.cli(*self.import_args("--apply"), streams={TARGET_PORT: stream})
        self.assertEqual((code, result["error"]), (1, "outcome_unknown"))
        self.assertEqual(stream.ops.count("import"), 1)
        self.assertEqual(stream.records[0], A)

    def test_readback_mismatch_is_not_success(self):
        stream = FakeStream()

        def change_before_readback(op, args):
            if op == "export" and "import" in stream.ops:
                stream.records = EMPTY

        stream.hook = change_before_readback
        code, result = self.cli(*self.import_args("--apply"), streams={TARGET_PORT: stream})
        self.assertEqual((code, result["error"]), (1, "verification_failed"))

    def test_clear_requires_apply_and_backup_and_keeps_other_slots(self):
        stream = FakeStream(SOURCE)
        args = ("clear", "--port", TARGET_PORT, "--slot", "0")
        code, result = self.cli(*args, streams={TARGET_PORT: stream})
        self.assertEqual(code, 0)
        self.assertFalse(result["applied"])
        self.assertEqual(stream.records, SOURCE)
        code, result = self.cli(*args, "--apply", streams={TARGET_PORT: stream})
        self.assertEqual((code, result["error"]), (1, "backup_required"))
        backup = self.directory / "before-clear.json"
        code, result = self.cli(*args, "--apply", "--backup", backup, streams={TARGET_PORT: stream})
        self.assertEqual(code, 0)
        self.assertEqual(stream.records, (None, B, None, None))
        self.assertEqual(setup.load_snapshot(backup), SOURCE)

    def test_migration_preserves_source_and_saves_backup_before_write(self):
        backup = self.directory / "source.json"
        source, destination = FakeStream(kind="valve"), FakeStream()

        def require_backup(op, args):
            if op == "import":
                self.assertEqual(setup.load_snapshot(backup), SOURCE)

        destination.hook = require_backup
        args = (
            "migrate", "--source-port", SOURCE_PORT, "--source-kind", "valve",
            "--source-slot", "0", "--source-backup", backup,
            "--port", TARGET_PORT, "--slot", "2",
        )
        streams = {SOURCE_PORT: source, TARGET_PORT: destination}
        code, result = self.cli(*args, streams=streams)
        self.assertEqual(code, 0)
        self.assertFalse(result["applied"])
        self.assertFalse(backup.exists())
        self.assertEqual(destination.records, EMPTY)
        code, result = self.cli(*args, "--apply", streams=streams)
        self.assertEqual(code, 0)
        self.assertEqual(destination.records, (None, None, A, None))
        self.assertEqual(source.ops, ["bonds -h", "bonds -h"])
        self.assertEqual(self.opens, [(SOURCE_PORT, "valve"), (TARGET_PORT, "puckoff")])

    def test_same_port_migration_refused_without_open(self):
        code, result = self.cli(
            "migrate", "--source-port", TARGET_PORT, "--source-kind", "puckoff",
            "--source-slot", "0", "--source-backup", self.directory / "source.json",
            "--port", TARGET_PORT, "--slot", "1", "--apply",
        )
        self.assertEqual((code, result["error"]), (1, "same_device"))
        self.assertEqual(self.opens, [])

    def test_export_and_convert_are_private_and_never_print_records(self):
        for command in ("export", "convert"):
            target = self.directory / f"{command}.json"
            if command == "export":
                args = (command, "--port", SOURCE_PORT, "--kind", "valve", "--output", target)
            else:
                args = (command, "--input", FIXTURES / "valve-bonds.txt", "--output", target)
            code, result = self.cli(*args, streams={SOURCE_PORT: FakeStream(kind="valve")})
            self.assertEqual(code, 0)
            self.assertEqual(setup.load_snapshot(target), SOURCE)
            self.assertNotIn(A.hex(), json.dumps(result))
            self.assertNotIn("SYNTHETIC-A", json.dumps(result))

    def test_malformed_private_file_error_withholds_contents(self):
        private = self.directory / "bad.json"
        private.write_text('{"SYNTHETIC-PRIVATE-SERIAL":invalid}')
        code, result = self.cli("check", "--file", private)
        self.assertEqual(code, 1)
        self.assertNotIn("SYNTHETIC-PRIVATE-SERIAL", json.dumps(result))


if __name__ == "__main__":
    unittest.main()
