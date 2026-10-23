#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
"""Explicit-device, local-only PuckOff setup. Offline commands need only Python."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import secrets
import stat
import sys
import tempfile
import time
from typing import Iterator


SLOTS = 4
RECORD_BYTES = 24
FORMAT = "org.puckoff.bonds"
PREFIX = b"PUCKOFF/1 "
LINE_LIMIT = 1024
RECEIVE_LIMIT = 16384
FILE_LIMIT = 65536
TIMEOUT = 5.0
ANSI = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")
HEX_RECORD = re.compile(r"[0-9a-fA-F]{48}")
ERRORS = {"invalid_argument", "storage_unavailable", "conflict", "storage_io"}
Snapshot = tuple[bytes | None, ...]


class SetupError(Exception):
    """A privacy-safe error: never attach the command, record or serial transcript."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def invalid(message: str) -> SetupError:
    return SetupError("invalid_data", message)


def json_object(pairs: list[tuple[str, object]]) -> dict:
    result: dict = {}
    for key, value in pairs:
        if key in result:
            raise invalid("Duplicate JSON fields are not accepted.")
        result[key] = value
    return result


def invalid_constant(value: str):
    raise invalid("Non-finite JSON numbers are not accepted.")


def parse_json(data: bytes) -> object:
    try:
        return json.loads(data, object_pairs_hook=json_object, parse_constant=invalid_constant)
    except (ValueError, UnicodeError, RecursionError):
        raise invalid("Malformed JSON; contents withheld.") from None


def require_fields(value: object, fields: set[str]) -> dict:
    if not isinstance(value, dict) or set(value) != fields:
        raise invalid("Unexpected fields or object shape; contents withheld.")
    return value


def validate_record(record: bytes) -> bytes:
    if len(record) != RECORD_BYTES:
        raise invalid("A pairing record must contain exactly 24 bytes.")
    receiver = int.from_bytes(record[:4], "little")
    controller = int.from_bytes(record[4:8], "little")
    serial, _, padding = record[8:].partition(b"\0")
    if not receiver or not controller:
        raise invalid("Used records require two nonzero identifiers.")
    if any(c < 0x21 or c > 0x7E for c in serial) or any(padding):
        raise invalid("Serial bytes must be printable ASCII with zero padding.")
    return record


def parse_record(text: object) -> bytes | None:
    if text is None:
        return None
    if not isinstance(text, str) or not HEX_RECORD.fullmatch(text):
        raise invalid("A used slot must contain exactly 48 hexadecimal characters.")
    return validate_record(bytes.fromhex(text))


def parse_slots(value: object) -> Snapshot:
    if not isinstance(value, list) or len(value) != SLOTS:
        raise invalid("A snapshot must contain all four slots.")
    records = []
    for slot, entry in enumerate(value):
        entry = require_fields(entry, {"slot", "record"})
        if type(entry["slot"]) is not int or entry["slot"] != slot:
            raise invalid("Slots must be ordered 0, 1, 2, 3 without duplicates.")
        records.append(parse_record(entry["record"]))
    return tuple(records)


def snapshot_document(records: Snapshot) -> dict:
    return {
        "format": FORMAT,
        "version": 1,
        "slots": [
            {"slot": slot, "record": record.hex() if record else None}
            for slot, record in enumerate(records)
        ],
    }


def read_file(path: Path) -> bytes:
    try:
        before = path.lstat()
        if not stat.S_ISREG(before.st_mode):
            raise invalid("Select a regular file, not a symlink, device or pipe.")
        flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0)
        with os.fdopen(os.open(path, flags), "rb") as stream:
            current = os.fstat(stream.fileno())
            if not stat.S_ISREG(current.st_mode) or (
                before.st_dev, before.st_ino
            ) != (current.st_dev, current.st_ino):
                raise invalid("Selected file changed while opening it.")
            data = stream.read(FILE_LIMIT + 1)
    except OSError:
        raise SetupError("file_io", "Cannot read the selected file.") from None
    if len(data) > FILE_LIMIT:
        raise invalid("The selected file exceeds the 64 KiB size limit.")
    return data


