# Implementation Plan: Host-Core Control Engine

**Branch**: `003-host-core-engine` | **Date**: 2026-09-20 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/003-host-core-engine/spec.md`

## Summary

Deliver the host-core control engine as a new portable library `omgp_core` (`core/`): a
2 ms superframe scheduler (status polls, one enrolment probe, budgeted demand traffic with
carry-over — trunk §6) built directly on the existing `link::Master` and `link::HealthTracker`
engines (research R-02 — not a separate `OMGPTransport` wrapper); module discovery
(IDENTIFY → descriptor read with chunking → cache by MODEL_ID+CRC); node-ID assignment from
`BP_SLOT_MAP`, whose wire format this feature must itself define (R-01, a small protocol
addition); the accept-then-settle channel-switch flow; parameter set/get (asynchronous,
callback-delivered — R-10); event draining prioritised ahead of parameter/descriptor demand
traffic (spec FR-020); and node lifecycle reporting — presence, derived module liveness, and
forwarded backplane/bus health (R-03) — through a function-pointer-plus-context callback
interface (R-04, no `std::function`), delivered only via an explicit, caller-invoked
`drain_callbacks()` off two pending-delivery rings so `run_superframe()` itself can never
block on application code (spec FR-021; R-04's `/speckit-analyze` correction). Tests: a
message-level scripted test double this feature
builds itself (`MockL3Node`, R-08 — F2 did not deliver the `MockTransport` the original
roadmap assumed) driving the real `Master`+`HealthTracker` over a `FakeClock`; full
multi-node/fault-injection rig scenarios stay F4's, per the plan input.

## Technical Context

**Language/Version**: C++17 portable subset for `core/` (`-fno-exceptions -fno-rtti`, no heap
after init, no OS/wall-clock); reuses `link/`'s and `l3/`'s existing build presets and
toolchains (GCC 11 / Clang 14 locally, Clang 18 CI, ESP-IDF v5.3). One small `l3/` codec
addition (R-01) in the same C++17 embedded subset.

**Primary Dependencies**: none new. `link/master.hpp`, `link/health.hpp`, `link/clock.hpp`,
`link/link_types.hpp` (R-02); every `l3/` header this feature's opcodes touch; the generated
protocol header. Build/test: existing CMake presets, vendored Catch2, pytest, Mull
(deep-verify) — nothing new to vet under Principle VIII.

**Storage**: none — every table in `data-model.md` is fixed-size and in-memory for the life of
the running engine (spec Assumption: descriptor-cache persistence across a restart is out of
scope, same boundary as preset persistence).

**Testing**: Catch2 unit (`tests/unit/test_core_*.cpp`) driven by `MockL3Node` (R-08, this
feature's own scripted test double, `tests/support/mock_l3_node.hpp`) + `FakeClock` (F2's
existing one, reused); pytest for the new `BP_SLOT_MAP` codec pair and its Python reference
(`tools/refimpl/omgp_l3.py`, `tools/refimpl/test_l3.py`); `tools/diffcheck.py` carries the new
payload through the existing L3 differential path (no `--frames`-style special case needed,
R-01); `tools/check_embedded.py --cite-dirs` gains `core` (currently `["l3", "link"]`).

**Target Platform**: Linux native (ASan/UBSan) and ESP32-S3
(`esp32-host/components/omgp_core`, `main/core_smoke.cpp`) — same two-build gate as every
other portable-path feature (CLAUDE.md rule 10).

**Project Type**: embedded-portable library + a small protocol/codec addition + test
infrastructure, in the existing single repository.

**Performance Goals**: spec SC-002 (event visible within backplane poll period + 2 ×
`TRUNK_T_poll_us`, including under concurrent parameter-set load — asserted directly, not
sampled); SC-003 (a 40-item parameter burst never skips a superframe's status polls to any
enrolled backplane). Nothing else in this feature is throughput-bound.

**Constraints**: all timing via `Clock` (CLAUDE.md rule 3); symbols only, never restated
literals (rule 4, extended `check_embedded` scan); every table in `data-model.md` fixed at
`LIMIT_max_nodes`/`ADDR_backplane_*`/ring-capacity bounds (rule 5); `core/` includes only
`link::Master`/`HealthTracker`/`Clock`, `l3/`, and the generated header — never
`link::Responder`, `link::frame`, `link::ByteWire` directly, `sim/`, `cli/`, or a platform
header (R-02); every `core/` file cites `trunk §` or `protocol-l3 §`; both builds green;
`UNIT_TEST_FLOOR` raised; mutation triage gate on changed lines (Principle II/III/IV/IX).

**Scale/Scope**: 112 module addresses + 15 backplane addresses tracked (R-05); up to 32
cached distinct descriptors; three demand-item rings of `LIMIT_max_nodes` entries each (R-07);
success benchmark 3 backplanes / 12 modules (spec SC-001); expected ~1.5k lines C++ in `core/`
+ `tests/support/mock_l3_node.*`, a ~150-line `l3/` addition plus one golden vector, ~1.2k
lines of `core/` tests.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Gate | Status |
|---|---|---|
| I Spec-first | `BP_SLOT_MAP`'s payload format does not exist in the authoritative documents (`docs/protocol-l3.md` §3.1, §6 open question 1 names the sibling `BP_ROUTE` question but not this one) — this feature defines it (R-01), edits `protocol/omgp-protocol.yaml` + `docs/protocol-l3.md` in the same commit, regenerates, and records the decision in `docs/OPEN-QUESTIONS.md` (recommended default, ruling pending) rather than resolving it silently in `core/` code. Node-ID assignment (R-06) is a planning-level algorithm choice, not a protocol fact, and does not touch the wire format. | PASS — one T3-adjacent doc/YAML slice (R-01) flagged, not silently taken |
| II Test-first | `MockL3Node` scripts + failing Catch2 tests precede `core_engine.cpp` for every user story; the `BP_SLOT_MAP` codec lands with its Python reference and golden vector before `core/` reads it | PASS |
| III Dual verification | The new `BP_SLOT_MAP` codec pair gets the same Python-reference + differential + fuzz treatment every other L3 payload has (`bp-slot-map.md` "Differential coverage") | PASS — `core_engine.cpp` itself is orchestration, not a codec: verified by `MockL3Node`-scripted tests, the same standing 002's plan.md recorded for `Master`/`Responder`/`HealthTracker` |
| IV Portability & determinism | `core/` compiled `-fno-exceptions -fno-rtti`, every table fixed-size (`data-model.md`), `Clock` injected, no time/OS headers (scan extended to `core/`); native ASan/UBSan + ESP-IDF component both in the pipeline | PASS |
| V Idempotency | `SET_PARAM` already carries an absolute value at L3 (Principle V's own mechanism, unchanged); a demand item retried after a transient `Master` failure is therefore always safe to resend, so this feature adds no new idempotency surface to reason about | PASS |
| VI Simulator | `core/` takes `link::ByteWire` + `Clock` transitively through `Master`/`HealthTracker` (unchanged from F2) — the same swap point F4's `VirtualTransport` will use; `MockL3Node` (R-08) is this feature's own stand-in, not a simulator substitute | PASS — full scenario-YAML-driven rig scenarios stay F4's, per the plan input |
| VII Bridge discipline | Out of scope: `core/` is the poller, not the bridge — §8's `T_resp`/`ERR_BUSY` obligation binds a backplane's own firmware/virtual model, which this feature does not implement | N/A |
| VIII Minimal dependencies | Nothing new | PASS |
| IX Traceability | Every `core/` file cites `trunk §6`/`§7` or `protocol-l3 §3`/`§4`; `check_embedded.py` scan extended; test names reference the FR/SC they verify (`quickstart.md`) | PASS |

**Post-design re-check (after Phase 1)**: unchanged. `LifecycleEvent` as one discriminated
struct (data-model §7) removed the design pressure toward a class hierarchy that would have
pushed `core/` toward RTTI-adjacent dispatch; the free-pool node-ID allocator (R-06) removed
the pressure toward a fixed-block formula that Phase 0 research showed does not fit the
protocol's own stated backplane slot-count range.

## Project Structure

### Documentation (this feature)

```text
specs/003-host-core-engine/
├── plan.md              # This file
├── research.md          # Phase 0: decisions R-01..R-12
├── data-model.md         # Phase 1: node/backplane records, lifecycle event, demand items, BP_SLOT_MAP payload
├── quickstart.md         # Phase 1: how to prove it works end-to-end
├── contracts/
│   ├── core-cpp.md       # C++ API of omgp_core (CoreEngine, callbacks, node-id assignment)
│   ├── bp-slot-map.md    # the new BP_SLOT_MAP wire format and its codec (R-01)
│   └── mock-l3-node.md   # this feature's own scripted L3 test double (R-08)
└── tasks.md              # Phase 2 (/speckit-tasks) — not created here
```

### Source Code (repository root)

```text
core/                              # omgp_core (embedded path; CLAUDE.md rule 5)
├── core_types.hpp                # NEW: CoreStatus, DiscoveryState, NodeRecord, BackplaneRecord,
│                                  #      DescriptorCacheEntry, LifecycleEvent, ParamRequestId/
│                                  #      ParamResult, demand-item structs, SuperframeBudget,
│                                  #      CoreCallbacks (data-model.md)
├── core_engine.hpp/.cpp           # NEW: CoreEngine — superframe scheduler, discovery,
│                                  #      node-id assignment (R-06), channel/param operations,
│                                  #      implements link::HealthListener (R-03)
└── CMakeLists.txt                 # NEW: add_library(omgp_core) -fno-exceptions -fno-rtti,
                                    #      links omgp_link + omgp_l3

