# Pairing and moving between receivers

PuckOff's local Python tool copies an existing pairing without making you type
controller IDs. It works independently of Safari, Brave, or any other browser.
There is no account, network service, board quota, or paid-board distinction.
Each receiver has four storage slots; this does not change the current
firmware's USB gamepad or simultaneous-controller capabilities.

**This is a copy, not a new over-air pairing.** Start with a controller already
paired to an official 2026 Steam Controller Puck, a configured PuckOff, or a
private backup. Nothing here resets, reparents, or erases the official puck.
After copying, disconnect receivers with the same pairing that you are not
using. Automatic handoff between simultaneously powered copies is not promised.

## Install the local tool

Use Python 3.10 or newer on macOS or Linux. Live serial access has one small
dependency, pyserial; offline file checks, conversion, and planning use only
the Python standard library. Windows serial behavior has not been verified.

```sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r tools/requirements.txt
python tools/puckoff_setup.py devices
```

`devices` reads port metadata but does not open any port. It labels only these
exact known descriptor combinations as supported:

| Kind | VID:PID | Manufacturer | Product |
|------|---------|--------------|---------|
| `puckoff` | `1209:0001` | `PuckOff` | `PuckOff Receiver` |
| `valve` | `28de:1304` | `Valve Software` | `Steam Controller Puck` |

The PuckOff VID/PID is a development identity, not a commercial allocation.
Other Valve dongles, unknown boards, and missing/mismatched metadata are
refused before opening. There is no "first available device" or identity-check
bypass. USB descriptors are compatibility checks, not cryptographic
authentication; use devices you trust.

Flash firmware containing `setup info` before using a PuckOff destination.
Close serial terminals and other companion apps first. Connections are
exclusive on macOS/Linux, use 115200 baud and DTR, and do not reset the board.
Leave the Zephyr shell's echo, metakeys, and VT100 support enabled. The tool
clears any unfinished input with Ctrl-U and waits for a clear-EOS reply before
sending a command terminator. If that fails, it sends no command and stops.
Do not work around this with a leading Enter: it could execute unfinished
input left by another terminal. Turn `watch off` manually before setup if
continuous logs prevent the console from settling.

The port names below are placeholders. Substitute the exact paths from
`devices`; on macOS they usually start with `/dev/cu.usbmodem`.

## Export once, set up as many boards as you need

Keep pairing files outside the repository, in a private directory:

```sh
mkdir -p "$HOME/.puckoff"
chmod 700 "$HOME/.puckoff"
python tools/puckoff_setup.py export \
  --kind valve --port /dev/cu.usbmodemSOURCE \
  --output "$HOME/.puckoff/controller.json"
```

For another PuckOff source, use `--kind puckoff`. Export saves all four slots
and prints only their occupied slot numbers, not IDs or serials. If multiple
slots are occupied, select the intended source slot deliberately; the tool
does not guess which controller you meant.

Plan an import into an explicitly selected destination slot:

```sh
python tools/puckoff_setup.py import \
  --file "$HOME/.puckoff/controller.json" --source-slot 0 \
  --port /dev/cu.usbmodemDESTINATION --slot 0
```

This is a **dry run**: it reads the destination but does not change it or create
backups. Add `--apply` to perform the planned operation:

```sh
python tools/puckoff_setup.py import \
  --file "$HOME/.puckoff/controller.json" --source-slot 0 \
  --port /dev/cu.usbmodemDESTINATION --slot 0 --apply
python tools/puckoff_setup.py status --port /dev/cu.usbmodemDESTINATION
```

The tool checks the firmware contract before exporting or modifying a PuckOff,
compares the destination with the record it just read, and verifies the saved
record by reading it back. An identical import is a no-op, avoiding flash wear.
A controller already in another destination slot is rejected, not duplicated
or moved automatically. Repeat with the same private source file and a
different explicitly selected board to configure additional receivers.

Unplug the source receiver before testing the copied pairing. A controller
already connected to the source may need to be turned off and on to find the
new receiver.

## Copy directly between two connected devices

`migrate` combines source export, private backup, and one-slot import. It still
defaults to a dry run, and **never clears the source**.

```sh
python tools/puckoff_setup.py migrate \
  --source-kind valve --source-port /dev/cu.usbmodemSOURCE --source-slot 0 \
  --source-backup "$HOME/.puckoff/source-before-copy.json" \
  --port /dev/cu.usbmodemDESTINATION --slot 0 --apply
```

Use `--source-kind puckoff` for a PuckOff-to-PuckOff copy. Source and
destination must differ. A source backup is saved before any destination
write. The official-puck path uses only the documented read-only
`bonds -h` command after line cancellation; there are no probing, pairing,
reset, or undocumented-command fallbacks.

## Replace or clear deliberately

Replacing a different occupied record requires both `--replace` and a **new**
destination backup path in addition to `--apply`:

```sh
python tools/puckoff_setup.py import \
  --file "$HOME/.puckoff/controller.json" --source-slot 0 \
  --port /dev/cu.usbmodemDESTINATION --slot 0 \
  --replace --backup "$HOME/.puckoff/destination-before-replace.json" --apply
```

The same `--replace --backup ...` options apply to `migrate`. Clearing is
limited to one explicit slot and also requires a backup before deleting a
used record:

```sh
python tools/puckoff_setup.py clear \
  --port /dev/cu.usbmodemDESTINATION --slot 0 \
  --backup "$HOME/.puckoff/destination-before-clear.json" --apply
```