def load_snapshot(path: Path) -> Snapshot:
    doc = require_fields(parse_json(read_file(path)), {"format", "version", "slots"})
    if doc["format"] != FORMAT or type(doc["version"]) is not int or doc["version"] != 1:
        raise invalid("Unsupported pairing backup format or version.")
    return parse_slots(doc["slots"])


def save_snapshot(path: Path, records: Snapshot) -> None:
    """Publish a complete 0600 file without replacing any existing path."""
    data = (json.dumps(snapshot_document(records), indent=2) + "\n").encode("ascii")
    temporary: str | None = None
    try:
        fd, temporary = tempfile.mkstemp(prefix=".puckoff-", dir=path.parent)
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary, path, follow_symlinks=False)
        os.unlink(temporary)
        temporary = None
        directory = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except FileExistsError:
        raise SetupError("file_exists", "Backup already exists; choose a new path.") from None
    except OSError:
        raise SetupError("file_io", "Cannot create a private backup in the selected directory.") from None
    finally:
        if temporary is not None:
            try:
                os.unlink(temporary)
            except OSError:
                raise SetupError("file_io", "Could not remove the private temporary backup.") from None


def parse_valve_bonds(data: bytes) -> Snapshot:
    if len(data) > RECEIVE_LIMIT:
        raise invalid("Official-puck response exceeds the receive limit.")
    try:
        text = ANSI.sub(b"", data).decode("ascii")
    except UnicodeError:
        raise invalid("Official-puck output is not ASCII; contents withheld.") from None
    if any(ord(c) < 0x20 and c not in "\r\n\t" for c in text):
        raise invalid("Unexpected control bytes in official-puck output.")
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    lines = [line.lstrip(" \t") for line in lines if line.strip(" \t")]
    if (
        len(lines) != 14
        or lines[0].rstrip(" \t") not in ("bonds -h", "dongle~$ bonds -h")
        or lines[-1].rstrip(" \t") != "dongle~$"
    ):
        raise invalid("Expected one complete `bonds -h` response and dongle prompt.")
    fields: dict[tuple[int, str], str] = {}
    for line in lines[1:-1]:
        match = re.fullmatch(
            r"ibex([0-3])_(proteus_uuid|ibex_uuid|peer_sn)[ \t]*:[ \t]*(.*)", line
        )
        if not match:
            raise invalid("Unrecognized official-puck bond field; contents withheld.")
        key = (int(match[1]), match[2])
        if key in fields:
            raise invalid("Duplicate official-puck bond field.")
        fields[key] = match[3]
    result: list[bytes | None] = []
    for slot in range(SLOTS):
        try:
            receiver = fields[(slot, "proteus_uuid")]
            controller = fields[(slot, "ibex_uuid")]
            serial = fields[(slot, "peer_sn")]
        except KeyError:
            raise invalid("Incomplete official-puck slot.") from None
        if not all(re.fullmatch(r"0x[0-9a-fA-F]{8}", value) for value in (receiver, controller)):
            raise invalid("Official-puck identifiers must have exactly eight hex digits.")
        if not re.fullmatch(r"[\x21-\x7e]{0,16}", serial):
            raise invalid("Official-puck serial must contain at most 16 printable ASCII bytes.")
        # Verified hardware mapping: proteus is the receiver, ibex the controller.
        record = (
            int(receiver, 16).to_bytes(4, "little")
            + int(controller, 16).to_bytes(4, "little")
            + serial.encode("ascii").ljust(16, b"\0")
        )
        result.append(None if record == bytes(RECORD_BYTES) else validate_record(record))
    return tuple(result)


def serial_module():
    try:
        import serial
        import serial.tools.list_ports
    except ImportError:
        raise SetupError(
            "dependency_missing", "Install the live-port dependency with pip install -r tools/requirements.txt."
        ) from None
    return serial


def list_devices() -> list[dict]:
    serial = serial_module()
    devices = []
    for port in serial.tools.list_ports.comports():
        kind = "unsupported"
        if (
            (port.vid, port.pid) == (0x1209, 0x0001)
            and port.product == "PuckOff Receiver"
            and port.manufacturer == "PuckOff"
        ):
            kind = "puckoff"
        elif (
            (port.vid, port.pid) == (0x28DE, 0x1304)
            and port.product == "Steam Controller Puck"
            and port.manufacturer == "Valve Software"
        ):
            kind = "valve"
        # Do not return USB serial_number, HWID, or arbitrary descriptor text.
        devices.append({"port": port.device, "kind": kind, "vid": port.vid, "pid": port.pid})
    return sorted(devices, key=lambda device: device["port"])


