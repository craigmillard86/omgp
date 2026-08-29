# Research: Trunk Link Layer (feature 002)

Phase 0 of `/speckit.plan`. Every unknown in the Technical Context is resolved here as a
decision with rationale and rejected alternatives. Facts about the repository were read
from the tree on 2026-08-29; facts about the trunk protocol come from
`docs/trunk-link-layer.md` (§3, §4, §7, §9) and `protocol/omgp-protocol.yaml`
(`link_trunk`). Where a decision rests on an assumption rather than a document, it says so.

## R-01 — Where the `Clock` and byte-wire interfaces live: `link/`

- **Decision**: two pure-interface headers in `link/`: `link/clock.hpp` (`omgp::Clock`,
  one method `now_us()` returning monotonic microseconds as `uint64_t`) and
  `link/byte_wire.hpp` (`omgp::link::ByteWire`: `transmit(bytes, n, now) → tx_end_us`,
  `receive(out_byte, out_start_us) → bool`, `bit_rate()`, `set_bit_rate(bps)`). Virtual
  interfaces with no RTTI requirement; `-fno-rtti` builds virtual dispatch fine.
- **Rationale**: nothing in the repo defines a clock yet (`core/`, `transport/` are
  empty). `core/` (F3) will depend on `link/` anyway — the host runs the L2 master — so
  `link/` is the lowest layer that can own the interfaces without creating a `link/ →
  core/` dependency. Spec §42 explicitly allows "an equivalent C/C++ interface
  appropriate to resource constraints"; a one-method interface is that. The CLAUDE.md
  invariant "core depends only on OMGPTransport and Clock" is preserved: `core/` includes
  `link/clock.hpp`, and the trunk implementation of `OMGPTransport` (F3/F4) wraps the
  master engine.
- **Alternatives considered**: a new top-level `platform/` for shared interfaces — adds a
  directory, the risk-score regex and CLAUDE.md layout edits (T3) for two headers;
  template policy parameters instead of virtual interfaces — faster in theory, but the
  scripted transport and the simulator need runtime substitution and the engines would
  become header-only templates that Mull and the ESP-IDF component build handle worse;
  `std::function` callbacks — forbidden on the embedded path (allocating STL type).

## R-02 — Frame buffer sizing: 142 bytes, derived from generated symbols

- **Decision**: `kMaxUnstuffed = 4 + LIMIT_max_l3_payload + 2 = 70`,
  `kMaxWire = 2 + 2 * kMaxUnstuffed = 142`, both `constexpr` from the generated header;
  every frame buffer (encoder output, deframer accumulator, replay buffer, mock wire
  queues) is a fixed array of that size. Worst-case frame time at the reference rate is
  142 × 10 / 1 000 000 s = 1.42 ms — §4's "≤ ~1.4 ms". The spec's SC-008 and story 1
  scenario 4 said 140 B / 1.4 ms; corrected in the spec during this plan.
- **Rationale**: the plan input asks for buffers "sized from limits.max_l3_payload plus
  worst-case stuffing"; worst case is every byte of dst..crc escaped (payload of all 0x7E
  or 0x7D plus a header/CRC that happen to need escaping), which doubles 70 to 140, plus
  the two FLAGs.
- **Alternatives considered**: sizing to 70 + a stuffing allowance (e.g. +16) with a
  "too long" discard — rejects legal worst-case frames; a dynamic buffer — forbidden.

## R-03 — Deframer: byte-at-a-time state machine, unstuffed accumulator, abort on first bad escape

- **Decision**: `Deframer` with states `Hunting` (discarding until FLAG), `InFrame`,
  `Escaped`; it unstuffs into a 70-byte accumulator as bytes arrive. On FLAG: 0
  accumulated bytes → ignore (empty frame / shared delimiter); < 6 → `BadLength`;
  `len != accumulated − 6` → `BadLength`; CRC mismatch → `BadCrc`; otherwise deliver a
  `FrameView` (fields + payload pointer into the accumulator, valid until the next byte).
  An escape followed by anything but 0x5E/0x5D → `BadEscape`, abort, return to `Hunting`
  (ruling Q1 — the "≥ 8" clause is satisfied a fortiori; the accumulator can never hold
  eight violations because the first one ends the frame). A 71st unstuffed byte →
  `TooLong`, abort. Every discard increments a counter by reason; nothing is reported
  otherwise (§4: "discarded silently").
