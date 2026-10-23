# Steam Controller (2026) wireless protocol

What PuckOff needs to know to act as a receiver. Facts here were verified on
real hardware with PuckOff unless marked otherwise. Interoperability
information was cross-checked against public community research (the OpenPuck
project's `docs/PROTOCOL.md`); no code from that project is used.

## Radio

| Setting   | Value |
|-----------|-------|
| Modulation | 2 Mbit/s GFSK (nRF "BLE 2M" mode), +8 dBm |
| Packet    | `[length 8 bits][S1 3 bits][payload]`, big-endian, no whitening |
| Address   | 4 base bytes + 1 prefix byte; each byte is sent bit-reversed |
| CRC       | 16-bit, polynomial 0x11021, init 0xFFFF, includes address |
| Max payload | 96 bytes (input packets can be ~66) |

S1 carries a 2-bit packet id in bits 1–2. Bit 0 is set on polls.

## Pairing record

24 bytes, the same format Valve's receiver uses, so pairings can be moved:

    [receiver id: u32 LE][controller id: u32 LE][controller serial: 16 ASCII]

A receiver holds 4 slots.

In the official 2026 puck's read-only `bonds -h` shell output,
`ibexN_proteus_uuid` is the **receiver** ID and `ibexN_ibex_uuid` is the
**controller** ID. This ordering was confirmed against the pairing that worked
on PuckOff; the word `ibex` in the rendezvous address is not evidence for
reversing these fields. `ibexN_peer_sn` supplies the serial.

The importer preserves this ordering and requires both IDs to be nonzero in a
used record. It accepts up to 16 non-space printable ASCII serial bytes with
zero padding, and rejects truncation, control characters, and nonzero bytes
after a NUL. Empty slots are all-zero records. These are explicit importer
validation rules, not claims about every possible future Valve firmware.

See [pairing and migration](pairing.md) and the
[PuckOff management protocol](management.md). Native over-air pairing is not
implemented; the established advertisement/polling protocol alone does not
describe a verified pairing handshake.

## Finding the controller

A controller looking for its receiver listens on the shared **rendezvous**
address `"ibex"` + prefix `0x10`, channel 2.

The receiver repeatedly sends an **advertise** packet (type `E1`, 18 bytes)
there, and also on the session address once connected:

| Offset | Field |
|--------|-------|
| 0 | `E1` |
| 1–4 | receiver id (LE) |
| 5–8 | controller id (LE) |
| 9 | session channel |
| 10–12 | zero |
| 13–16 | session address base |
| 17 | session address prefix |

The controller only answers if both ids match its own pairing. It then moves to
the session address and channel. The receiver chooses the session address; it
must not be `"ibex"` and should avoid `00`/`FF` bytes. PuckOff derives a
different one per board and per controller.

Timing used by PuckOff: advertise continuously while nothing is connected,
every 200 ms on the rendezvous and every 25 ms on the session address once
connected.

## Polling for input

Every 4 ms the receiver sends a one-byte poll (`E3`) on the session address and
listens ~1.2 ms for the reply. Reply type is the first payload byte:

| Type | Meaning |
|------|---------|
| `F1` | Input |
| `F2` | Controller is powering off |
| `F3` | Status |

`F1` payloads hold records: `[length][type][value...]`. Type 6 is an input
report. A link with no reply for 300 ms is treated as lost and input is
neutralized.

## Input report (`0x45`, or `0x42` on newer controller firmware)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | report id |
| 1 | 1 | sequence |
| 2 | 4 | buttons (LE) |
| 6, 8 | 2+2 | left, right trigger (0–32767) |
| 10–17 | 4×2 | left X/Y, right X/Y (signed, +Y up) |
| 18–23 | 3×2 | left pad X, Y, force |
| 24–29 | 3×2 | right pad X, Y, force |
| 30 | 4 | IMU timestamp (µs) |
| 34–39 | 3×2 | accelerometer |
| 40–45 | 3×2 | gyroscope |

Button bits are listed in `firmware/src/input.h`.