class Wire:
    def __init__(self, stream, timeout: float = TIMEOUT):
        self.stream = stream
        self.timeout = timeout

    def write(self, data: bytes) -> None:
        try:
            if self.stream.write(data) != len(data):
                raise SetupError("transport_io", "Incomplete serial write; reconnect and inspect before retrying.")
        except OSError:
            raise SetupError("transport_io", "Serial write failed; reconnect and inspect before retrying.") from None

    def read(self) -> bytes:
        try:
            return self.stream.read(256)
        except OSError:
            raise SetupError("transport_io", "Serial read failed; reconnect and inspect before retrying.") from None

    def drain(self) -> None:
        deadline, total = time.monotonic() + self.timeout, 0
        while time.monotonic() < deadline:
            data = self.read()
            if not data:
                return
            total += len(data)
            if total > RECEIVE_LIMIT:
                break
        raise SetupError("busy", "Console output did not settle; close other terminals and turn watch off.")

    def prepare(self) -> None:
        self.drain()
        # A CR could execute another terminal's unfinished command. Ctrl-U clears
        # Zephyr's edit buffer; require its VT100 acknowledgment before any CR.
        self.write(b"\x15")
        deadline = time.monotonic() + self.timeout
        received = bytearray()
        while time.monotonic() < deadline:
            received.extend(self.read())
            if len(received) > RECEIVE_LIMIT:
                break
            if b"\x1b[J" in received or b"\x1b[0J" in received:
                self.drain()
                return
        raise SetupError(
            "unsafe_console", "No line-clear acknowledgment; no command was sent. Use the default echo-enabled Zephyr shell."
        )

    def request(self, op: str, *arguments: str) -> dict:
        arity = {"info": 0, "status": 0, "export": 0, "import": 3, "clear": 2}
        if op not in arity or len(arguments) != arity[op]:
            raise invalid("Unknown management operation or wrong argument count.")
        if arguments:
            if not isinstance(arguments[0], str) or not re.fullmatch(r"[0-3]", arguments[0]):
                raise invalid("Destination slot must be 0, 1, 2, or 3.")
            if op == "import" and parse_record(arguments[1]) is None:
                raise invalid("Import requires a used record.")
            if arguments[-1] != "empty" and parse_record(arguments[-1]) is None:
                raise invalid("Expected record must be a used record or empty.")
        token = secrets.token_hex(8)
        command = " ".join(("setup", op, token, *arguments))
        self.prepare()
        self.write(command.encode("ascii") + b"\r")
        deadline = time.monotonic() + self.timeout
        pending = bytearray()
        total = 0
        while time.monotonic() < deadline:
            data = self.read()
            pending.extend(data)
            total += len(data)
            if total > RECEIVE_LIMIT:
                raise invalid("Response exceeded the total receive limit.")
            while b"\n" in pending:
                raw, _, rest = pending.partition(b"\n")
                pending = bytearray(rest)
                if len(raw) > LINE_LIMIT * 4:
                    raise invalid("Serial line exceeded the wire limit.")
                line = ANSI.sub(b"", raw).strip(b"\r")
                if not line.startswith(PREFIX):
                    continue
                if len(line) > LINE_LIMIT:
                    raise invalid("Management response exceeded 1024 bytes.")
                response = parse_json(line[len(PREFIX):])
                if not isinstance(response, dict):
                    raise invalid("Expected a management response object.")
                if response.get("id") != token:
                    continue
                if response.get("op") != op or type(response.get("ok")) is not bool:
                    raise invalid("Mismatched management response.")
                if not response["ok"]:
                    require_fields(response, {"id", "op", "ok", "error"})
                    if not isinstance(response["error"], str) or response["error"] not in ERRORS:
                        raise invalid("Unknown management error.")
                    raise SetupError(response["error"], "Receiver rejected the request; no pairing data is shown.")
                return response
            if len(pending) > LINE_LIMIT * 4:
                raise invalid("Unterminated serial line exceeded the wire limit.")
        if op in ("import", "clear"):
            raise SetupError("outcome_unknown", "No write confirmation. Inspect the destination before retrying; it may have saved.")
        raise SetupError("timeout", "No matching management response; check the port and firmware version.")

    def valve_bonds(self) -> Snapshot:
        self.prepare()
        self.write(b"bonds -h\r")
        deadline = time.monotonic() + self.timeout
        received = bytearray()
        while time.monotonic() < deadline:
            received.extend(self.read())
            if len(received) > RECEIVE_LIMIT:
                raise invalid("Official-puck response exceeded the receive limit.")
            if ANSI.sub(b"", received).rstrip().endswith(b"dongle~$"):
                return parse_valve_bonds(bytes(received))
        raise SetupError("timeout", "No complete official-puck response; no fallback commands were sent.")