- **Rationale**: FR-002/FR-003/FR-004 verbatim; O(1) memory; no over-read (the parser
  never indexes input beyond the byte it was given). Resynchronisation is purely "next
  FLAG", which is what makes the torture corpus tractable to specify: the expected
  delivered frames are exactly the intact ones.
- **Alternatives considered**: buffering the stuffed frame and unstuffing on FLAG — needs
  the 142-byte buffer *and* a second pass; a length-driven parser (read `len`, then wait
  for that many bytes) — cannot resync on a corrupted `len`, exactly the failure §4's
  framing exists to avoid.

## R-04 — Master engine: explicit state machine, `poll(now)` driven, time from the wire

- **Decision**: `Master` with states `Idle`, `Transmitting` (until the wire's reported
  `tx_end_us`), `AwaitResponse{attempt}` (until a valid response or `tx_end + T_resp`),
  `Gap` (until `last_activity + T_gap`). API: `begin(dst, payload) → Status`;
  `feed(byte, start_us)` for every received byte (the wire hands bytes to the engine's
  deframer); `poll(now) → Event` where `Event ∈ {None, Answered{payload}, Failed{reason}}`;
  `busy()`; `stats(addr)`; `set_bit_rate()` pass-through. Start-bit instants: the wire
  reports each received byte with the instant of its **start bit**; "final stop bit" of
  a frame = last byte start + 10 bit-times at the current rate (FR-012). The response
  window is `[tx_end_us, tx_end_us + T_resp)` — exclusive at the end (spec edge case).
- **Rationale**: a `poll(now)`-driven machine has no waits and no timers of its own
  (CLAUDE.md rule 3); tests advance a `FakeClock` and call `poll`; F3's scheduler does the
  same from the superframe loop. Modelling serialisation time from byte count and the
  generated bit rate is what makes the two bit rates testable without a UART.
- **Alternatives considered**: callback-driven completion — pushes control flow into the
  wire implementation and complicates the simulator; a blocking `transact()` — would
  need real waiting (forbidden) or a co-routine.

## R-05 — Responder engine: application hook as a virtual interface, turnaround as a parameter

- **Decision**: `Responder(addr, RequestHandler&, turnaround_us = T_turn_min)`;
  `feed(byte, start_us)`, `poll(now)`. On an intact frame addressed to it whose (seq,
  retry) is not a replay of the buffered sequence it invokes
  `handler.handle(request, out_response) → response length` exactly once and schedules the
  response frame at `request_end + turnaround_us` (constructor-clamped to
  `[T_turn_min, T_turn_max]`); on a retry of the buffered sequence it retransmits the
  buffered 142-byte frame unchanged. Frames for other addresses and corrupt frames are
  ignored (and counted). Replay buffer = one `{valid, seq, len, bytes[142]}`.
- **Rationale**: FR-014..FR-017. The turnaround parameter lets tests assert both bounds
  and lets F4's virtual backplane model a slow node; clamping keeps every node built on
  this engine conformant by construction.
- **Alternatives considered**: keying the replay buffer by (src, seq) with N entries —
  §7 says "single-frame replay buffer per node" and strict master poll means one
  outstanding request at a time; more entries buy nothing.

## R-06 — Health tracker and bus fault: a fixed 16-entry table, listener interface for notifications

- **Decision**: `HealthTracker(Clock&, HealthListener&)` with a fixed array of 16 records
  (trunk addresses 0x00–0x0F per §5; entry 0 is the host and never enrolled).
  `on_result(addr, ok, now)`, `state(addr)`, `poll_due(addr, now)` (ENROLLED: always;
  SUSPECT: `now − last_poll ≥ 10 × T_poll`; OFFLINE/UNENROLLED: only via the enrolment
  rotation), `next_probe(now) → {addr, bit_rate}` (rotation over UNENROLLED and OFFLINE
  addresses; while BUS_FAULT the rate alternates reference/fallback per probe — ruling Q3),
  `bus_fault()`. Transitions and BUS_FAULT declare/clear call
  `HealthListener::on_event(kind, addr)` (kinds: ENROLLED, SUSPECT, OFFLINE, RECOVERED,
  BUS_FAULT, BUS_RECOVERED, ALERT); the listener is a virtual interface implemented by F3
  and by a recording listener in tests. BUS_FAULT rule (ruling Q2): after any result, if
  ≥ 1 enrolled node and every enrolled node (ENROLLED/SUSPECT/OFFLINE) is SUSPECT or
  OFFLINE → declare once; the first valid response from any node clears it once and pins
  the bit rate that produced it.
