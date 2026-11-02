# Experimental MPSL RF-to-BLE bridge

`CONFIG_PUCKOFF_TRANSPORT_BLE_RF` wires real proprietary controller transactions
through a new MPSL backend, the receiver's packet decoder and controller-ownership
policy, and the encrypted BLE HID service. It does not link the raw exclusive-owner
driver, USB gamepad transport, diagnostic main, or manual button commands.
**This is compile/model-tested experimental firmware, not demonstrated hardware
coexistence or certified Bluetooth/game-controller support.**

The default USB/raw-RF image and the [BLE-only diagnostic](bluetooth.md) remain
separate choices. Router USB can power the bridge; the nRF52840 has no Wi-Fi and
does not acquire network connectivity from a router. The intended BLE host must
be nearby and must actually accept the HID report map. No Safari, iOS, tvOS, or
Apple Game Controller compatibility is claimed.

## Build and use

With the NCS 3.4.1 / Zephyr 4.4.2 environment active:

```sh
west build -b promicro_nrf52840/nrf52840/uf2 \
  -d build/ble-rf firmware -- \
  -DCONF_FILE=ble-diagnostic.conf \
  -DEXTRA_CONF_FILE=ble-rf.conf \
  -DEXTRA_DTC_OVERLAY_FILE=ble-rf.overlay
```

The first configuration supplies the common BLE/CDC/settings options; the extra
configuration explicitly switches to the real RF entry point and the name
`PuckOff BLE RF`. The overlay reserves EGU1/SWI1 for completion handling. Omitting
that reservation is a compile error, not permission to share another driver's IRQ.
The application-only artifact is `build/ble-rf/firmware/zephyr/zephyr.uf2`.
The board's application/settings/UF2 partition boundaries are unchanged.
An S140-compatible bootloader layout does not mean a legacy SoftDevice runs
beside MPSL; the linked SDC and MPSL are application libraries.

Use the receiver's normal controller-bond provisioning workflow. Controller
records and the BLE host bond are separate settings subtrees. No automatic
controller pairing or remote-management GATT service is added by this transport.
The existing decoder maps actual input, keeps the first active controller as
owner, and sends neutral before releasing a lost owner. A competing controller
does not interleave reports. BLE retains only the newest report, with an 8 ms
send opportunity, not an unbounded queue of stale controller samples.

The CDC shell includes:

```text
ble status
ble forget_host
rf_slots
bootloader
```

The first command exposes host/encryption/subscription/suspend state and
notification submission counters. `forget_host` requires disconnection and
removes only the BLE host bond. `rf_slots` reports requests, grants, denials,
admission deferrals, hardware cutoffs, forced resets, late completions, invalid
signals, and maximum observed call duration. `bootloader` retains the receiver's
software UF2 reentry (`GPREGRET=0x57`, cold reboot). There are no `ble buttons`
or `ble neutral` commands in this RF image. USB enumeration and DTR are not
prerequisites for RF/BLE startup.

Pairing is encrypted Just Works, without authenticated MITM protection,
an allowlist, or a physical pairing-button policy. A successful connection,
encrypted GATT read, or submitted notification is not proof that a host consumed
game-controller input.

## Ownership and resource allocation

The raw driver rejects Bluetooth/MPSL at compile time and is excluded from this
image. Only the new backend calls MPSL's public timeslot API. These allocations
were checked against the pinned SDK, not assumed to be generally free:

| Resource | Ownership |
| --- | --- |
| RADIO | MPSL/SDC outside grants; proprietary configuration only inside a grant |
| TIMER0 | MPSL; grant-local 1 MHz timer, CC0 cutoff, CC1 RX timeout, CC2 end guard, CC3 capture |
| PPI19 | MPSL explicitly lends it during a grant; CC0 triggers RADIO.DISABLE |
| PPI17..31 | SDC reserved; no application use except the documented grant-local PPI19 |
| RTC0 and CLOCK | MPSL-owned; never reset or configured by the backend |
| RTC1 | Zephyr system clock; counter read only for absolute caller deadlines |
| SWI1/EGU1 | Dedicated normal-priority completion IRQ; DT node disabled, nrfx/errata users rejected |
| SWI5 | SDK MPSL low-priority IRQ; must not collide with completion |