@contextmanager
def open_wire(port: str, kind: str) -> Iterator[Wire]:
    matches = [device for device in list_devices() if device["port"] == port]
    if len(matches) != 1 or matches[0]["kind"] != kind:
        raise SetupError("wrong_device", "Explicit port does not match the required USB product; run devices again.")
    serial = serial_module()
    stream = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=1)
    try:
        stream.dtr = False
        stream.rts = False
        if os.name == "posix":
            stream.exclusive = True
        stream.port = port
        stream.open()
        stream.dtr = True
        yield Wire(stream)
    except (OSError, serial.SerialException):
        raise SetupError("transport_io", "Cannot use the selected port; close other terminals and inspect before retrying.") from None
    finally:
        stream.close()


class Receiver:
    def __init__(self, wire: Wire):
        self.wire = wire
        info = wire.request("info")
        require_fields(info, {
            "id", "op", "ok", "protocol", "product", "firmware", "slots",
            "record_bytes", "capabilities", "native_pairing",
        })
        if (
            type(info["protocol"]) is not int or info["protocol"] != 1
            or info["product"] != "PuckOff Receiver"
            or type(info["slots"]) is not int or info["slots"] != SLOTS
            or type(info["record_bytes"]) is not int or info["record_bytes"] != RECORD_BYTES
            or not isinstance(info["firmware"], str)
            or not re.fullmatch(r"[A-Za-z0-9._+-]{1,32}", info["firmware"])
            or type(info["native_pairing"]) is not bool
            or not isinstance(info["capabilities"], list)
            or any(not isinstance(value, str) for value in info["capabilities"])
            or set(info["capabilities"]) != {"status", "export", "import", "clear"}
            or len(info["capabilities"]) != 4
        ):
            raise invalid("Unsupported PuckOff management contract.")
        self.info = info

    def export(self) -> Snapshot:
        response = self.wire.request("export")
        require_fields(response, {"id", "op", "ok", "slots"})
        return parse_slots(response["slots"])

    def status(self) -> dict:
        response = self.wire.request("status")
        require_fields(response, {"id", "op", "ok", "storage_ready", "slots"})
        if type(response["storage_ready"]) is not bool:
            raise invalid("Invalid storage status.")
        if not isinstance(response["slots"], list) or len(response["slots"]) != SLOTS:
            raise invalid("Status must contain four slots.")
        for slot, item in enumerate(response["slots"]):
            require_fields(item, {
                "slot", "used", "connected", "powered_off", "rssi",
                "polls", "replies", "reports", "crc_errors",
            })
            if (
                type(item["slot"]) is not int or item["slot"] != slot
                or any(type(item[key]) is not bool for key in ("used", "connected", "powered_off"))
                or type(item["rssi"]) is not int or not -255 <= item["rssi"] <= 0
                or any(
                    type(item[key]) is not int or not 0 <= item[key] <= 0xFFFFFFFF
                    for key in ("polls", "replies", "reports", "crc_errors")
                )
            ):
                raise invalid("Invalid status fields; contents withheld.")
        return {"storage_ready": response["storage_ready"], "slots": response["slots"]}

    def change(self, slot: int, record: bytes | None, expected: bytes | None) -> None:
        guard = expected.hex() if expected else "empty"
        op, args = ("clear", (str(slot), guard)) if record is None else (
            "import", (str(slot), record.hex(), guard)
        )
        response = self.wire.request(op, *args)
        require_fields(response, {"id", "op", "ok", "slot"})
        if type(response["slot"]) is not int or response["slot"] != slot:
            raise invalid("Receiver acknowledged a different slot; inspect before retrying.")
        if self.export()[slot] != record:
            raise SetupError("verification_failed", "Readback did not match. Inspect the destination before retrying.")