l3/
├── l3_types.hpp                   # + BpSlotMapResp (R-01)
└── l3_payload.hpp/.cpp            # + encode_bp_slot_map_resp / decode_bp_slot_map_resp

tests/
├── support/
│   └── mock_l3_node.hpp/.cpp      # NEW: scripted L3 responder over a trivial in-memory
│                                  #      ByteWire, real Master + HealthTracker (R-08)
└── unit/
    ├── test_core_discovery.cpp    # NEW: US1 — cold boot, deterministic transcript
    ├── test_core_scheduler.cpp    # NEW: US2 — budget/carry-over, SC-002/SC-003 under load
    ├── test_core_channel.cpp      # NEW: US3 — accept-then-settle, timeout
    ├── test_core_events.cpp       # NEW: US4 — bounded event latency
    ├── test_core_lifecycle.cpp    # NEW: US5 — presence + derived/forwarded health (R-03)
    ├── test_core_params.cpp       # NEW: FR-023/FR-024 — parameter failure + async GetParam
    └── test_l3_bp_slot_map.cpp    # NEW: R-01 codec unit tests

tools/
├── refimpl/
│   ├── omgp_l3.py                 # + encode/decode_bp_slot_map_resp
│   ├── test_l3.py                 # + bp_slot_map cases
│   └── genvectors.py              # + bp_slot_map_full_occupancy vector
├── diffcheck.py                   # + bp_slot_map through the existing L3 payload differential
└── check_embedded.py              # --cite-dirs default gains `core`

