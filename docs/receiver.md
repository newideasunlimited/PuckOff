# Receiver reliability

The default firmware directly owns the nRF52840 RADIO peripheral. It does not
enable Bluetooth or MPSL. The raw driver's compile-time guard rejects either
stack: a future coexistence build must select a different radio implementation,
not execute the raw driver inside a timeslot callback.

## State and ownership

Four slots store bonds; there is only one generic USB HID gamepad. With no
owner, discovery rotates through eligible slots in ascending order. The first
valid input report owns the gamepad, and only that controller is polled until
release. Other controllers' status traffic cannot claim it. The main callback
also refuses reports from a different owner.

Goodbye, 300 ms without valid input, clearing a bond, or replacing any bond
field queues a neutral report before another controller can take ownership.
Status-only and malformed traffic cannot leave buttons held indefinitely.
Snapshots copy bond values under a short lock; flash writes do not block the
radio on that lock. A bond changed during an exchange causes its reply to be
discarded. Link state belongs to the radio thread, public statistics use a
spinlock, and shell-controlled watch flags are atomic.

The USB transport remains responsible for delivering the queued latest state,
including neutral, through endpoint busy/disconnect/reconnect conditions.
Submitting a neutral state is not evidence that a host received it.

## Radio and timing contract

`radio_exchange()` is synchronous in thread context and copies TX into aligned,
owned DMA storage. RX always targets a full-size owned buffer; length and CRC
are checked before copying into the caller's capacity. A failed call leaves
the caller's RX buffer unchanged. No caller pointer is retained.

The call accepts a whole-exchange budget, not just an RX timeout. Raw admission
reserves 1,100 microseconds of overhead plus the requested RX window. TX waits
at most 600 microseconds, disable waits at most 200 microseconds, and HF crystal
startup waits at most 100 milliseconds with cancellation on failure. Stuck TX
or disable operations power-cycle and clear the radio before returning an
error; absence of an RF reply alone is normal and does not reset it.

All software waits and the scheduler use Zephyr's monotonic RTC clock, scaled
to microseconds with roughly 31-microsecond resolution. The driver does not
reset or depend on the CPU DWT counter. Unsigned time subtraction handles
rollover; no zero timestamp is interpreted as a future deadline.

Each 4 ms frame has one poll, at most one budgeted retry for a healthy owner,
and at most one advertisement if time remains. Missed frames are skipped.
The budget bounds programmed waits and prevents deliberate frame overcommit;
thread preemption and diagnostic printing can still delay execution. This is
not a hard real-time guarantee or a measured end-to-end latency claim.

Operational radio errors, malformed frames, CRC errors and budget deferrals
have separate counters in `link_stats`. Hardware errors are rate-limited in
logs. Storage, USB and radio startup errors propagate out of `main()` rather
than printing a false ready message; USB setup is still attempted after a
storage error so the setup console can expose the fault.

## Deterministic host checks

Requires CMake, a C11 compiler and POSIX threads; no Zephyr SDK or hardware:

```sh
cmake -S tests/receiver -B build/receiver-tests
cmake --build build/receiver-tests
ctest --test-dir build/receiver-tests --output-on-failure
```

AddressSanitizer and UndefinedBehaviorSanitizer are enabled by default
(`-DPUCKOFF_SANITIZERS=OFF` disables them for unsupported host compilers).
Tests compile the production parser, scheduler, main callbacks and raw driver;
fake Zephyr headers supply synchronization, clocks and peripheral events.
Private source inclusion exposes a single scheduler step, not a separate
reimplementation of its policy.

Coverage includes both report IDs and IMU boundaries, 30,000 seeded
allocation-bounded parser cases, malformed trailing records, simultaneous
controller candidates, bond replacement/clear during traffic, stale input with
continued status, goodbye backoff, 32-bit microsecond/millisecond rollover,
retry/frame budgets, missed deadlines, concurrent statistics/watch access,
startup failures, RX DMA bounds, CRC failures, stale hardware events and
successful operation after timeout recovery.

## Hardware verification still required

Baseline commit `76e830c` was exercised with one 2026 Steam Controller, a
nice!nano-compatible nRF52840 UF2 board, and macOS/Brave. Host tests and a UF2
build do not establish RF timing, USB delivery, or support for other hosts.
The hardware owner should check:

- Pair/reconnect and sustained input with the actual board; check new radio
  error/deferral counters and missed input under normal and watch-enabled use.
- Hold buttons/sticks while powering off or removing the controller, then
  reconnect. Confirm host-visible neutral state and no stale input.
- Replace and clear the active bond while receiving input, using authorized
  records locally; never paste real records into a test or PR.
- Offer two paired controllers concurrently: only one must drive the single
  gamepad, and ownership must transfer only after the old input is released.
- Exercise USB unplug/replug, sleep/resume and radio interference; verify the
  USB transport actually delivers final neutral state.

No physical device access or flashing is performed by the host suite.