The backend asserts the nRF52840, SoftDevice Controller, one available timeslot
session, lock-free 32-bit atomics, full-width 24-bit RTC, and 32768 Hz tick/cycle
timebase. It does not replace MPSL's priority-zero RADIO/TIMER0 handlers. Every
granted-window cleanup clears the owned PPI endpoints/fork, timer interrupts and
events, RADIO interrupts/shortcuts/events, and packet pointer before returning END.

## Transaction and timing contract

The receiver's existing eight-argument `radio_exchange()` API is preserved.
`budget_us` is the remaining **whole-call** budget (maximum 4000 us), not an
extra allowance added after waiting for a grant. TX includes the length and S1
bytes, payloads are limited to 96 bytes, and RX is capped at 1200 us. Caller
buffers are never held by callbacks: one aligned static job owns copied TX/RX
storage. Only a validated `RADIO_RX_OK` result copies into the caller's RX buffer.

Thread-context admission never waits for the serialization or MPSL/SDC lock.
One normal-priority EARLIEST request asks for a 2000 us XTAL-guaranteed window;
there are no extensions or priority escalation. Admission subtracts the complete
grant and 250 us call margin before offering any grant-wait timeout. Insufficient
budget, blocked/cancelled grants, and a busy SDK lock are deferrals, not packets.

At START, read-only RTC1 elapsed time shortens the window cutoff against the
absolute caller budget. A late or abandoned grant ends without starting RF.
The normal RADIO cutoff is no later than 1800 us after grant start. TIMER0/PPI19
issues DISABLE in hardware even if software is delayed. A separate guard 100 us
later forces a RADIO power-cycle **inside the grant only** if DISABLE is stuck,
then returns END. No thread-context reset touches stack-owned radio/clock state.
The remaining grant/caller margins allow cleanup and LF/HF clock quantization
and drift; physical worst-case timing still needs measurement.

The priority-zero state machine is TX -> DISABLED -> optional RX -> DISABLED ->
END. It performs no Zephyr, BLE, logging, allocation, waiting, packet decoding,
or semaphore calls. Completion is an atomic publication plus SWI1; that normal
IRQ gives the thread's semaphore. BLOCKED/CANCELLED are handled without touching
RADIO or returning a high-priority END action.

At the maximum payload, 2 Mbit/s TX is budgeted at **462 us**, including the
40 us fast ramp, 8-bit preamble, five-byte address, length/S1, and CRC. With
1200 us RX and 30 us switching margin, TX must launch by 108 us into a normal
1800 us window. Admission is rechecked after hardware setup and before RX.
RX timeout includes ramp-up. CRC failures, malformed lengths, timeout and missing
TX END cannot become successful input.

A thread timeout marks the copied job abandoned and quarantines it until MPSL
retires the request. A subsequent call cannot overwrite pending staging; it
returns busy. A late START ends without RF. There is no speculative cancellation
API, unsafe buffer reuse, or forced session/radio reset outside a grant.

**The hardware activity deadline is separate from thread-return latency.** Zephyr
or higher-priority scheduling can delay the calling thread after an operation
has quiesced. The implementation checks elapsed time before accepting/copying a
completion, reports detected lateness as `-ETIMEDOUT`, and records `max_call_us`;
it does not establish a universal hard 4 ms CPU-return guarantee under arbitrary
preemption. This limitation must be measured and resolved at system-scheduling
level before claiming a hard real-time transport.

## Scheduling and rate limits