def read_source(port: str, kind: str) -> Snapshot:
    with open_wire(port, kind) as wire:
        return Receiver(wire).export() if kind == "puckoff" else wire.valve_bonds()


def plan_import(source: Snapshot, source_slot: int, target: Snapshot, slot: int, replace: bool) -> dict:
    record = source[source_slot]
    if record is None:
        raise invalid("The selected source slot is empty.")
    before = target[slot]
    if before == record:
        return {"slot": slot, "source_slot": source_slot, "change": "none"}
    if before is not None and not replace:
        raise SetupError("occupied", "Destination slot is occupied. Replacement requires --replace and a new --backup path.")
    if any(
        i != slot and existing is not None and existing[4:8] == record[4:8]
        for i, existing in enumerate(target)
    ):
        raise SetupError("duplicate", "This controller already occupies another destination slot.")
    return {"slot": slot, "source_slot": source_slot, "change": "replace" if before else "add"}


def perform_import(args, source: Snapshot, *, source_backup: Path | None = None) -> dict:
    if args.target_file:
        if args.apply:
            raise SetupError("offline_only", "--target-file permits planning only; --apply requires an explicit live port.")
        target = load_snapshot(args.target_file)
        plan = plan_import(source, args.source_slot, target, args.slot, args.replace)
        return {"applied": False, **plan}
    with open_wire(args.port, "puckoff") as wire:
        receiver = Receiver(wire)
        target = receiver.export()
        plan = plan_import(source, args.source_slot, target, args.slot, args.replace)
        if not args.apply:
            return {"applied": False, **plan}
        if plan["change"] == "replace" and not args.backup:
            raise SetupError("backup_required", "Replacement requires a new --backup path for the destination snapshot.")
        if source_backup is not None:
            save_snapshot(source_backup, source)
        if plan["change"] != "none":
            if args.backup:
                save_snapshot(args.backup, target)
            receiver.change(args.slot, source[args.source_slot], target[args.slot])
        return {"applied": True, **plan}


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(
        description="Local-only PuckOff setup. Mutations are dry runs unless --apply is present."
    )
    root.add_argument("--json", action="store_true", help="emit one privacy-safe JSON result")
    commands = root.add_subparsers(dest="command", required=True)
    commands.add_parser("devices", help="list port metadata without opening any port")
    status = commands.add_parser("status", help="read PuckOff version and status (no pairing identifiers)")
    status.add_argument("--port", required=True)
    check = commands.add_parser("check", help="validate a backup offline without printing its contents")
    check.add_argument("--file", type=Path, required=True)
    convert = commands.add_parser("convert", help="convert a protected `bonds -h` capture offline")
    convert.add_argument("--input", type=Path, required=True)
    convert.add_argument("--output", type=Path, required=True)
    export = commands.add_parser("export", help="save all four slots to a new private backup")
    export.add_argument("--port", required=True)
    export.add_argument("--kind", choices=("puckoff", "valve"), required=True)
    export.add_argument("--output", type=Path, required=True)
    import_cmd = commands.add_parser("import", help="copy one backup slot to a selected PuckOff")
    import_cmd.add_argument("--file", type=Path, required=True)
    destination = import_cmd.add_mutually_exclusive_group(required=True)
    destination.add_argument("--port")
    destination.add_argument("--target-file", type=Path, help="offline destination snapshot (planning only)")
    migrate = commands.add_parser("migrate", help="back up a source and copy one slot; never erase the source")
    migrate.add_argument("--source-port", required=True)
    migrate.add_argument("--source-kind", choices=("puckoff", "valve"), required=True)
    migrate.add_argument("--source-backup", type=Path, required=True)
    migrate.add_argument("--port", required=True, help="destination PuckOff port")
    migrate.set_defaults(target_file=None)
    clear = commands.add_parser("clear", help="back up and clear exactly one PuckOff slot")
    clear.add_argument("--port", required=True)
    for sub in (import_cmd, migrate, clear):
        sub.add_argument("--slot", type=int, choices=range(SLOTS), required=True)
        sub.add_argument("--backup", type=Path, help="new private destination backup (required to replace/clear)")
        mode = sub.add_mutually_exclusive_group()
        mode.add_argument("--apply", action="store_true", help="explicitly enable a destination write")
        mode.add_argument("--dry-run", action="store_true", help="plan only (the default)")
    for sub in (import_cmd, migrate):
        sub.add_argument("--source-slot", type=int, choices=range(SLOTS), required=True)
        sub.add_argument("--replace", action="store_true", help="permit guarded replacement of an occupied slot")
    return root


