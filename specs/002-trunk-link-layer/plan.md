# Implementation Plan: Trunk Link Layer

**Branch**: `002-trunk-link-layer` | **Date**: 2026-08-29 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/002-trunk-link-layer/spec.md`

## Summary

Deliver L2 for the host trunk as an embedded-path library `omgp_link` (`link/`): (1) a
frame codec — encoder with byte stuffing and CRC-16/CCITT-FALSE, and a byte-at-a-time
`Deframer` that resynchronises on the next FLAG after any discard; (2) a `Master` engine
implementing strict master poll with the retry rule (same sequence, retry bit, at most
`TRUNK_retries`), and a `Responder` engine with the single-frame replay buffer and the
turnaround window; (3) a `HealthTracker` implementing UNENROLLED/ENROLLED/SUSPECT/OFFLINE,
reduced-rate polling of SUSPECT nodes, the enrolment rotation, BUS_FAULT with
alternating-rate re-probe, per-address statistics and listener notifications. Every wait
is a comparison against the injected `Clock`; every constant is a generated `TRUNK_*` /
`LIMIT_*` symbol; every buffer is a fixed 142-byte array derived from those symbols.
Verification reuses the feature-001 machinery: an independent Python framing reference
and golden `frame_*` vectors, a differential run over a seeded torture corpus through the
existing helper, a `fuzz_frame` libFuzzer target, Catch2 unit/property tests driven by a
scripted `MockWire` step table and a `FakeClock`, a mechanical timing-symbol → test map,
the embedded-path scan extended to `link/`, the ESP-IDF component build, and the
triage-gated mutation run.

**Terms**: "host-core implementation" = `omgp_link` in `link/`; "reference
implementation" = `tools/refimpl/omgp_link.py`; "byte wire" = the `ByteWire` interface
(research R-01); "scripted transport" (spec) = `MockWire` (R-07).

## Technical Context

**Language/Version**: C++17 portable subset for `link/` (`-fno-exceptions -fno-rtti`,
no heap after init, no OS/wall-clock; virtual interfaces allowed, RTTI not needed);
Python 3.10-compatible tooling and reference (3.12 in CI).

**Primary Dependencies**: none new. Build: CMake ≥ 3.22 presets `native`/`fuzz`
(existing); GCC 11 / Clang 14 locally, Clang 18 in CI; ESP-IDF v5.3 via Docker.
Tests: vendored Catch2 v3 + `tests/support` (listener, heap guard), pytest, libFuzzer,
Mull 0.34.0 (deep-verify). Generated header `build/gen/omgp_protocol.h` supplies
`TRUNK_*`, `LIMIT_max_l3_payload`, CRC variant; `link/crc16.hpp` (existing) is the CRC.

**Storage**: files only — golden vectors (`tests/vectors/frame_*.json`, immutable), the
generated vectors header, seeded corpora generated in memory.

**Testing**: Catch2 unit (`tests/unit/test_link_*.cpp`) and property
(`tests/property/test_link_*.cpp`) with `MockWire` + `FakeClock`; pytest for the Python
reference, vectors, torture generator and the timing-symbol map; `tools/diffcheck.py
--frames` (differential, via `l3_helper` verbs `FENC`/`FDEC`/`FSTREAM`); `fuzz_frame`
under `tools/fuzz-smoke.sh`; `tools/check_embedded.py` over `link/` (citations required);
`tools/mutate.sh --diff origin/main` triage gate (zero unlabelled survivors).

**Target Platform**: Linux native (ASan/UBSan) and ESP32-S3 (`esp32-host/components/
omgp_link`, `main/link_smoke.cpp`). No UART, no device: the byte wire is an interface.

**Project Type**: embedded-portable library + test infrastructure + reference/tooling
extensions in an existing single repository.

**Performance Goals**: spec SC-002 (≥ 10k-frame torture corpus differential < 60 s on CI);
worst-case frame 140 bytes / 1.40 ms (SC-008, corrected 2026-08-31; `kMaxWire = 142`
is the buffer bound, unreachable on the wire); nothing else is throughput-bound.

**Constraints**: all timing via `Clock` (CLAUDE.md rule 3); symbols only (rule 4,
`check_embedded`); fixed buffers of `kMaxWire = 2 + 2·(4 + LIMIT_max_l3_payload + 2)`
(rule 5); L2 opaque to L3 (payload = bytes); every `link/` file cites `trunk §`; both
builds green; `UNIT_TEST_FLOOR` raised; mutation triage gate on changed lines.

**Scale/Scope**: 16 trunk addresses (fixed table); 9 timing/limit symbols each pinned by
a tagged boundary test; 5 frame vectors; 1 fuzz target; ≥ 10k-frame corpus; expected
~1.8k lines C++ in `link/` + `tests/support`, ~700 lines Python, ~1.2k lines tests.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Gate | Status |
|---|---|---|
| I Spec-first | Framing, retry, health and timing come from `docs/trunk-link-layer.md` §3/§4/§7/§9; every constant from the generated `link_trunk` symbols; the three open behaviours (bad escape, BUS_FAULT rule, post-fault rate) were ruled by the human on 2026-08-29 and recorded in OPEN-QUESTIONS before design; no YAML/doc change is needed (all `link_trunk` keys already exist) | PASS — no T3 slice required; the vectors schema amendment is a feature-001 contract file (T0) |
| II Test-first | Python reference + `frame_*` vectors first; each C++ component lands with failing Catch2 tests first; every §9 symbol has a tagged boundary test (R-12); bug fixes with a reproducing case | PASS |
| III Dual verification | Independent Python framing reference (written from the document, ordered before the C++ deframer); differential over frame corpus + torture corpus; `fuzz_frame` on the deframer | PASS — engines/health are behavioural, not codecs: verified by scripted-transport tests (spec assumption "Python reference scope") |
| IV Portability & determinism | `link/` compiled `-fno-exceptions -fno-rtti`, fixed arrays, `Clock` injected, no time/OS headers (scan); native ASan/UBSan + ESP-IDF component both in the pipeline | PASS |
| V Idempotency | L2 retry = same sequence + retry bit (R-04); replay buffer resends the stored response, never re-invokes the handler (R-05); tests drop/duplicate/corrupt responses in every position (SC-004) | PASS — this feature *is* the mechanism Principle V relies on |
| VI Simulator | Ruled Q1: L2 sits below `OMGPTransport`; F4's virtual wire is a second `ByteWire`; every §7 mode is a `MockWire` step / `HealthTracker` input, no special-cased paths (FR-033, SC-005 mapping table) | PASS — scenario YAML itself is F4 |
| VII Bridge discipline | Out of scope (§8, F4); the `Responder`'s turnaround clamp is the primitive §8 will sit on | N/A |
| VIII Minimal dependencies | Nothing new; Catch2/Mull/libFuzzer already approved | PASS |
| IX Traceability | Every `link/` file cites `trunk §` (scan enforces from this feature on); vectors carry `spec_ref`; timing tests carry `[timing:<symbol>]` tags | PASS |

**Post-design re-check (after Phase 1)**: unchanged. The listener interface (R-06)
removed the one design pressure toward a bounded notification queue whose overflow would
be a silent loss; the 142-byte derived constant (R-02) removed the pressure toward a
"large enough" literal.

## Project Structure

### Documentation (this feature)

```text
specs/002-trunk-link-layer/
├── plan.md              # This file
├── research.md          # Phase 0: decisions R-01..R-15
├── data-model.md        # Phase 1: frame, engine states, health record, stats, steps, corpus, vectors
├── quickstart.md        # Phase 1: how to prove it works end-to-end
├── contracts/
│   ├── link-cpp.md              # C++ API of omgp_link (types, codec, Master, Responder, HealthTracker)
│   ├── byte-wire-and-clock.md   # the two injected interfaces + the timing model
│   ├── link-python.md           # Python reference API (framing only) + torture generator
│   ├── mock-wire.md             # scripted transport step table + FakeClock
│   ├── frame-vectors.md         # `frame` vector kind, canonical line, helper verbs
│   └── tooling.md               # diffcheck --frames, fuzz_frame, check_embedded, timing map, esp32, pipeline
└── tasks.md             # Phase 2 (/speckit-tasks) — not created here
```

### Source Code (repository root)

```text
link/                             # omgp_link (embedded path; CLAUDE.md rule 5)
├── crc16.hpp                     # existing, unchanged
├── clock.hpp                     # NEW: omgp::Clock interface (R-01)
├── byte_wire.hpp                 # NEW: omgp::link::ByteWire interface (R-01)
├── link_types.hpp                # NEW: Status, FrameFields, FrameView, kMaxUnstuffed/kMaxWire, Stats, HealthState
├── frame.hpp/.cpp                # NEW: encode_frame(), Deframer (trunk §4)
├── master.hpp/.cpp               # NEW: Master engine (trunk §3, §7)
├── responder.hpp/.cpp            # NEW: Responder engine + replay buffer (trunk §3, §7)
├── health.hpp/.cpp               # NEW: HealthTracker, HealthListener, bus fault (trunk §6, §7)
└── CMakeLists.txt                # NEW: add_library(omgp_link) -fno-exceptions -fno-rtti

