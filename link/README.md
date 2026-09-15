# link/ — OMGP trunk L2 (`omgp_link`)

The RS-485 trunk link layer of `docs/trunk-link-layer.md`: the §4 frame codec
(`encode_frame` and `Deframer`), the host-side `Master` and node-side `Responder` engines
(§3, §7) and the `HealthTracker` (§6, §7). It carries L3 payloads as opaque bytes and never
inspects them (spec 002 FR-013), and it owns neither a wire nor a clock — both arrive as
injected interfaces, so the same code runs under the test mock, under F4's virtual rig and
on an ESP32-S3 UART.

API: `specs/002-trunk-link-layer/contracts/link-cpp.md` (types, frame codec, `Master`,
`Responder`, `HealthTracker`) and `specs/002-trunk-link-layer/contracts/byte-wire-and-clock.md`
(`Clock`, `ByteWire`, the timing model). Those two files are the contract; this README points
at them and deliberately does not restate their member listings — a second copy drifts, which
is the problem the single-source rule for `protocol/omgp-protocol.yaml` exists to avoid.

## Portable-subset constraints (plan.md "Language/Version", FR-034, CLAUDE.md rule 5)

| Constraint | Enforced by |
|---|---|
| No exceptions, no RTTI | `-fno-exceptions -fno-rtti` on the `omgp_link` target (`link/CMakeLists.txt`) and on the ESP-IDF component (`esp32-host/components/omgp_link`) — use is a compile error. `tools/check_embedded.py` also rejects `throw`/`try`/`catch`/`dynamic_cast`/`typeid` by construct |
| No dynamic allocation after init | `check_embedded.py` (quality stage) forbids `new`/`delete`/`malloc`/`free` and the allocating STL types; Catch2 cases run the engines inside `HEAP_FREE_SCOPE` (`tests/support/heap_guard.hpp`), which counts calls through a wrapped `malloc` and fails on any |
| Fixed-capacity buffers sized from the generated limits | by construction in `link_types.hpp`: `kMaxUnstuffed`/`kMaxWire` derive from `LIMIT_max_l3_payload`, `kAddrCount` sizes the per-address sequence and statistics tables and the health table. No function here takes ownership of memory — callers pass buffers in, `Status` comes back |
| No OS, no wall clock | `check_embedded.py` forbids `<chrono>`, `<thread>`, `sleep` and `system_clock`; time enters only through `Clock` (FR-027, CLAUDE.md rule 3) |
| No restated protocol or timing literals | `check_embedded.py` flags any integer literal ≥ 0x10 that the protocol YAML also defines; the values come from `build/gen/omgp_protocol.h` (FR-028, CLAUDE.md rule 4) |
| Spec traceability | `check_embedded.py` requires a spec citation — `trunk §…`, `trunk-link-layer §…`, `protocol-l3 §…` or `Spec §…`, any one of them satisfies the guard — in every file under the `--cite-dirs` basenames (default `l3 link`). Citing `trunk §` specifically is this directory's convention, not something the tool checks |
| Compiles for the target | `./pipeline.sh esp32` builds `esp32-host/components/omgp_link` with the pinned ESP-IDF image (Xtensa LX7); `./pipeline.sh` builds and tests natively with ASan/UBSan. Both are gates, not advisories |

## Timing model