- **Rationale**: FR-018..FR-026 with no allocation; listener beats a queue because a
  bounded queue can overflow (then a notification is lost silently — the exact failure
  class this repo just fixed in the metrics workflow). `10 × T_poll` is a clock-based
  reading of "once per 10 superframes" (spec assumption); F3 may pass a superframe count
  instead, keeping the threshold a single symbol.
- **Alternatives considered**: notification ring buffer polled by F3 — overflow semantics;
  per-node dynamic map — forbidden.

## R-07 — Scripted transport (`MockWire`): step table in `tests/support/`, deterministic PRNG

- **Decision**: `tests/support/mock_wire.{hpp,cpp}` implements `ByteWire` plus a
  `FakeClock`. A `Step { uint8_t node; Kind kind; uint32_t delay_us; uint16_t count;
  uint32_t seed; }` table per node (`Kind ∈ {Respond, Silence, Garbage, CrcError,
  Duplicate, Babble, Rate}`); on every transmitted frame the mock deframes it (the
  test-side parser is the same `Deframer`), looks up the addressed node's next step,
  and schedules RX bytes at computed start-bit instants (`tx_end + delay`, one byte every
  10 bit-times). `advance_to(t)` releases scheduled bytes whose start instant ≤ t to the
  engine under test via `feed`. Responses are produced by a `RequestHandler` the test
  supplies per node (often the real `Responder`, giving an end-to-end master↔node loop).
  `Rate` steps set the rate at which the node "hears": a probe at another rate yields
  silence (or garbage, when the step says so). Garbage/babble bytes come from a seeded
  xorshift PRNG in the mock; scripts are plain aggregate-initialised arrays.
- **Rationale**: FR-030 (all seven behaviours), FR-033 (every §7 mode reachable from a
  script); tests may use the full language, but the mock stays allocation-free anyway so
  the same code can seed F4's virtual wire.
- **Alternatives considered**: YAML-driven scripts now — F4 owns scenario YAML; a step
  table is what the plan input asked for and is what F4 will map YAML onto.

## R-08 — Torture corpus: Python generator + differential over the helper; C++ property test for cut points

- **Decision**: `tools/refimpl/torture.py` generates, from a seed, streams of valid frames
  interleaved with every corruption class (bit flip, byte drop, byte insert, truncation,
  FLAG insertion, invalid escape, garbage burst, over-length) and the list of frames a
  correct receiver must deliver; it re-parses each corrupted segment with the reference
  deframer and **drops any corruption that still yields a valid frame** (CRC-lucky cases,
  spec edge case). `tools/diffcheck.py --frames` streams each corpus element to the
  helper (`FSTREAM <hex>` → one line per delivered frame) and compares with the Python
  deframer output line-for-line (SC-002 evidence, ≥ 10,000 frames, ≥ 1,000 corruptions per
  class, < 60 s). The C++ property test `tests/property/test_link_resync.cpp` does the
  plan input's "resync from random cut points": a seeded generator concatenates known
  frames, cuts/corrupts at random points, and asserts the intact frames are delivered.
- **Rationale**: the corpus never becomes a compiled artefact (10k frames as a header
  would bloat every build); the cross-implementation agreement is the strong claim and
  it runs through the same helper protocol F1 built.
- **Alternatives considered**: committing the corpus under `tests/vectors/` — immutable
  evidence should be small and human-checkable; a generated corpus is reproducible from
  its seed instead.

## R-09 — Golden vectors gain a `frame` kind

- **Decision**: extend `contracts/golden-vector.schema.json` (feature 001) with kind
  `frame` and name prefix `frame_`; fields `{dst, src, response, retry, seq, payload}`
  (payload as hex); canonical line `frame dst=0x01 src=0x00 flags=0x.. seq=n payload=<hex>`
  (contract `frame-canonical.md`); `bytes` = the stuffed wire bytes including FLAGs.
  Vectors: `frame_ping_req` (empty payload), `frame_max_payload` (64 B), `frame_worst_stuffing`
  (payload of 64 × 0x7E → 142 wire bytes), `frame_retry` (retry bit), `frame_response`
  (response bit + non-zero seq). `genvectors.py`, `codegen.py --vectors` (kind string is
  pass-through), `test_vectors*.py`, `canonical.py` and `tools/canonical.cpp` learn the
  new kind; `omgp_vectors.h.j2` comment lists it.