There is no clear-all or factory-reset operation. Backups are new files with
mode `0600`, written completely and synchronized before publication. Existing
files and symlinks are never overwritten. A backup failure prevents the
destination write. Files are **not encrypted**: keep them out of source
control, terminal recordings, cloud logs, issue attachments, and PRs.

If the destination changes after inspection, firmware rejects the stale
expected record as `conflict`. If a write loses its response, it may already
have saved: inspect the destination before retrying. The tool never retries a
mutation automatically. `storage_unavailable` or `storage_io` is an error,
not an empty receiver or a successful import; do not erase storage to hide it.

## Offline files and automation

```sh
python tools/puckoff_setup.py check --file "$HOME/.puckoff/controller.json"
python tools/puckoff_setup.py convert \
  --input "$HOME/.puckoff/private-bonds-capture.txt" \
  --output "$HOME/.puckoff/converted.json"
python tools/puckoff_setup.py --json import \
  --file tests/pairing/fixtures/bonds.json --source-slot 0 \
  --target-file tests/pairing/fixtures/empty.json --slot 0 --dry-run
```

`convert` accepts a complete private `bonds -h` capture, including its command
echo, all twelve named fields, and the final `dongle~$` prompt. It rejects
unknown, duplicated, malformed, or partial output rather than guessing.
CRLF and ordinary Zephyr ANSI formatting are accepted. The two identifier
fields have different roles: **proteus is the receiver; ibex is the
controller**. Short serials are zero-padded, never truncated.

`--target-file` is strictly offline planning and cannot be combined with
`--apply`. `--json`, placed before the command name, emits one privacy-safe
JSON result and a nonzero exit code on failure. Syntax errors use argparse's
standard stderr/exit-code-2 behavior. No command accepts pairing IDs on the
process command line. No serial transcript, record bytes, or controller
serial is included in tool results or errors. The private backup is the
explicit exception: it intentionally contains the complete records.

The backup schema and wire protocol are specified in
[management protocol v1](management.md). Apps may implement that protocol
directly; a browser or a Python subprocess is not required.

## Manual recovery commands

The original `pair show`, `pair set`, `pair clear`, `status`, `watch`, and
`bootloader` commands remain. `pair show` intentionally reveals private data,
so do not capture or share its output. `pair set` now validates slot numbers,
32-bit hex IDs, and the serial without truncation, and refuses to overwrite a
different occupied record. Interactive `pair clear <slot>` remains an explicit
destructive recovery command; unlike the setup tool, it does not create a
backup or require a compare-and-swap guard.

## Verification and remaining hardware checks

Run all synthetic pairing tests, including the real firmware C code against
fake settings, locks, and shell output:

```sh
python -m unittest discover -s tests/pairing -p 'test_*.py' -v
```

A host C compiler is required. Tests exercise flash failure rollback,
reloading persisted records, strict parsing, occupied/duplicate-slot guards,
concurrent readers during blocked flash writes, private backup publication,
framing bounds, partial/stale responses, dry runs, and failed confirmations.
Firmware-generated info/status lines are compared byte-for-byte with the
shared fixtures in `tests/pairing/fixtures/`.

The separate **Pairing** workflow in `.github/workflows/pairing.yml` runs this
suite for firmware, pairing-tool/dependency, fixture/test, and workflow edits.
It uses a host C compiler and Python, with no SDK, serial dependency
installation, or device access. It does not change the receiver/USB build
workflow.

All bond/serial fixtures are invented. `line-clear.json` is a non-sensitive
protocol observation: the coordinator confirmed that Ctrl-U on the original
PuckOff firmware returns exactly `ESC[J`, without a preceding CR. The official
puck's descriptor tuple, field grammar, and receiver/controller ordering were
also confirmed by the coordinator from an earlier successful pairing.
No competitor implementation code is used.

Only the coordinator should perform the following physical checks for this
change; host tests and a board build do not substitute for them:

1. Flash the integrated UF2 on a spare nRF52840 UF2 board, leaving existing
   pairings intact. Confirm exact port classification, Ctrl-U acknowledgment,
   version/status framing, and `storage_ready:true`. Reject an unrelated port
   without sending anything to it.
2. From an existing private PuckOff backup/source, dry-run an empty-slot
   import and confirm no slot changed. Apply it, verify connection/input, then
   unplug/replug the destination and verify persistence and reconnection.
3. Confirm an occupied import is refused without `--replace`, and replacement
   or clearing is refused without a new backup. On the spare board, perform
   an authorized backed-up replacement/clear and verify that other slots stay
   unchanged. Verify neutral input and reconnection with the core reliability
   changes integrated.
4. When the official puck is available, confirm its Ctrl-U clear-EOS reply
   before allowing `bonds -h`. Export into a private file without recording
   the transcript, copy an explicitly chosen slot, disconnect the source,
   and verify the controller works on the destination. Leave the source
   pairing untouched. Fail closed if cancellation or parsing differs.
5. Restore any deliberately changed spare-board slot from its private backup.
   Do not factory-reset either receiver, inject real bonds into tests, or
   publish live IDs, serials, or pairing files.

Native over-air pairing is not shipped and is advertised as unsupported.
Implementing it requires a separately gated, independently written state
machine based on a verified controller/puck handshake, explicit user pairing
intent, timeout/cancel behavior, persistence only after successful completion,
and real-hardware positive and rejection-path checks. The working importer
does not depend on that future work.
