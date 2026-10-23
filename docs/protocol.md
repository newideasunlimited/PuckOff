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

A receiver stores 4 bonds. The generic HID firmware exposes **one gamepad**,
not four simultaneous controllers. It discovers slots in ascending cyclic
order; the first valid input selects an owner until input loss, goodbye, or
bond replacement. Other stored controllers cannot overwrite that owner's input.

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

Implemented discovery timing: each eligible slot is polled in cyclic order
while no input owner exists. Session beacons are due every 25 ms, rendezvous
advertisements every 20 ms while idle or 200 ms while connected. Only the
selected controller is serviced while connected. At most one advertisement is
sent in the time remaining after a poll; due times are not guarantees of
airtime when retries or scheduling delays consume the frame.

## Polling for input

The receiver schedules a one-byte poll (`E3`) every 4 ms and listens for up to
1.2 ms including RX ramp-up. A connected controller gets at most one immediate
retry, only if its worst-case exchange fits before the next poll deadline.
Late frames are skipped, not accumulated. Reply type is the first payload byte:

| Type | Meaning |
|------|---------|
| `F1` | Input |
| `F2` | Controller is powering off |
| `F3` | Status |

`F1` payloads hold records: `[length][type][value...]`. Type 6 is an input
report. Payloads over 96 bytes, truncated/zero-length records, unknown packet
types and incomplete recognized input reports do not refresh input. Unknown
well-formed record/report types are skipped. A complete packet is validated
before any decoded state is published.

No **valid input** for 300 ms releases and neutralizes the gamepad, even if
`F3` status traffic continues. `F2` releases it immediately and suppresses that
slot for 2.5 seconds. Changing or clearing any bond field releases the old
session and resets its counters/timers; receiver-id changes also rederive the
session address. All intervals use unsigned elapsed time or short signed
deadline differences, including across timer wrap.

These are implemented receiver policies, not new hardware-compatibility
claims. See [receiver reliability](receiver.md) for the test boundary.

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

## USB reports

The USB-facing formats are separate from these radio packets. See
[USB modes](usb-modes.md) for the generic and opt-in wired Switch report layouts,
public protocol sources, and host/game compatibility limits. Changing USB mode
does not change the radio pairing format.
