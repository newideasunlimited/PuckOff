# PuckOff management protocol v1

This is a local USB CDC Zephyr-shell protocol, not WebSerial, Bluetooth,
authentication, or a network API. Connect only after an explicit user device
selection. The official Valve puck uses a different, read-only protocol; see
[pairing](pairing.md). Never send PuckOff setup commands to an official puck
or fall back to legacy mutation commands after a failed handshake.

## Transport and framing

- 115200 baud, DTR asserted, exclusive access on supported hosts.
- Before a request, drain bounded pending input, send Ctrl-U (`15` hex)
  **alone**, and require the Zephyr clear-EOS acknowledgment `ESC[J` or
  `ESC[0J`. Do not send a leading CR. Missing acknowledgment means stop
  without sending a command; a stale shell editing buffer must not be
  executed. Echo, metakeys, and VT100 support must be enabled.
- Requests are ASCII, end in CR, and begin `setup <operation> <id>`.
  `<id>` is 1-16 characters from `[A-Za-z0-9_-]`; use a fresh unpredictable
  token per request. The CLI uses 16 hexadecimal characters.
  Firmware pins both its command buffer and serial RX ring to 256 bytes, large
  enough for a complete import with two 48-digit records in one USB write.
- Each response is one ASCII line: `PUCKOFF/1 ` followed by a JSON object,
  terminated by LF (usually CRLF). The complete normalized line is at most
  1024 bytes, excluding CR/LF.
- Ordinary Zephyr ANSI formatting may surround output. Ignore unrelated
  prompts, echo, and logs. Match both `id` and `op`; a stale valid response is
  not an acknowledgment. Reject malformed JSON, duplicate fields, unexpected
  types, or an incompatible protocol. Do not print raw transcripts in errors.
- Bound every wait and buffer. The CLI uses a five-second deadline per
  phase/request, a 16 KiB total receive limit per phase, and a 4096-byte raw
  line limit to accommodate ANSI overhead before the 1024-byte frame check.
  No mutation is automatically retried after timeout or transport failure.

The command buffer is not a durable journal. Firmware disables shell command
history because import requests contain private records. The USB channel and
private backup files are not encrypted.

## Read-only commands

`setup info <id>` is the capability handshake. Example:

```text
PUCKOFF/1 {"id":"0123456789abcdef","op":"info","ok":true,"protocol":1,"product":"PuckOff Receiver","firmware":"dev","slots":4,"record_bytes":24,"capabilities":["status","export","import","clear"],"native_pairing":false}
```

`firmware:"dev"` identifies this unversioned development build; `protocol:1`
is the compatibility version. Do not interpret `native_pairing:false` as
permission to try undocumented radio or shell pairing operations.

`setup status <id>` returns:

```json
{
  "id": "0123456789abcdef",
  "op": "status",
  "ok": true,
  "storage_ready": true,
  "slots": [
    {
      "slot": 0,
      "used": true,
      "connected": true,
      "powered_off": false,
      "rssi": -47,
      "polls": 1000,
      "replies": 995,
      "reports": 990,
      "crc_errors": 2
    }
  ]
}
```

For readability, this example shows only one slot. The actual wire response
**always contains four ordered slot objects**, numbered 0 through 3, on one
line. All three state fields are JSON booleans. RSSI is an integer in
`-255..0` dBm; zero means unavailable. Counters are unsigned 32-bit integers
and may wrap. Unused slots report false state flags and zero counters/RSSI.
Link statistics and bond values are individually synchronized but are not a
cross-subsystem transaction.

`storage_ready:false` is still a successful status response, not an empty
storage claim. Stop writes and surface this state to the user. Neither info
nor status includes receiver IDs, controller IDs, USB serials, or controller
serials. These are the appropriate commands for a default read-only app.

`setup export <id>` explicitly reads private pairing data:

```text
PUCKOFF/1 {"id":"0123456789abcdef","op":"export","ok":true,"slots":[{"slot":0,"record":null},{"slot":1,"record":null},{"slot":2,"record":null},{"slot":3,"record":null}]}
```

This is one coherent four-slot snapshot. A used `record` is a 48-character
hex string encoding the 24-byte record; `null` is an empty slot. There is no
implicit backup-to-stdout in the CLI: export requires a new private file.
Export fails if storage could not be initialized/validated.

## Guarded mutations

```text
setup import <id> <slot> <new-record-hex> <expected-record-hex|empty>
setup clear <id> <slot> <expected-record-hex|empty>
```