tests/
├── support/
│   ├── fake_clock.hpp            # NEW: settable Clock
│   └── mock_wire.hpp/.cpp        # NEW: scripted ByteWire (step table, PRNG, scheduling)
├── unit/
│   ├── test_link_frame.cpp       # NEW: codec + deframer + vectors + [timing:max_payload][timing:bit_rate]
│   ├── test_link_master.cpp      # NEW: transactions, retry, gap, discards + [timing:T_resp][timing:T_gap][timing:retries]
│   ├── test_link_responder.cpp   # NEW: window, replay + [timing:T_turn_min][timing:T_turn_max]
│   ├── test_link_health.cpp      # NEW: state machine, due-polling, rotation + [timing:T_poll]
│   ├── test_link_busfault.cpp    # NEW: declare/clear, alternating rates + [timing:bit_rate_fallback]
│   └── test_link_loop.cpp        # NEW: Master ↔ Responder over MockWire, drop/dup/corrupt in every position (SC-004)
├── property/
│   ├── test_link_stuffing.cpp    # NEW: stuff/unstuff round-trip, CRC published vectors
│   └── test_link_resync.cpp      # NEW: random cut points / corruptions, intact frames recovered
├── fuzz/
│   ├── fuzz_frame.cpp            # NEW
│   └── CMakeLists.txt            # + omgp_add_fuzz_target(fuzz_frame omgp_link)
└── vectors/
    └── frame_*.json              # NEW, immutable: ping_req, max_payload, worst_stuffing, retry, response