esp32-host/
├── components/omgp_core/CMakeLists.txt   # NEW: SRC_DIRS ../../../core
└── main/core_smoke.cpp                    # NEW: references CoreEngine so every translation
                                            #      unit of omgp_core links on Xtensa

protocol/omgp-protocol.yaml         # + l3_payloads.BP_SLOT_MAP, limits.bp_slot_map_max_slots
docs/protocol-l3.md                  # + §3.1 BP_SLOT_MAP payload table row
docs/OPEN-QUESTIONS.md               # + R-01 entry: recommended default, ruling pending
CMakeLists.txt                       # add_subdirectory(core); omgp_add_catch_test links omgp_core
pipeline.sh                          # bootstrap build compiles core/*.cpp; clang-tidy over
                                      # core/; UNIT_TEST_FLOOR raised
```

**Structure Decision**: everything portable goes in the existing `core/` placeholder
(CLAUDE.md layout: "host-core: scheduler, node health, discovery, presets (portable)"),
already in the risk-score T2 regex (`^(core\/|link\/|l3\/)`) — no governance edit needed
there; `check_embedded.py`'s `--cite-dirs` default (currently `["l3", "link"]`) does need
`core` added. `transport/` stays untouched (R-02) — nothing in this feature populates it.
Test infrastructure's one new file (`mock_l3_node.*`) lives in `tests/support/` beside the
existing `fake_clock.hpp` and `mock_wire.*`, reusing `fake_clock.hpp` directly rather than
duplicating it.

## Complexity Tracking

No constitution violations to justify. Three deliberate scope choices worth naming:

| Choice | Why | Simpler alternative rejected because |
|---|---|---|
| Defining `BP_SLOT_MAP`'s wire format now (R-01), inside this feature, rather than treating it as a blocking external dependency | Discovery (this feature's own P1 user story) cannot be built at all without it, and the format is already fully determined by the opcode's existing prose description — only bytes are missing | Waiting for a separate ruling before starting would block the feature's highest-priority story on a decision this feature is itself best positioned to make and record (CLAUDE.md working agreement: recommended default + proceed) |
| `core/`'s node-ID pool is first-fit in canonical order (R-06), not a closed-form `f(backplane, slot)` formula | The protocol's own numbers (112 module addresses over up to 15 backplanes of up to 16 slots each) make any fixed per-backplane block either too small for one large backplane or wasteful for small ones | A fixed formula was the spec-drafting-time assumption; Phase 0 research showed it does not fit the numbers, so it was corrected here rather than carried forward on the strength of having been written down first |
| `core/`'s own module-liveness tracking (R-03) duplicates `HealthTracker`'s threshold *constants* rather than its *code* | `HealthTracker` is hard-sized to 16 trunk addresses and built around L2 probe-rotation semantics that do not exist at the module layer (modules are never L2-addressable at all) | Reusing `HealthTracker` itself (templated or generalised to a wider address range) would pull L2-only concepts — alternating-rate re-probe, the reference pass — into a layer that has no rate or probe of its own to alternate |
