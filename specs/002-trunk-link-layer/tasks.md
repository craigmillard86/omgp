# Tasks: Trunk Link Layer

**Input**: Design documents from `/specs/002-trunk-link-layer/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md

**Tests**: REQUIRED — constitution Principle II (test-first) and CLAUDE.md rule 8. Within
every story, test tasks precede implementation tasks and MUST fail before the
implementation lands (record the failing run in the PR). The Python framing reference and
the `frame_*` golden vectors land before the C++ deframer that consumes them (research
R-11, R-09); after their creating commit, `tests/vectors/` is frozen again.

**Organization**: grouped by user story (spec.md US1–US5). Dependencies: US2 (Master)
and US3 (Responder) both build on US1's frame codec; US3's end-to-end loop test needs US2;
US4 (health) is independent of US2/US3 except for the shared `Clock`; US5 (bus fault)
extends US4's tracker and uses US2's `set_bit_rate`.

**Risk tiers** (GOVERNANCE.md §3): every task here is T2 (`link/`, `pipeline.sh`) or
lower — **no T3 task exists in this feature**: all `link_trunk` symbols already exist in
the YAML, the three open behaviours were ruled on 2026-08-29 (OPEN-QUESTIONS), and no
workflow, CLAUDE.md or spec-document edit is needed. `link/` is in the risk-score T2
regex, so every PR runs `deep-verify` (600 s fuzz + triage-gated mutation). The vectors
schema amendment (T006) touches a feature-001 contract file (T0). The vector-creating
commit (T020) is human-triggered with a written justification (CLAUDE.md rule 9).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1–US5)
- Include exact file paths in descriptions

## Path Conventions

Repository root layout per plan.md: `link/` (library `omgp_link` + the two interfaces),
`tests/support/` (FakeClock, MockWire), `tests/{unit,property,fuzz,vectors}/`,
`tools/` + `tools/refimpl/`, `esp32-host/components/omgp_link/`, `build/gen/` (generated,
not committed). Timing tests carry Catch2 tags `[timing:<symbol>]` (research R-12).

---

## Phase 1: Setup — build skeleton for `link/`

**Purpose**: an empty-but-linked `omgp_link` target on every build path so each story
lands as a vertical slice into a green tree.

- [ ] T001 Create `link/CMakeLists.txt` declaring `omgp_link` as an `INTERFACE` library for now (CMake refuses `add_library(... STATIC)` with no sources, and `link/` holds only the header-only `crc16.hpp` until T008 adds `link_status.cpp` — T008 converts it to `STATIC` with `-fno-exceptions -fno-rtti`), with `INTERFACE` include dirs `link/`, root, `build/gen`; add `add_subdirectory(link)` to the root `CMakeLists.txt`; extend `omgp_add_catch_test` to link `omgp_link`; link `omgp_link` into `omgp_canon` and `l3_helper` (contracts/tooling.md "CMake"; caught by the enrichment of issue #19)
- [ ] T002 [P] Extend the bootstrap g++ path in `pipeline.sh` (`stage_build`) to compile `link/*.cpp` into every Catch2 binary and into `l3_helper`; add `link` to the clang-tidy directory set in `stage_quality` (contracts/tooling.md "pipeline.sh")
- [ ] T003 [P] Create `esp32-host/components/omgp_link/CMakeLists.txt` (`idf_component_register(SRC_DIRS ../../../link INCLUDE_DIRS ../../../link ../../../build/gen)`, `-fno-exceptions -fno-rtti`) and `esp32-host/main/link_smoke.cpp` (initially references only `link/crc16.hpp`; grows in T047) so `./pipeline.sh esp32` links the component from the first PR
- [ ] T004 [P] Change `tools/check_embedded.py` default `--cite-dirs` to `l3 link` and add two cases to `tools/refimpl/test_check_embedded.py`: a `link/` file without a `trunk §` citation fails; a restated `200` in a `link/` file fails naming `TRUNK_T_resp_us` (write the test first; it fails on the old default)
- [ ] T005 [P] Make "L2 is opaque to L3" mechanical (spec FR-013): add a case to `tools/refimpl/test_check_embedded.py` asserting no file under `link/` includes anything from `l3/` (grep of `#include` lines), and in `link/CMakeLists.txt` leave `omgp_link` with no `target_link_libraries` on `omgp_l3` (a comment states why); write the test first — it passes on the empty library and is the guard for every later task
- [ ] T006 [P] Amend `specs/001-protocol-foundation/contracts/golden-vector.schema.json`: `kind` enum gains `frame`, `name` pattern gains the `frame_` prefix, `fields` description gains the frame field list (contracts/frame-vectors.md); extend `tools/refimpl/test_vectors.py` to validate `frame_*` files against it (no such files yet — the test iterates whatever exists)

**Checkpoint**: `./pipeline.sh codegen quality build unit` green on the cmake path AND the bootstrap path with an empty `omgp_link`; `./pipeline.sh esp32` green; `check_embedded.py` demands citations in `link/`.

---

## Phase 2: Foundational — injected interfaces, shared types, test harness

**Purpose**: the `Clock`/`ByteWire` interfaces, the shared types every story uses, and the
scripted transport skeleton all engine tests are written against.

**⚠️ CRITICAL**: no user-story work can begin until T007–T009 are complete. (T010/T011
are the exception — re-slotted after T022 by ruling 2026-08-30, because
`MockWire::transmit` deframes with the real `Deframer` per contracts/mock-wire.md; they
gate US2, not US1. See docs/OPEN-QUESTIONS.md.)

- [ ] T007 [P] Write `link/clock.hpp` (`omgp::Clock`, `now_us()`) and `link/byte_wire.hpp` (`omgp::link::ByteWire`: `transmit`, `receive`, `bit_rate`, `set_bit_rate`) exactly per contracts/byte-wire-and-clock.md, each citing `trunk §3`
- [ ] T008 [P] Write `link/link_types.hpp` per contracts/link-cpp.md "Types": `Status` + `status_name()`, `kHeaderLen/kCrcLen/kMaxUnstuffed/kMaxWire/kAddrCount` derived from `LIMIT_max_l3_payload` (no literal 70/142 — `check_embedded` would not catch 70/142 but review must), `byte_time_us()`, `FrameFields`, `FrameView`, `Discard`, `DeframerStats`, `HealthState` (UNENROLLED/ENROLLED/SUSPECT/OFFLINE), `Notice`, `AddrStats`, `BusStats`; add `link/link_status.cpp` for `status_name`, and convert `omgp_link` from `INTERFACE` (T001) to `add_library(omgp_link STATIC link_status.cpp)` with `-fno-exceptions -fno-rtti` and the same include dirs now `PUBLIC`
- [ ] T009 [P] Write `tests/unit/test_link_types.cpp` (write first): `kMaxWire == 2 + 2*(4 + LIMIT_max_l3_payload + 2)` and `== 142` `[timing:max_payload]`; `byte_time_us(TRUNK_bit_rate) == 10` and `byte_time_us(TRUNK_bit_rate_fallback) == 86` `[timing:bit_rate][timing:bit_rate_fallback]`; every `Status` has a distinct non-"?" name; `HealthState` names are the four trunk words; register it in `CMakeLists.txt`
- [ ] T010 Write `tests/support/fake_clock.hpp` (`FakeClock : Clock` with `advance`, `set`) and the `MockWire` skeleton `tests/support/mock_wire.{hpp,cpp}` per contracts/mock-wire.md: `Step`/`Kind`, per-node step arrays, `transmit()` returning `now + n × byte_time_us(rate)`, RX queue of `4 × kMaxWire` bytes with start-bit instants, `advance_to(t)` setting the clock and calling the engine's `poll(t)` (engines pull via `receive()`; no push path — analysis F1), xorshift32 PRNG, transcript of transmitted frames with `tx_start_us`; kinds `Respond` and `Silence` only for now (others in T030); add `mock_wire.cpp` to `omgp_test_support`. **Runs after T022** (ruling 2026-08-30: `Respond` needs the real `Deframer`, which does not exist until T022; do not stub framing here)
- [ ] T011 Write `tests/unit/test_mock_wire.cpp` (write first, fails until T010 is complete): `transmit` returns the modelled end instant at both rates; queued bytes are released in start-instant order by `advance_to`; the PRNG is deterministic for a seed; queue overflow is a `REQUIRE` failure not a silent drop

**Checkpoint**: pipeline green with `test_link_types`; `UNIT_TEST_FLOOR` raised.
(`test_mock_wire` joins the US2 entry checkpoint — T010/T011 re-slotted after T022,
ruling 2026-08-30.)

---

## Phase 3: User Story 1 — Frames survive a hostile wire (Priority: P1) 🎯 MVP

**Goal**: frame encoder + resynchronising `Deframer` in C++ and Python, golden `frame_*`
vectors, differential over ≥ 10 000 frames plus the torture corpus, `fuzz_frame` target.

**Independent Test**: `pytest tools/refimpl/test_link.py tools/refimpl/test_torture.py`
green; `./build/native/test_link_frame` and `test_link_resync` green; `python3
tools/diffcheck.py --frames-only` reports agreement in < 60 s; `./tools/fuzz-smoke.sh 60`
lists `fuzz_frame` with `findings=0`.

### Tests for User Story 1 (write first, must fail — then land WITH their implementation)

> **One-dispatch-unit rule (ruling 2026-08-30, docs/OPEN-QUESTIONS.md):** a write-first
> test task and the implementation task that makes it collect/compile/pass are ONE
> dispatch unit — one PR, test commits first with the recorded failing run in the PR
> body (that record is CLAUDE.md rule 8's evidence), then the implementation commits;
> the PR closes both issues and CI is green at every PR boundary. A merged tree is
> never left red awaiting a later task. Units here: T012+T018 (landed, PR #99),
> T013+T024 (PR #100), T014–T017+T022 (five issues, one PR).

- [ ] T012 [P] [US1] Write `tools/refimpl/test_link.py` per contracts/link-python.md: stuff/unstuff identities (7E/7D-only payloads), `crc(b"123456789") == 0x29B1`, hand-computed bytes for a PING frame and a 64-byte frame, `PayloadTooLong`/`ReservedAddress` refusals, deframer discard reasons for bad CRC / bad length / bad escape / trailing escape / 71 unstuffed bytes, resync after each corruption class, empty frame and back-to-back FLAG handling
- [ ] T013 [P] [US1] Write `tools/refimpl/test_torture.py`: `corpus(seed)` deterministic; every class ≥ `per_class`; ≥ 10 000 frames; no element's corrupted segment parses as a frame in isolation
- [ ] T014 [P] [US1] Write `tests/unit/test_link_frame.cpp`: `[vectors]` every `frame_*` vector encodes to its bytes and deframes byte-at-a-time to its fields (via `omgp::vectors::ALL`, `kind == "frame"`); `encode_frame` refusals with `written == 0`; exact-capacity buffer succeeds, one less is `BufferTooSmall`; the worst-stuffing vector is 142 bytes `[timing:max_payload]`; `Deframer` discard counters per reason; reserved ctrl bits ignored on decode and never set on encode; `HEAP_FREE_SCOPE` around encode and a full deframe
- [ ] T015 [P] [US1] Write `tests/property/test_link_stuffing.cpp`: seeded stuff→unstuff identity over random payloads incl. 7E/7D-dense ones; published CCITT-FALSE vectors through `encode_frame` (CRC bytes of a frame with known payload equal `crc16_ccitt_false` of dst..payload); frame time `= wire_len × byte_time_us` at both rates `[timing:bit_rate][timing:bit_rate_fallback]`
- [ ] T016 [P] [US1] Write `tests/property/test_link_resync.cpp`: seeded concatenation of known frames with corruption at random cut points (flip, drop, insert, truncate, FLAG insert, bad escape, garbage burst, over-length); assert exactly the intact frames are delivered in order and the deframer never delivers a frame that was corrupted
- [ ] T017 [P] [US1] Write `tests/fuzz/fuzz_frame.cpp` (byte-at-a-time pass, then chunked pass; any delivered frame re-encodes and re-parses to equal fields) and add `omgp_add_fuzz_target(fuzz_frame omgp_link)` to `tests/fuzz/CMakeLists.txt`; add `fuzz_frame` to `TARGETS` in `tools/fuzz-smoke.sh` with seeds from `frame_*` vectors (contracts/tooling.md). "Fails first" here means it does not compile until T022 provides `frame.hpp`; the `fuzz` preset is outside the default pipeline, so this blocks nothing earlier

### Implementation for User Story 1

- [ ] T018 [US1] Write `tools/refimpl/omgp_link.py` per contracts/link-python.md (`Frame`, `stuff`, `unstuff`, `encode_frame`, `crc`, `Deframer` with the R-03 states and discard names) from `docs/trunk-link-layer.md` §4 and the Q1 ruling — make T012 pass
- [ ] T019 [US1] Extend `tools/refimpl/canonical.py` with the frame line (`frame dst=… src=… flags=… seq=… payload=…`, `ERR <reason>`) per contracts/frame-vectors.md and extend `tools/refimpl/test_canonical.py` (write the cases first)
- [ ] T020 [US1] Extend `tools/refimpl/genvectors.py` with the five `frame_*` vectors (contracts/frame-vectors.md table) and generate them **once** into `tests/vectors/` — human-triggered commit whose message states the creating reason (CLAUDE.md rule 9); `genvectors.py --check` must then pass and `tests/vectors/` is frozen
- [ ] T021 [US1] Regenerate `build/gen/omgp_vectors.h` via `python3 tools/codegen.py --vectors tests/vectors` (kind string passes through; update the `kind` comment in `protocol/templates/omgp_vectors.h.j2`) and extend `tools/refimpl/test_vectors_complete.py` to require the five frame vectors
- [ ] T022 [US1] Write `link/frame.hpp` / `link/frame.cpp` (`encode_frame`, `Deframer` per contracts/link-cpp.md and data-model.md §3, citing `trunk §4`) — make T014, T015, T016 pass; add to `link/CMakeLists.txt`
- [ ] T023 [US1] Extend `tools/canonical.{hpp,cpp}` with frame rendering/parsing and `tools/l3_helper.cpp` with `FENC`/`FDEC`/`FSTREAM` verbs (contracts/frame-vectors.md); extend the bootstrap build if new sources are needed
- [ ] T024 [US1] Write `tools/refimpl/torture.py` per contracts/link-python.md (classes, self-check dropping CRC-lucky corruptions, determinism) — make T013 pass
- [ ] T025 [US1] Extend `tools/diffcheck.py` with `--frames` / `--frames-only`: ≥ 10 000 seeded frames through `FENC`/`FDEC` and every torture element through `FSTREAM`, first mismatch printed with `(seed, index)`; default run includes it; summary line gains `frames <n>, torture <m>`; confirm the whole `diffcheck` stage stays < 2 min and the torture part < 60 s (SC-002)
- [ ] T026 [US1] Run `./tools/fuzz-smoke.sh 60` (fuzz preset) and record in `tests/fuzz/README.md`: `fuzz_frame findings=0`, then the discriminating check (remove the `TooLong` guard → ASan finding → restore), with the actual output lines
- [ ] T027 [US1] Full `./pipeline.sh` + `./pipeline.sh esp32`; raise `UNIT_TEST_FLOOR`; `link/frame.cpp` and headers pass `check_embedded.py` (citations, no literals)

**Checkpoint**: US1 independent test passes; SC-002, SC-003, SC-008 demonstrated; the wire format is pinned by immutable vectors.

---

## Phase 4: User Story 2 — The host polls, waits, retries, never double-applies (Priority: P1)

**Goal**: `Master` engine with the retry rule, sequence numbering, response acceptance,
inter-frame gap and statistics, driven by `MockWire` with every fault kind.

**Independent Test**: `./build/native/test_link_master` green, including the tagged
boundary tests for `T_resp`, `T_gap` and `retries`.

### Tests for User Story 2 (write first, must fail)

- [ ] T028 [P] [US2] Extend `tests/unit/test_mock_wire.cpp` for the remaining step kinds (`Garbage` never contains a valid frame; `CrcError` flips the last CRC byte; `Duplicate` schedules the second copy `delay_us` after the first ends; `Babble` emits regardless of addressee; `Rate` makes a wrong-rate request behave as silence or garbage)
- [ ] T029 [P] [US2] Write `tests/unit/test_link_master.cpp`: happy path (one transmission, `Answered` with the payload, `stats.transactions == 1`); silence → exactly two retries with the same `seq` and `retry` set, then `Failed{Timeout}`, `attempts() == 3`, `stats.retries == 2`, `stats.timeouts == 3` `[timing:retries]`; response first byte at `tx_end + T_resp − 1` accepted and at `tx_end + T_resp` missed `[timing:T_resp]`; CRC-failed response ends the attempt immediately and counts `crc_failures`; late duplicate after success is discarded and does not affect the next transaction; wrong `src`/`dst`/`seq`/response-bit frames discarded and counted; second `begin()` before `last_activity + T_gap` transmits exactly at `+ T_gap` `[timing:T_gap]`; new transactions to one node use seq 0..15 then wrap; `begin()` while busy → `Busy`; 65-byte payload → `PayloadTooLong` with nothing on the wire; `HEAP_FREE_SCOPE` around a full transaction; the engine makes no progress without `poll()` (time not advanced → still `Pending`)

### Implementation for User Story 2

- [ ] T030 [US2] Implement the remaining `MockWire` kinds in `tests/support/mock_wire.cpp` — make T028 pass
- [ ] T031 [US2] Write `link/master.hpp` / `link/master.cpp` per contracts/link-cpp.md "Master engine" and data-model.md §4 (states, per-destination `next_seq`, acceptance checks, retry, gap deferral, stats, `set_bit_rate` pass-through incrementing `BusStats.rate_changes`), citing `trunk §3` and `§7` — make T029 pass; add to `link/CMakeLists.txt`
- [ ] T032 [US2] Full `./pipeline.sh` + `./pipeline.sh esp32`; raise `UNIT_TEST_FLOOR`; `mutate.sh --diff origin/main` locally (extracted Mull) — zero unlabelled survivors or labels justified in the PR body

**Checkpoint**: US2 independent test passes; every retry/discard rule of FR-007..FR-012 has a named test.

---

## Phase 5: User Story 3 — A node answers in its window and replays, never re-executes (Priority: P2)

**Goal**: `Responder` engine with the turnaround window and the single-frame replay
buffer; an end-to-end `Master ↔ Responder` loop over `MockWire` proving retry/replay
safety in every position (SC-004) and the §7-mode → script mapping (SC-005).

**Independent Test**: `./build/native/test_link_responder` and `test_link_loop` green.

### Tests for User Story 3 (write first, must fail)

- [ ] T033 [P] [US3] Write `tests/unit/test_link_responder.cpp`: response first byte exactly at `request_end + T_turn_min` by default `[timing:T_turn_min]`; a constructor `turnaround_us` above the maximum is clamped to `T_turn_max` and one below the minimum to `T_turn_min` `[timing:T_turn_max]`; handler invoked once per new seq; retry of the buffered seq → identical bytes retransmitted, handler not invoked, `replays_served == 1`; different seq with retry set → treated as new; retry before any answer → new; frames for other addresses and corrupt frames → nothing transmitted, `discards` counted; never transmits outside a window (no bytes when nothing was addressed to it); first `poll()` after `request_end + T_turn_max` → transmits at once and `late_responses == 1` (spec FR-014); `HEAP_FREE_SCOPE` around handle+respond
- [ ] T034 [P] [US3] Write `tests/unit/test_link_loop.cpp` (needs T031): real `Master` and real `Responder`s on one `MockWire` (the mock's handler for node *n* is `Responder` *n*); the SC-004 matrix — {drop, duplicate, delay-past-T_resp, corrupt} × {attempt 0, retry 1, retry 2, after give-up} — asserting `handler invocations == 1` per new sequence, `transmissions ≤ 3`, accepted `seq == transaction seq`; plus a comment block mapping each §7 mode (response timeout, CRC-failed response, SUSPECT, OFFLINE, BUS_FAULT, babble, duplicate, wrong-rate probe) to the script that produces it (SC-005; the health rows reference T039/T042 scripts)

### Implementation for User Story 3

- [ ] T035 [US3] Write `link/responder.hpp` / `link/responder.cpp` per contracts/link-cpp.md "Responder engine" and data-model.md §5, citing `trunk §3` and `§7` — make T033 and T034 pass; add to `link/CMakeLists.txt`
- [ ] T036 [US3] Full `./pipeline.sh` + `./pipeline.sh esp32`; raise `UNIT_TEST_FLOOR`; local mutation run; PR body lists any labels

**Checkpoint**: SC-004 demonstrated end-to-end; the replay buffer is proven to never re-invoke the handler.

---

## Phase 6: User Story 4 — Node health follows the spec's state machine (Priority: P2)

**Goal**: `HealthTracker` with UNENROLLED/ENROLLED/SUSPECT/OFFLINE, reduced-rate polling,
enrolment rotation and one notification per transition.

**Independent Test**: `./build/native/test_link_health` green, including `[timing:T_poll]`.

### Tests for User Story 4 (write first, must fail)

- [ ] T037 [P] [US4] Write `tests/unit/test_link_health.cpp` with a recording `HealthListener`: every transition of data-model.md §6 and every non-transition at the boundary (2 failures stay ENROLLED; SUSPECT at 999 ms stays; `tick()` at 1000 ms → OFFLINE without a result); UNENROLLED never counts failures; `poll_due` for SUSPECT false at `9 × T_poll` after the last poll and true at `10 × T_poll` `[timing:T_poll]`; `next_probe` rotates over UNENROLLED and OFFLINE addresses only and skips 0x00; OFFLINE → ENROLLED on a valid answer with `RECOVERED`; exactly one notice per transition (count equality); `HEAP_FREE_SCOPE` around a full sequence

### Implementation for User Story 4

- [ ] T038 [US4] Write `link/health.hpp` / `link/health.cpp` per contracts/link-cpp.md "Health tracker" and data-model.md §6 (16-entry table, `on_result`, `tick`, `state`, `poll_due`, `mark_polled`, `next_probe` rotation — bus-fault parts stubbed to "never fault" until US5), citing `trunk §6` and `§7` — make T037 pass; add to `link/CMakeLists.txt`
- [ ] T039 [US4] Extend `tests/unit/test_link_loop.cpp` with the SUSPECT and OFFLINE scripts (three `Silence` steps → SUSPECT; silence for 1 s of simulated time → OFFLINE; a `Respond` after that → RECOVERED) driving `Master` + `HealthTracker` together — write first, then wire `HealthTracker::on_result` from the loop
- [ ] T040 [US4] Full `./pipeline.sh` + `./pipeline.sh esp32`; raise `UNIT_TEST_FLOOR`; local mutation run

**Checkpoint**: SC-006 demonstrated; F3 has `poll_due`/`next_probe`/`on_result`/`tick` to build the superframe on.

---

## Phase 7: User Story 5 — A dead bus is not ten dead nodes (Priority: P3)

**Goal**: BUS_FAULT declaration (ruling Q2), alternating-rate re-probe and recovery
(ruling Q3), rate pinned to the answering rate, bus statistics.

**Independent Test**: `./build/native/test_link_busfault` green, including
`[timing:bit_rate_fallback]`.

### Tests for User Story 5 (write first, must fail)

- [ ] T041 [P] [US5] Write `tests/unit/test_link_busfault.cpp`: three enrolled nodes all SUSPECT → exactly one `BUS_FAULT` and one `ALERT`, `bus_faults == 1`; two nodes with one ENROLLED → none; a single enrolled node SUSPECT → declared (ruling Q2); `next_probe()` rates alternate fallback, reference, fallback… while faulted, each change counted in `rate_changes` `[timing:bit_rate_fallback]`; first valid answer at the fallback rate → `BUS_RECOVERED` once, `bit_rate() == TRUNK_bit_rate_fallback`, answering node ENROLLED, the others keep SUSPECT/OFFLINE timers (one of them goes OFFLINE at its own 1 s mark afterwards); a second fault after recovery is declared again (once)
- [ ] T042 [P] [US5] Extend `tests/unit/test_link_loop.cpp` with the wrong-rate script: all nodes `Rate 115200`; host probes at 1 Mbit/s → silence → SUSPECT → BUS_FAULT → the alternating probe at 115 200 gets an answer → recovery, `Master::set_bit_rate` observed on the mock

### Implementation for User Story 5

- [ ] T043 [US5] Implement the bus-fault logic in `link/health.cpp` per data-model.md §7 (declare rule, alternation in `next_probe`, clear-and-pin on the first valid answer, `BusStats`), citing `trunk §7` — make T041 and T042 pass
- [ ] T044 [US5] Full `./pipeline.sh` + `./pipeline.sh esp32`; raise `UNIT_TEST_FLOOR`; local mutation run

**Checkpoint**: SC-007 demonstrated; every §7 mode has a script (SC-005 table complete).

---

## Phase 8: Polish & Cross-Cutting Concerns

- [ ] T045 Write `tools/refimpl/test_timing_map.py` (contracts/tooling.md): every `link_trunk` symbol and `limits.max_l3_payload` maps to a `[timing:…]` tag found in `tests/unit/test_link_*.cpp` or `tests/property/test_link_*.cpp`; prints the symbol → `TEST_CASE` map (SC-001); confirm renaming one tag fails it
- [ ] T046 [P] Write `link/README.md`: purpose, the portable-subset constraints and their enforcement, the timing model in one paragraph, API pointer to `specs/002-trunk-link-layer/contracts/link-cpp.md` and `byte-wire-and-clock.md`, and the F3/F4 interface note summary (SC-010)
- [ ] T047 [P] Complete `esp32-host/main/link_smoke.cpp`: trivial in-memory `ByteWire`/`Clock`, references `encode_frame`, `Deframer`, `Master`, `Responder`, `HealthTracker` so every translation unit of `omgp_link` links on Xtensa; `./pipeline.sh esp32` green
- [ ] T048 [P] Run `clang-format -i` over `link/ tests/ tools/*.cpp`; `./pipeline.sh quality` clean including clang-tidy over `link/`
- [ ] T049 Execute every step of `specs/002-trunk-link-layer/quickstart.md` on the cmake path and, with `cmake` masked from PATH, the bootstrap path; record the actual outputs of the three discriminating checks (fuzz `TooLong`, mutation `attempt <= retries`, timing-map rename) in `tests/fuzz/README.md` and the PR body
- [ ] T050 Final `UNIT_TEST_FLOOR` raise; confirm `tests/vectors/` shows only the T020 creating commit for `frame_*` (`git log --oneline -- tests/vectors`)
- [ ] T051 Append to `docs/OPEN-QUESTIONS.md` any assumption that implementation turned into a question (append-only; e.g. the integer `byte_time_us` at the fallback rate, the clock-based reading of "once per 10 superframes") with recommendation + "Ruling: pending" for the human reviewer

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: no dependencies; T002–T006 parallel after T001.
- **Foundational (Phase 2)**: depends on Phase 1; T007/T008/T009 parallel. T007–T009
  BLOCK all stories. T010 after T007/T008 **and T022**, T011 after T010 (ruling
  2026-08-30: `MockWire::transmit` deframes with the real `Deframer` —
  contracts/mock-wire.md; found live on #28 by the dispatch agent. T010/T011 gate US2,
  not US1).
- **US1 (Phase 3)**: depends on Phase 2 (T007–T009; **not** T010/T011). Python first (T012/T013 → T018 → T019 → T020 human commit → T021), then C++ (T014–T017 tests → T022 → T023 → T024 → T025 → T026 → T027). BLOCKS US2/US3 (they encode/decode frames).
- **US2 (Phase 4)**: depends on US1 (frame codec) and on T010/T011 (`MockWire`, re-slotted after T022); T028/T029 parallel; T030 → T031 → T032.
- **US3 (Phase 5)**: depends on US1; T034 additionally depends on T031 (US2). T033/T034 parallel; T035 → T036.
- **US4 (Phase 6)**: depends on Phase 2 only (a `Clock`); T039 depends on T031 + T038. Can run in parallel with US2/US3 by a second agent — but the WIP cap is one.
- **US5 (Phase 7)**: depends on US4 (extends `health.cpp`) and US2 (`set_bit_rate`); T042 depends on T034/T039.
- **Polish (Phase 8)**: depends on all stories; T045 needs every tag present.

### Within Each User Story

- Tests first; they MUST fail before the implementation lands, and the failing run is
  recorded in the PR body. Test task + implementation task are one dispatch unit (one
  PR; ruling 2026-08-30) — "fail first" is commit ordering inside that PR, never a red
  file merged to await a later task: a bare import/include of a not-yet-written module
  aborts pytest collection (or the C++ build) for the whole tree, turning the merge
  gate red by design — observed on PR #99 (T012) and PR #100 (T013), each burning a
  ci-failure-router auto-fix cycle.
- Python reference and vectors before C++ (R-11, R-09); the vector commit is human-triggered and justified; afterwards `genvectors.py --check` proves immutability.
- Each story ends with a full `./pipeline.sh` + `./pipeline.sh esp32`, a `UNIT_TEST_FLOOR` raise and a local mutation run whose survivors are triaged (a/b/c) in the PR body.

### Parallel Opportunities

- Phase 1: T002, T003, T004, T006 after T001.
- Phase 2: T007, T008, T009 together.
- US1: T012–T017 (six test files) together; T018/T019 while T014–T017 are being written.
- US2: T028/T029 together. US3: T033/T034 together. US5: T041/T042 together.
- Polish: T046, T047, T048 together.

---

## Parallel Example: User Story 1

```bash
# All six US1 test files together (different files, all must fail first):
Task: "Write tools/refimpl/test_link.py"
Task: "Write tools/refimpl/test_torture.py"
Task: "Write tests/unit/test_link_frame.cpp"
Task: "Write tests/property/test_link_stuffing.cpp"
Task: "Write tests/property/test_link_resync.cpp"
Task: "Write tests/fuzz/fuzz_frame.cpp + register target"

# Then, sequentially: T018 (Python codec) → T019 (canonical) → T020 (vectors, human commit)
# → T021 (regen header) → T022 (C++ codec) → T023 (helper verbs) → T024 (torture) → T025 (diffcheck)
# → T026 (fuzz evidence) → T027 (pipeline + floor)
```

---

## Implementation Strategy

### MVP First (Phase 1 + 2 + US1)

1. Phase 1: `omgp_link` linked everywhere while empty; scan covers `link/`.
2. Phase 2: interfaces, types, `FakeClock`, `MockWire` skeleton with their own tests.
3. US1: framing in both languages, vectors, differential + torture, fuzz target.
4. **STOP and VALIDATE**: quickstart §2 and §7; the wire format is pinned.

### Incremental Delivery

1. US2 → the host can transact with the retry rule; every timeout/retry rule tested.
2. US3 → node side + end-to-end loop; SC-004 retry/replay safety demonstrated.
3. US4 → health state machine; F3 can be planned against it.
4. US5 → bus fault; SC-005 table complete.
5. Polish → SC-001 mechanical map, README, quickstart executed on both paths.

### One PR per task group, WIP cap 1

GOVERNANCE.md §2 caps agent work in flight at one item. Suggested PR boundaries:
T001–T011 (scaffold + harness) · T012–T021 (Python framing + vectors; the vector commit
is human-triggered) · T022–T027 (C++ framing, helper, diffcheck, fuzz) · T028–T032 (Master)
· T033–T036 (Responder + loop) · T037–T040 (health) · T041–T044 (bus fault) · T045–T051.
Every PR is T2 (`link/`), so `deep-verify` runs on each: 600 s fuzz and the triage-gated
mutation. Each PR: `Refs #n` in commits, `Closes #n` in the body, claim-labelled evidence
per CLAUDE.md rule 11, a NOT EXAMINED section, and any `mutant-ok` labels listed with
their justification.

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Timing symbols are never restated: `check_embedded.py` catches the values ≥ 0x10; review must catch derived constants (70, 142, 10, 86) that are computed from symbols, not typed
- `tests/vectors/` is immutable after T020; a wrong vector is regenerated from the Python reference with the reason in the commit message, never edited by hand
- Every fast/partial path (bootstrap build, Mull absent, clang absent) prints its blind spot
- Verify tests fail before implementing; paste the failing run into the PR