The opt-in link branch admits **at most one operation per nominal 4 ms frame**,
including discovery and session keepalive, not just polls. Due maintenance
replaces a poll; otherwise the selected controller is polled once. There are no
same-frame retries or catch-up bursts. The raw-USB branch keeps its original
bounded retry/scheduling behavior.

BLE connection events are capped at a configured 1500 us and event extension is
disabled. These are controller scheduling settings, not a host latency promise.
BLE traffic, flash activity, denied grants, maintenance, multiple bonded slots,
and the roughly 31 us RTC resolution reduce effective per-controller polling.
There is **no guaranteed 250 Hz RF or BLE report rate**.

## Software validation

```sh
cmake -S firmware/tests/ble -B build/ble-tests \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/ble-tests
ctest --test-dir build/ble-tests --output-on-failure
```

The five executables cover latest-report gates; pure timing/airtime/state policy;
the **production** MPSL callback/exchange implementation behind fake MMIO/SDK/RTOS
boundaries; real `link.c` maintenance fairness/denial/wrap/no-retry behavior; and
real `main.c` BLE dispatch, sticky ownership, neutral release and startup errors.
The callback model checks priority-zero kernel-call exclusion, grant denial and
cancellation, late grants, shortened deadlines, TX-only, CRC/size errors, stuck
disable, forced cleanup, missing callbacks, job quarantine, and unchanged failed
RX output. Policy tests sweep all supported TX lengths and elapsed budgets.
The fakes are test boundaries, not a controller-input source or a physical timing
simulator. Real NCS board builds separately check SDK API and resource assertions.
The default receiver's existing host tests must also continue passing.

Backend commit `56697a9`, integrated with merged core/diagnostic prerequisites
at `cfa2d0d`, passed all five BLE tests and four receiver tests under ASan/UBSan.
The grant-event cleanup follow-up additionally clears READY/ADDRESS/RSSIEND before
release; its production callback model and RF image were checked again.
Clean board builds using **only** the modules pinned by `west.yml` produced:

| Image | Flash | RAM | Local build directory |
| --- | ---: | ---: | --- |
| Default USB/raw RF | 108360 B | 27136 B | `build/locked-usb-default` |
| BLE-only diagnostic | 242192 B | 45692 B | `build/locked-ble-diagnostic` |
| Experimental RF-to-BLE | 252828 B | 49340 B | `build/locked-ble-rf` |

Each generated module list was checked against the manifest; the installed SDK
repositories matched its commits without tracked modifications. Each image's
`firmware/zephyr/zephyr.uf2` passed the existing UF2 validator. ELF checks confirmed
the expected transport/radio symbols and software bootloader recovery, with manual
button commands present only in the diagnostic. These results precede subsequent
USB changes and do not constitute hardware or host-compatibility results.

Before hardware claims, the coordinator must establish software recovery,
advertising/security/subscription, actual controller packet reception, host HID
acceptance, repeated connect/disconnect and bond persistence, neutral on RF loss,
and maximum timing/denial/cutoff behavior under BLE and settings/flash contention.
Equipment needed: the specified nRF52840 board/bootloader layout, stable USB
power, a provisioned Steam Controller, and a BLE central that can inspect
encrypted HID traffic. RF/logic capture equipment is needed to independently
measure activity relative to grant deadlines. This worker does not flash,
read live controller bonds, or access USB/serial devices.

The SDK source/API references in [the diagnostic document](bluetooth.md) also
apply. SDC's additional reservation mask is in
[`sdc_soc.h`](https://github.com/nrfconnect/sdk-nrfxlib/blob/bb715183a57ac501907b63df2079c3fbcd067641/softdevice_controller/include/sdc_soc.h);
RTC1 ownership is in
[`nrf_rtc_timer.c`](https://github.com/nrfconnect/sdk-zephyr/blob/33fa6a7aac6a4401d16a67cb9f27a3483fa02dd6/drivers/timer/nrf_rtc_timer.c).
Implementation and tests are independently written Apache-2.0 code.