tools/
├── canonical.hpp/.cpp            # + frame rendering/parsing (host-only)
├── l3_helper.cpp                 # + FENC / FDEC / FSTREAM verbs
├── diffcheck.py                  # + --frames (frame corpus + torture corpus)
├── check_embedded.py             # --cite-dirs default `l3 link`
├── fuzz-smoke.sh                 # + fuzz_frame in TARGETS, seeds from frame_* vectors
└── refimpl/
    ├── omgp_link.py              # NEW: stuff/unstuff, encode_frame, Deframer
    ├── torture.py                # NEW: seeded corpus generator (+ expected frames)
    ├── canonical.py              # + frame lines
    ├── genvectors.py             # + frame vectors
    ├── test_link.py              # NEW
    ├── test_torture.py           # NEW
    ├── test_timing_map.py        # NEW: every link_trunk symbol has a [timing:…] test
    └── test_vectors*.py          # + frame kind

specs/001-protocol-foundation/contracts/golden-vector.schema.json   # + kind "frame", prefix frame_
esp32-host/
├── components/omgp_link/CMakeLists.txt   # NEW: SRC_DIRS ../../../link
└── main/link_smoke.cpp                   # NEW: references encoder/deframer/master/responder
CMakeLists.txt                    # add_subdirectory(link); omgp_add_catch_test links omgp_link; helper links link
pipeline.sh                       # bootstrap build compiles link/*.cpp; clang-tidy over link/; UNIT_TEST_FLOOR raised
```

**Structure Decision**: everything portable goes in the existing `link/` (CLAUDE.md
layout: "trunk L2: framing, stuffing, CRC16, retry/replay (portable)"), which is already
in the risk-score T2 regex, `mutate.cfg` scope, the clang-format set and `check_embedded`
dirs — no governance edits are needed. The two injected interfaces live in `link/` (R-01)
so that `core/` (F3) depends downward only. Test infrastructure lives in `tests/support/`
beside the existing listener and heap guard so every Catch2 test can link it. No new
top-level directory, no new dependency.

## Complexity Tracking

No constitution violations to justify. Two deliberate scope choices worth naming:

| Choice | Why | Simpler alternative rejected because |
|---|---|---|
| Virtual interfaces (`Clock`, `ByteWire`, `RequestHandler`, `HealthListener`) on the embedded path | Runtime substitution for tests and the simulator; F3/F4 build against them | Templates would make the engines header-only and harder for Mull/ESP-IDF; callbacks via `std::function` are forbidden |
| Amending the feature-001 vector schema instead of a link-specific vector format | One generator, one C++ header, one immutability rule for all evidence | A second vectors tree duplicates `genvectors.py`, the codegen path and `test_vectors*.py` |