- **Rationale**: FR-006 and the F1 rule that vectors are the shared immutable evidence
  consumed by both implementations through the existing machinery; the schema is a
  feature-001 contract, so the change is recorded there as an additive amendment.
- **Alternatives considered**: a separate `tests/vectors/link/` tree with its own schema —
  duplicates the generator and the C++ header path.

## R-10 — Fuzzing: one new target, `fuzz_frame`

- **Decision**: `tests/fuzz/fuzz_frame.cpp` feeds the `Deframer` the input one byte at a
  time and, on a second pass, in random chunk sizes derived from the input, asserting no
  crash/hang/over-read (ASan/UBSan) and that any delivered frame re-encodes to bytes that
  parse to the same fields. Added to `TARGETS` in `tools/fuzz-smoke.sh` with seeds from
  `frame_*` vectors. The engines are not separate fuzz targets: their only untrusted input
  is bytes, which reach them through the same deframer.
- **Rationale**: FR-032; the F1 harness (preset, seeds, budget split, `setarch -R`) is
  reused unchanged except for the target list.

## R-11 — Python reference for framing: written from the document, before the C++

- **Decision**: `tools/refimpl/omgp_link.py`: `stuff/unstuff`, `encode_frame`,
  `Deframer` (same states and discard reasons as R-03, same names), pytest
  `test_link.py`. Task ordering puts the Python reference and the vectors before the C++
  deframer, as in feature 001, so the C++ author works from the document and the vectors.
- **Rationale**: constitution Principle III; the discard-reason vocabulary is shared so
  the differential compares reasons, not just delivered frames.

## R-12 — Timing-symbol test map (SC-001): tags in test names, checked mechanically

- **Decision**: every boundary test names its symbol in a Catch2 tag
  `[timing:T_resp]`, `[timing:T_turn_min]`, …, `[timing:bit_rate]`,
  `[timing:bit_rate_fallback]`, `[timing:retries]`, `[timing:max_payload]`;
  `tools/refimpl/test_timing_map.py` reads the `link_trunk` keys from the YAML, greps
  `tests/unit/test_link_*.cpp` and `tests/property/test_link_*.cpp` for the tags, and
  fails naming any symbol without a test. The same test prints the symbol → test-name map.
- **Rationale**: SC-001 asks for a mechanical mapping; tags are already how Catch2 tests
  are grouped, so the map costs nothing at runtime and cannot drift silently.
- **Alternatives considered**: a hand-maintained table in a doc — drifts.

## R-13 — Embedded-path enforcement extends to `link/`

- **Decision**: `tools/check_embedded.py` default `--cite-dirs` becomes `l3 link` (the
  citation regex already accepts `trunk §`); `link/` joins the clang-tidy set in the
  `quality` stage (it is already in the clang-format set and in `mutate.cfg` scope). The
  ESP-IDF build gains `esp32-host/components/omgp_link` (`SRC_DIRS ../../../link`) and
  `main/link_smoke.cpp` referencing the encoder, deframer, master and responder so the
  component is really linked.
- **Rationale**: FR-034; the F1 pattern (`omgp_l3` component + `l3_smoke.cpp`) is what
  proved the toolchain path, so it is copied rather than reinvented.

## R-14 — Differential helper: extend `l3_helper` rather than add a second binary

- **Decision**: `tools/l3_helper.cpp` gains verbs `FENC <canonical>` (→ hex wire bytes),
  `FDEC <hex>` (→ canonical or `ERR <reason>`), `FSTREAM <hex>` (→ one canonical line per
  delivered frame, in order, then `END <n_discards>`); `tools/canonical.cpp` renders
  frames. The bootstrap g++ path in `pipeline.sh` links `link/` sources into the helper.
- **Rationale**: one process, one line protocol, one reader thread — the F1 deadlock fix
  and batch design carry over; a second helper would duplicate the Python driver.

## R-15 — Local environment blind spots (stated, per CLAUDE.md working agreements)

- Mull runs locally only through the extracted LLVM-14 package with `MULL_RUNNER`/
  `MULL_PLUGIN`; CI uses the LLVM-18 deb (both verified on 2026-08-29). The ESP-IDF
  Docker build works locally (Docker integration enabled 2026-08-28) and takes ~2 min.
- No real UART exists; every timing claim is about the modelled wire (R-04). The
  provisional values will move when Backplane Rev A is measured — they move in the YAML,
  and SC-001's map is what proves the tests move with them.
- Python locally is 3.10: tooling avoids 3.11-only syntax (as in feature 001).