The slot must be exactly one of `0`, `1`, `2`, `3`. An import requires a
nonempty record. The expected value is mandatory: `empty` represents the
all-zero empty record. A 48-digit all-zero guard is also accepted. There is
no wildcard, force-overwrite, or clear-all wire operation.

Firmware validates both records, serializes mutations with a writer mutex,
checks the expected value against the current record, rejects a duplicate
controller in another slot, and persists the candidate before publishing it
to readers. A mismatch returns `conflict`. Equal current/desired records are
successful no-ops without a flash write. Failed persistent writes do not
publish a new RAM bond. NVS supplies record-level durable storage; no atomic
multi-slot update is implied.

Successful mutation response:

```text
PUCKOFF/1 {"id":"0123456789abcdef","op":"import","ok":true,"slot":0}
```

`clear` uses the same shape with `op:"clear"`. This acknowledgment contains no
record. A host should re-export and verify the selected slot before reporting
completion. The expected record protects against stale state, not a malicious
USB peer or an ABA sequence returning to the exact same record.

The CLI adds safeguards above the wire protocol: mutation defaults to
dry-run, `--apply` is mandatory for a write, replacement of a different used
slot needs `--replace`, and a new private backup is mandatory before replacing
or clearing a used destination record. A direct app implementation must make
its user consent, backups, and replacement policy explicit too.

## Errors

```text
PUCKOFF/1 {"id":"0123456789abcdef","op":"import","ok":false,"error":"conflict"}
```

| Error | Meaning |
|-------|---------|
| `invalid_argument` | Invalid slot, record, token, or command arguments |
| `storage_unavailable` | Storage did not initialize or contained invalid settings |
| `conflict` | Expected value changed, a used legacy slot would be overwritten, or the controller occupies another slot |
| `storage_io` | Persistent write or internal response construction failed |

Errors never include IDs, serials, records, or the offending command.
Malformed request IDs are not echoed: their error response uses an empty
`id`. Unknown operations or excessive arguments may be rejected by Zephyr
before a handler emits a frame. Valid clients use only the documented
grammar, reject a missing/mismatched response, and never treat shell text as
success.

## Portable backup format

```json
{
  "format": "org.puckoff.bonds",
  "version": 1,
  "slots": [
    {"slot": 0, "record": null},
    {"slot": 1, "record": null},
    {"slot": 2, "record": null},
    {"slot": 3, "record": null}
  ]
}
```

The same ordered slot representation is used in firmware export. Unknown or
duplicate fields, partial tables, non-integer slot/version fields, and
malformed records are rejected. Empty slots use `null`, not zero-filled
strings. A used record contains receiver ID (u32 little-endian), controller
ID (u32 little-endian), then 16 serial bytes. Both IDs must be nonzero.
Serials accept up to 16 non-space printable ASCII bytes (`21..7e` hex),
zero-padded after the first NUL; no silent truncation or discarded trailing
bytes. The official source's `proteus_uuid` maps to the first ID and
`ibex_uuid` to the second.

Files contain no USB identity, account, board limit, or entitlement metadata.
They are intentionally sufficient to set up another DIY receiver; protect
them as private pairing data.

## Firmware API and fixtures

`bond_snapshot(int slot, struct bond *out)` returns `0`, or `-EINVAL` for an
invalid slot/null output. It copies a value under a short spinlock and never
waits on flash I/O. Callers must use their copied value, not a retained global
pointer. `bond_export()` snapshots the whole table. `bond_import()` and
`bond_clear_if()` require an expected raw record and use the same guarded
transaction as the shell. `bond_storage_ready()` reports whether settings
loaded successfully; callback validation errors are retained even if the
settings backend does not propagate them.

`bond_set()` is the legacy no-overwrite API; `bond_clear()` is the explicit,
unguarded legacy recovery operation. The unsafe `bond_get()` pointer API has
been removed. Core callers use `bond_snapshot()`; session reset/neutral-input
handling on a changed bond belongs to the receiver link layer.

`tests/pairing/fixtures/setup-info.txt` and `setup-status.txt` are complete
wire lines. Tests compare them against output from the actual C handlers.
`valve-bonds.txt` and `bonds.json` use only synthetic asymmetric IDs and
invented serials, so reversing receiver/controller byte order cannot pass.
`line-clear.json` records the coordinator-observed, non-sensitive Ctrl-U /
`ESC[J` exchange on original PuckOff firmware; official-puck cancellation
still needs its separate hardware check.