def execute(args) -> dict:
    if args.command == "devices":
        return {"devices": list_devices()}
    if args.command == "check":
        source = load_snapshot(args.file)
        return {"used_slots": [i for i, record in enumerate(source) if record]}
    if args.command == "convert":
        source = parse_valve_bonds(read_file(args.input))
        save_snapshot(args.output, source)
        return {"saved": str(args.output), "used_slots": [i for i, record in enumerate(source) if record]}
    if args.command == "export":
        source = read_source(args.port, args.kind)
        save_snapshot(args.output, source)
        return {"saved": str(args.output), "used_slots": [i for i, record in enumerate(source) if record]}
    if args.command == "import":
        return perform_import(args, load_snapshot(args.file))
    if args.command == "migrate":
        if os.path.realpath(args.port) == os.path.realpath(args.source_port):
            raise SetupError("same_device", "Source and destination must be different explicitly selected devices.")
        source = read_source(args.source_port, args.source_kind)
        return perform_import(args, source, source_backup=args.source_backup)
    with open_wire(args.port, "puckoff") as wire:
        receiver = Receiver(wire)
        if args.command == "status":
            return {"firmware": receiver.info["firmware"], **receiver.status()}
        target = receiver.export()
        changed = target[args.slot] is not None
        if changed and args.apply:
            if not args.backup:
                raise SetupError("backup_required", "Clearing a used slot requires a new --backup path.")
            save_snapshot(args.backup, target)
            receiver.change(args.slot, None, target[args.slot])
        return {"applied": args.apply, "slot": args.slot, "change": "clear" if changed else "none"}


def show(result: dict, machine: bool) -> None:
    if machine:
        print(json.dumps(result, separators=(",", ":")))
    elif not result["ok"]:
        print(f"Error ({result['error']}): {result['message']}", file=sys.stderr)
    elif "devices" in result:
        for device in result["devices"]:
            print(f"{device['port']}\t{device['kind']}")
        if not result["devices"]:
            print("No serial devices found.")
    elif "slots" in result:
        print(f"PuckOff {result['firmware']}; storage {'ready' if result['storage_ready'] else 'UNAVAILABLE'}")
        for slot in result["slots"]:
            state = "empty" if not slot["used"] else (
                "connected" if slot["connected"] else "off" if slot["powered_off"] else "searching"
            )
            print(f"Slot {slot['slot']}: {state}, reports {slot['reports']}, CRC errors {slot['crc_errors']}")
    elif "change" in result:
        mode = "Applied" if result["applied"] else "Dry run (no write)"
        print(f"{mode}: slot {result['slot']}, {result['change']}.")
    elif "saved" in result:
        print(f"Private backup saved: {result['saved']}; used slots: {result['used_slots']}")
    else:
        print(f"Valid backup; used slots: {result['used_slots']}")


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        result = {"ok": True, "command": args.command, **execute(args)}
    except SetupError as exc:
        show({"ok": False, "error": exc.code, "message": str(exc)}, args.json)
        return 1
    except KeyboardInterrupt:
        show({"ok": False, "error": "interrupted", "message": "Interrupted; inspect the destination before retrying a write."}, args.json)
        return 130
    show(result, args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