Every instant is microseconds read from the injected `Clock`, and every timing value is a
generated symbol out of `build/gen/omgp_protocol.h` rather than a number written here
(FR-027/FR-028; `check_embedded.py` is what makes that mechanical): one byte occupies
`byte_time_us(bps) = 10 000 000 / bps` — 8N1 is ten bit times, start plus eight data plus
stop — evaluated at whichever of the trunk's two rates the wire is running, `TRUNK_bit_rate`
(the reference rate) or `TRUNK_bit_rate_fallback`, between which the health tracker alternates
its probes while a bus fault is declared; `ByteWire::transmit()` returns the instant of the
frame's final stop bit and received bytes carry their start-bit instant, which is what the
four timing symbols are measured against — a `Responder` whose `poll()` reaches the response
inside the turnaround window answers inside
`[request_end + TRUNK_T_turn_min_us, request_end + TRUNK_T_turn_max_us]`, while one still due
on the first `poll()` past `TRUNK_T_turn_max_us` is transmitted then, outside that bracket and
counted in `stats().late_responses`, rather than dropped or backdated (FR-014); the `Master`
fails an attempt whose response has not opened before `tx_end + TRUNK_T_resp_us` (exclusive),
retries up to `TRUNK_retries` times, and defers a transmission to `TRUNK_T_gap_us` after the
last byte it heard — a courtesy to other stations rather than a guarantee to them, because it
is bounded: past one worst-case frame plus `TRUNK_T_gap_us` from the instant the transmission
was first deferred to, the engine transmits on schedule whatever is on the wire, so a station
that will not stop talking cannot stall the host indefinitely (that bound buys exactly this,
and no more: a frame already on the wire at the deferred instant finishes and gets its full
gap, whereas a station still transmitting when the cap expires is transmitted over — see the
contract's "That push-out is bounded"); the gap rule is the `Master`'s, and the `Responder`
takes it on, cap and all, only when it answers late — inside the turnaround window trunk §3
reserves the bus for it, so it keys down at its turnaround with no gap check at all, and only
past `T_turn_max`, where that reservation is gone, does it defer on the same bounded rule;
and `TRUNK_T_poll_us` is the superframe cadence
at which the layer above calls in; nothing in
this directory ever waits, sleeps or spins on the clock — it is handed `now` (`poll(now_us)`)
and compares it against a deadline.

## What F3 and F4 need (interface note, SC-010)

**F3 (host-core scheduler)**: `Master::begin` and `Master::poll` to run a transaction;
`HealthTracker::poll_due`, `next_probe`, `on_result`, `tick` and `mark_polled` to drive the
§6/§7 lifecycle; `HealthListener`, which F3 implements to receive the notices; and `Clock`,
which F3 supplies. The trunk's `OMGPTransport` (Spec §42) is the `Master` engine over a
`ByteWire` — the host-core never sees frame bytes (FR-013a). Three obligations this layer
assumes and cannot enforce from its side — probe-only traffic during a bus fault, one
`next_probe()` call per probe issued, and the wire and the tracker moved to a new rate in the
same step — are stated in full under "What F3/F4 need" in `contracts/link-cpp.md`, and F3's
own tests are where they get pinned.

**F4 (virtual rig)**: `ByteWire` for its virtual wire — a second implementation of the
interface `tests/support/mock_wire.hpp` already implements, so the simulator exercises this
same L2 code rather than an approximation of it; `Responder` plus `RequestHandler` for the
node side of a virtual backplane; and `MockWire`'s step kinds (`Respond`, `Silence`,
`Garbage`, `CrcError`, `Duplicate`, `Babble`, `Rate` — `contracts/mock-wire.md`) as the
mapping target for scenario YAML fault steps, so trunk §7's failure modes stay configuration
instead of special-cased code paths.

Everything either needs is specified in `contracts/link-cpp.md` and
`byte-wire-and-clock.md`, with `contracts/mock-wire.md` for the step table; no interface
outside those files is required.

Three places where the contract is ahead of the code, so that a reader coding against it is
not surprised. `contracts/link-cpp.md`'s SC-010 list says `Master::begin/poll/feed`, but there
is no public `feed()` — `poll()` and `begin()` drain `ByteWire::receive()` themselves and that
is the only receive path. `HealthTracker::set_bit_rate` is specified but not yet declared in
`health.hpp` (tasks.md T041/T043 carry it as a red-first slice, as the header comment records).
And of the seven step kinds above only `Respond`, `Silence`, `CrcError` and `Duplicate` are
implemented in `tests/support/mock_wire.cpp` today: `Garbage`, `Babble` and `Rate` are declared
so scripts can name them, but the switch raises a "not implemented until T030" test fault
instead of producing the behaviour (`tests/support/mock_wire.hpp:6-10`,
`tests/support/mock_wire.cpp` `case Kind::Garbage:`). F4's mapping for garbage, babble and
rate-change waits on T030, or on F4 implementing them in its own `ByteWire`.

## Files

- `link_types.hpp` — `Status`, `FrameFields`/`FrameView`, `Discard`/`DeframerStats`, the
  `kMaxUnstuffed`/`kMaxWire`/`kAddrCount` bounds, `byte_time_us`, `HealthState`/`Notice`,
  `AddrStats`/`BusStats`; `link_status.cpp` — `status_name`
- `frame.{hpp,cpp}` — §4 codec: `encode_frame`, `Deframer` (flag framing, byte stuffing,
  CRC check, per-reason discard counters); `crc16.hpp` — CRC-16/CCITT-FALSE over the
  unstuffed bytes
- `clock.hpp`, `byte_wire.hpp` — the two injected interfaces (contract:
  `byte-wire-and-clock.md`)
- `master.{hpp,cpp}` — host-side transaction engine: sequence numbers, the `T_resp` window,
  retries, per-address and bus statistics (§3, §7)
- `responder.{hpp,cpp}` — node-side engine: acceptance screen, turnaround scheduling and the
  single-frame replay buffer that makes a §7 retry idempotent (§3, §7)
- `health.{hpp,cpp}` — `HealthTracker`: the ENROLLED/SUSPECT/OFFLINE lifecycle (§6) and the
  bus-fault declare, alternating-rate re-probe and clear rules (§7)

Tests: `tests/unit/test_link_*.cpp` and `tests/property/test_link_*.cpp` against `MockWire` +
`FakeClock`; the independent Python reference for the frame codec is `tools/refimpl/
omgp_link.py`, and the golden frame vectors under `tests/vectors/` are generated from it and
are immutable.
