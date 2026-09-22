---

description: "Task list for feature 003 — Host-Core Control Engine"
---

# Tasks: Host-Core Control Engine

**Input**: Design documents from `/specs/003-host-core-engine/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md (all present)

**Tests**: test-first throughout (constitution Principle II, NON-NEGOTIABLE) — every
implementation task is preceded by a failing test task for the behaviour it makes pass.

**Organization**: Tasks are grouped by user story (spec.md, priority order P1/P1/P2/P2/P3) so
each story is independently implementable, testable, and shippable, matching
`specs/002-trunk-link-layer/tasks.md`'s own convention (a closing "full pipeline" task ends
each story's phase, not deferred to one final polish phase).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies on incomplete tasks)
- **[Story]**: Which user story this task belongs to (US1..US5); Setup/Foundational/Polish carry none
- File paths are exact, per `plan.md`'s Project Structure

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: build wiring, the `BP_SLOT_MAP` protocol addition (R-01) every later story reads.

- [ ] T001 Create `core/CMakeLists.txt` declaring `omgp_core` as an `INTERFACE` library for now
      (no sources exist until T015); `INTERFACE` include dirs `core/`, root, `build/gen`; add
      `add_subdirectory(core)` to the root `CMakeLists.txt`; extend `omgp_add_catch_test` to
      link `omgp_core` (contracts/core-cpp.md; mirrors `specs/002-trunk-link-layer/tasks.md`
      T001's own INTERFACE-then-STATIC pattern)
- [ ] T002 [P] Extend the bootstrap g++ path in `pipeline.sh` (`stage_build`) to compile
      `core/*.cpp` into every Catch2 binary and into `l3_helper`; add `core` to the clang-tidy
      directory set in `stage_quality`
- [ ] T003 [P] Create `esp32-host/components/omgp_core/CMakeLists.txt`
      (`idf_component_register(SRC_DIRS ../../../core INCLUDE_DIRS ../../../core
      ../../../build/gen)`, `-fno-exceptions -fno-rtti`) and a placeholder
      `esp32-host/main/core_smoke.cpp` (references only `core/core_types.hpp` until T045
      completes it against the real `CoreEngine`)
- [x] T004 [P] Change `tools/check_embedded.py` default `--cite-dirs` to `l3 link core` and add
      a case to `tools/refimpl/test_check_embedded.py`: a `core/` file without a `trunk §` or
      `protocol-l3 §` citation fails (write the test first; it fails on the old default)
- [x] T005 `protocol/omgp-protocol.yaml`: add `l3_payloads.BP_SLOT_MAP` (replacing
      `opaque: true`) and `limits.bp_slot_map_max_slots: 232` (232, not the originally
      recommended 248 — recomputed against F10's corrected `LIMIT_max_l3_payload`, see the
      2026-09-21 note below) per `contracts/bp-slot-map.md`; update `docs/protocol-l3.md`
      §3.1's `BP_SLOT_MAP` table row in the same commit (CLAUDE.md golden rule 1); append the
      R-01 reconciliation entry to `docs/OPEN-QUESTIONS.md`; run `python3 tools/codegen.py`
- [x] T006 [P] Write `tests/unit/test_l3_bp_slot_map.cpp` (write first, fails to compile until
      T007): round-trip for `slot_count` 0, 1, 8, 232 (the cap — 248 before the 2026-09-21
      correction); `OutOfRange` for `slot_count` 233; `Truncated`/`LengthMismatch` per the
      shared decoder rules (`l3_payload.hpp`'s file header)
- [x] T007 [US-shared] Add `BpSlotMapResp` to `l3/l3_types.hpp` and
      `encode_bp_slot_map_resp`/`decode_bp_slot_map_resp` to `l3/l3_payload.hpp`/`.cpp` per
      `contracts/bp-slot-map.md` — make T006 pass
- [x] T008 [P] Write `tools/refimpl/test_l3.py` cases for `bp_slot_map` (write first) mirroring
      T006's cases in Python; implement the matching encode/decode pair in
      `tools/refimpl/omgp_l3.py` — make the new pytest cases pass (dual verification, constitution Principle III)
- [x] T009 Extend `tools/refimpl/genvectors.py` with `msg_bp_slot_map_resp_full_occupancy`
      (`slot_count = 232`, every bit set) and generate it **once** into `tests/vectors/` — also
      regenerated the two pre-existing opaque `msg_bp_slot_map_req`/`_resp` vectors, which the
      new typed codec made wrong (AC10, CLAUDE.md rule 9) — human-triggered commit stating the
      reason
- [x] T010 Extend `tools/diffcheck.py`'s existing L3 payload differential path to cover
      `bp_slot_map` (no `--frames`-style special case needed, R-01); confirm it runs inside the
      existing `diffcheck` stage time budget

**Note (2026-09-21, #676).** T005-T010 landed together in one PR/commit, not T005 alone: AC9
(green-tree obligation) required the codec (T007/T008) and vector regeneration (T009/AC10) in
the same commit as T005's YAML change, since the two pre-existing opaque `BP_SLOT_MAP` vectors
would otherwise fail the moment the codec stopped treating the opcode as opaque. T005's
`limits.bp_slot_map_max_slots` also moved from the originally recommended 248 to 232, and
T006/T009's slot_count boundary values from 248/249 to 232/233, reconciling with F10's
`LIMIT_max_l3_payload` split (`docs/OPEN-QUESTIONS.md` "#110 rulings", "BP_SLOT_MAP wire
format (R-01)").

**Checkpoint**: `BP_SLOT_MAP` has a real wire format, a working codec, a Python reference, a
golden vector and differential coverage — discovery (US1) can now be built.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: the types and test double every user story's tests depend on.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [ ] T011 [P] Write `core/core_types.hpp` per `data-model.md` §1-9 and §8a: `CoreStatus`,
      `DiscoveryState`, `NodeRecord`, `BackplaneRecord`, `DescriptorCacheEntry`,
      `LifecycleKind`/`LifecycleEvent`, `ParamRequestId`/`ParamResult`, the three demand-item
      structs (`EventDrainItem`/`DescChunkItem`/`ParamOpItem`), `ParamResultDelivery` (§8a, the
      pending-delivery ring element T014's test needs), a fixed-capacity `Ring<T, N>` helper,
      `SuperframeBudget`, `CoreCallbacks` — no logic, POD types and one small ring template only
- [ ] T012 [P] Write `tests/support/mock_l3_node.hpp`/`.cpp` per `contracts/mock-l3-node.md`
      (R-08): the step tables (`IdentifyStep`, `DescChunkStep`, `StatusStep`, `EventStep`,
      `ChannelStep`, `ParamStep`, `SlotMapStep`, `SilenceStep`, `ErrorStep`), a trivial
      in-memory `link::ByteWire` backing them via real `encode_frame`/`Deframer` (never a
      byte-level shortcut), scheduling via `advance_to(t)` exactly as F2's `MockWire` does
- [ ] T013 Write `tests/unit/test_mock_l3_node.cpp` (write first): scripts consumed in
      per-opcode order; an exhausted script answers steady-state (last step repeats, not
      silence-by-default); a `SilenceStep` produces a genuine `Master` timeout over a real wire
      — make it pass by completing T012 (mirrors F2's own `test_mock_wire.cpp`, verifying the
      test double itself before anything is built on top of it)
- [ ] T014 [P] Write `tests/unit/test_core_callback_queue.cpp` (write first, RED; `/speckit-analyze`
      finding I1): pending `LifecycleEvent`/`ParamResultDelivery` rings (data-model.md §8a) —
      enqueueing does not invoke either `CoreCallbacks` function; `drain_callbacks()` is the
      only thing that does; a full ring refuses the newest enqueue and increments
      `dropped_deliveries_` rather than evicting an older entry; `drain_callbacks(n)` invokes at
      most `n` callbacks and leaves the rest pending for the next call
- [ ] T015 Write `core/core_engine.hpp`/`.cpp`: `CoreEngine` skeleton — constructor owning a
      `link::Master` and `link::HealthTracker` (R-02), the fixed node/backplane/descriptor
      tables from T011 zero-initialised (`descriptors_[32]`, R-12), the pending-delivery rings
      and `drain_callbacks()` (data-model.md §8a — make T014 pass), `discovery_state()`/
      `node_in_use()` accessors, an `on_notice()` override that only compiles (body completed
      in US5, T042) so `CoreEngine` satisfies `link::HealthListener` from its first commit, and
      an empty `run_superframe()` that only drains `Master`'s receive path — add to
      `core/CMakeLists.txt` (converts it from `INTERFACE` to `STATIC`, mirroring T001's own note)
- [ ] T016 Add `core` to `CMakeLists.txt` (root)'s existing `omgp_add_catch_test` link set if
      T001 did not already cover every consumer; confirm `./pipeline.sh codegen quality build`
      is green with `omgp_core` compiled but empty of behaviour

**Checkpoint**: `CoreEngine` exists, links, and compiles against the real `link::Master` and
`link::HealthTracker` (R-02); `MockL3Node` is itself tested; the pending-delivery queue that
makes `run_superframe()` provably non-blocking on application code (spec FR-021) is in place
before any story that emits a callback is built on top of it. User story implementation can
begin.

---

## Phase 3: User Story 1 - A newly powered rig discovers itself (Priority: P1) 🎯 MVP

**Goal**: cold-boot discovery — every backplane enrolled, every occupied slot's module
identified and described, node IDs assigned, a deterministic transcript (spec SC-001).

**Independent Test**: `quickstart.md` §3 — `MockL3Node` scripts for 3 backplanes / 12 modules,
run `CoreEngine` from a cold start, assert every module reaches `DiscoveryState::Discovered`
and two runs produce byte-identical transcripts.

### Tests for User Story 1 ⚠️

- [ ] T017 [P] [US1] Write `tests/unit/test_core_discovery.cpp` (write first, RED): spec's four
      Acceptance Scenarios — full discovery of 3 backplanes/12 modules with matching
      transcripts across two runs (AS1, AS2), including a `[clock-granularity]`-tagged case
      that replays the same script through `run_superframe()` at a different `FakeClock` step
      size and asserts the same transcript (SC-005, distinct from AS2's same-cadence rerun); a
      slot occupied but never answering `IDENTIFY` does not block any other slot/backplane and
      is retried, not abandoned (AS3); a backplane attached after cold boot is discovered
      without re-running already-enrolled backplanes (AS4); `NodeDiscovered`/`NodeRemoved` are
      enqueued (not delivered) by discovery and slot changes, observed here via
      `drain_callbacks()` (US5's own `test_core_lifecycle.cpp`, T041, owns the full lifecycle
      contract — this file only needs enough to assert the transcript, per T018/T022 below)

### Implementation for User Story 1

- [ ] T018 [US1] Implement `CoreEngine::reconcile_slot_map()` (R-06): first-fit node-ID
      assignment from the shared `ADDR_module_min..max` pool in canonical order (ascending
      backplane address, then ascending slot index), enqueueing `CoreStatus::NoFreeNodeId`
      onto `pending_lifecycle_` on pool exhaustion rather than failing silently; releasing an
      id when its slot's `changed` bit reports newly-unoccupied — clears the freed
      `NodeRecord::descriptor` (decrementing that `DescriptorCacheEntry::refcount`, U1) and
      enqueues `LifecycleKind::NodeRemoved` — make part of T017 pass
- [ ] T019 [US1] Implement `CoreEngine::run_superframe()`'s status-poll and enrolment-probe
      halves (spec FR-001/FR-002/FR-003, trunk §6 order): one `GET_STATUS`/`BP_SLOT_MAP`
      alternating poll per enrolled backplane via `Master::begin`, `HealthTracker::
      poll_due`/`mark_polled`/`next_probe`/`on_result`/`tick` driven exactly per
      `contracts/core-cpp.md`'s "What this feature needs" obligations — make more of T017 pass
- [ ] T020 [US1] Implement `IDENTIFY` issuance for a freshly assigned node id and
      `IdentifyResp` handling: `Undiscovered → Identifying`, `ModelIdRec`+`desc_crc` recorded on
      `NodeRecord`
- [ ] T021 [US1] Implement descriptor-cache lookup (spec FR-008): on a fresh `IdentifyResp`,
      check `DescriptorCacheEntry` by `(model_vendor, model_hw_rev, model_fw_rev, desc_crc)` —
      a hit sets `NodeRecord::descriptor` (incrementing that entry's `refcount`, U1) and jumps
      straight to `Discovered` (SC-004); a miss queues the first `DescChunkItem`
- [ ] T022 [US1] Implement chunked `READ_DESC` (R-09, `max_len = 61`) as `DescChunkItem` demand
      items: reassembly into a fresh `DescriptorCacheEntry` (`refcount` set to 1 on completion,
      U1), `descriptor_crc()` check against the `IdentifyResp` value, `ReadingDescriptor →
      Discovered` on match, enqueueing `LifecycleKind::NodeDiscovered` (first time this node id
      has reached `Discovered`) or `NodeRediscovered` (it has before) onto `pending_lifecycle_`
      (C1: this and T018 are the only two emission points for spec FR-017's presence events —
      `data-model.md` §7's `LifecycleKind` comment) — make the rest of T017 pass
- [ ] T023 [US1] Add a test-only transcript hook to `CoreEngine` (one line per `Master::begin`
      call: superframe number, opcode, dst, node_id) gated so it costs nothing when unused,
      exposed to `test_core_discovery.cpp` for the determinism assertion (AS2)
- [ ] T024 [US1] Full `./pipeline.sh` + `./pipeline.sh esp32`; raise `UNIT_TEST_FLOOR`; local
      `mutate.sh --diff origin/main` triage

**Checkpoint**: User Story 1 fully functional and independently testable — a cold rig
discovers itself deterministically. This is the shippable MVP slice.

---

## Phase 4: User Story 2 - Status polling never stops for parameter traffic (Priority: P1)

**Goal**: demand-traffic scheduling (budget, carry-over, event-first priority) plus parameter
set/get, none of it able to starve status polling.

**Independent Test**: `quickstart.md` §4/§8 — a fully discovered rig, a 40-item parameter
burst, assert every superframe still polls every enrolled backplane first; separately, a
failed `SET_PARAM`/`GET_PARAM` is reported, and `GetParam` is asynchronous and correlated.

### Tests for User Story 2 ⚠️

- [ ] T025 [P] [US2] Write `tests/unit/test_core_scheduler.cpp` (write first, RED): spec's
      three Acceptance Scenarios — 40 queued parameter sets never delay a superframe's status
      polls (AS1); an oversized burst carries the unsent remainder to later superframes rather
      than extending the current one (AS2); a concurrent `event_pending` burst still drains
      inside User Story 4's bound while the parameter burst is in flight (AS3, ties to SC-002)
      — plus, from the 2026-09-21 hardening amendment: SC-007's measured-time case (a scripted
      backplane whose answers cost several times an honest poll gets its own modules' demand
      items demoted within a bounded number of superframes, `LifecycleKind::Demoted` reported,
      while every OTHER enrolled backplane keeps receiving its own status poll every superframe
      throughout — FR-002 unaffected)
- [ ] T026 [P] [US2] Write `tests/unit/test_core_params.cpp` (write first, RED): a scripted
      `ParamStep{ok:false,...}` makes a queued `set_param` produce
      `LifecycleKind::ParamSetFailed` with the scripted reason (spec FR-023); `get_param`
      returns a `ParamRequestId` immediately and `on_param_result` delivers the scripted value
      or failure tagged with that id later (FR-024); two outstanding `get_param` calls for the
      same `(node_id, param_id)` get two distinct ids (R-10)

### Implementation for User Story 2

- [ ] T027 [US2] Implement `CoreEngine::set_param()`/`get_param()`: queue a `ParamOpItem` onto
      `param_queue_`, `CoreStatus::QueueFull` refusal (not a silent drop) when the ring is at
      capacity; `get_param` additionally assigns/reuses a `ParamRequestId` (R-10) — make T026's
      queuing half pass
- [ ] T028 [US2] Implement `SuperframeBudget` accounting (R-11, amended 2026-09-21 per spec
      FR-027/FR-028): debit `budget_.remaining_us` by each scheduled transaction's cost
      estimate — that backplane's/node's own `last_measured_duration_us` (data-model.md §3/§4)
      once one exists, else the worst-case wire time at `master_.bit_rate()` as the seed (the
      same reasoning `link/health.cpp`'s `kOutcomeWindowUs` uses) — reset `remaining_us` at the
      start of every `run_superframe()` call; after each transaction completes, record its
      actual elapsed time (via `clock_`) into that target's `last_measured_duration_us`. Track
      `consecutive_overrun_superframes` per backplane/node against a computed fair share (the
      remaining demand budget divided by the number of targets currently competing for it);
      past a configured threshold set `demoted = true` and enqueue `LifecycleKind::Demoted`
      (`demoted_is_backplane` set for a backplane-level demotion) onto `pending_lifecycle_`;
      clear it and enqueue `DemotionCleared` once measured cost returns to normal. Never demotes
      a backplane's own status poll or the enrolment probe (FR-002/FR-003 stay unconditional) —
      only the demand-item priority of that backplane's modules, or of a demoted node itself.
- [ ] T029 [US2] Implement the demand-item drain loop (spec FR-004/FR-005/FR-020, R-07): after
      status polls and the enrolment probe, drain `event_queue_` then `desc_queue_` then
      `param_queue_`, one item at a time, stopping the moment an item would exceed
      `budget_.remaining_us` (carry-over, not truncation mid-item) — make T025 pass. Amended
      2026-09-21 (spec FR-025, FR-028): within each ring, an item whose node (or whose owning
      backplane) has `demoted == true` (T028) is skipped to the back of that ring for this
      superframe rather than drained in plain arrival order; `event_queue_` additionally drains
      at most one item per distinct `node_id` per superframe turn (K=1, T039 owns the rate
      tracking this enforces against) regardless of how many of that node's events are queued.
- [ ] T030 [US2] Implement parameter-result decoding: on a `ParamOpItem::Kind::Get`
      transaction's `Answered` event, decode `GetParamResp` or `ErrorResp` and call
      `on_param_result`; on a `Kind::Set` transaction's failure (an `ErrorResp`, or the
      transaction's own `Failed` event after `Master`'s retries), emit
      `LifecycleKind::ParamSetFailed` — make the rest of T026 pass
- [ ] T031 [US2] Wire descriptor-chunk and event-drain items (already queued by US1/US4's own
      logic once those land) through the same drain loop's budget accounting — confirm no
      regression in T017 (US1's transcript ordering: status polls and the probe still precede
      every demand item)
- [ ] T032 [US2] Full `./pipeline.sh` + `./pipeline.sh esp32`; raise `UNIT_TEST_FLOOR`; local
      mutation run

**Checkpoint**: User Stories 1 and 2 both independently functional — discovery plus a
scheduler that never starves status polling under load, with parameter operations fully
reported.

---

## Phase 5: User Story 3 - A channel switch completes and the application is told (Priority: P2)

**Goal**: accept-then-settle `SELECT_CHANNEL`, descriptor-declared timeout, `CHANNEL_SETTLED`
handling.

**Independent Test**: `quickstart.md` §5 — a module whose descriptor declares a settle time,
scripted to either settle or not; the application is told the outcome at the right time either
way.

### Tests for User Story 3 ⚠️

- [ ] T033 [P] [US3] Write `tests/unit/test_core_channel.cpp` (write first, RED): spec's three
      Acceptance Scenarios — immediate accept, completion reported only once `CHANNEL_SETTLED`
      is drained (AS1); settle-timeout elapsing with no event reports
      `ChannelSwitchTimedOut`, not indefinite silence (AS2); a second `select_channel` for a
      node already mid-switch is refused, not queued behind the first, and cannot corrupt or
      drop the first's outcome (AS3)

### Implementation for User Story 3

- [ ] T034 [US3] Implement `CoreEngine::select_channel()`: `SELECT_CHANNEL` issuance,
      `NodeRecord::switch_outstanding`/`switch_channel`/`switch_deadline_us` set from the
      cached descriptor's `SwitchingRec::settle_ms` (spec FR-011/FR-012); refuses
      (`CoreStatus`) rather than queuing a second request while one is outstanding for the same
      node — make part of T033 pass
- [ ] T035 [US3] Implement settle-timeout detection inside `run_superframe()` (spec FR-013):
      compare `now_us` against every outstanding `switch_deadline_us`, emit
      `LifecycleKind::ChannelSwitchTimedOut` exactly once when it elapses with no
      `CHANNEL_SETTLED` drained — make more of T033 pass
- [ ] T036 [US3] Implement `CHANNEL_SETTLED` handling inside the event-drain path (T029): a
      drained `l3::GetEventResp` with `event_type == CHANNEL_SETTLED` for a node with
      `switch_outstanding` clears it and emits `LifecycleKind::ChannelSettled` instead of the
      generic `ModuleEvent` — make the rest of T033 pass
- [ ] T037 [US3] Full `./pipeline.sh` + `./pipeline.sh esp32`; raise `UNIT_TEST_FLOOR`; local
      mutation run

**Checkpoint**: User Stories 1-3 independently functional.

---

## Phase 6: User Story 4 - An event on a module reaches the application within a bounded time (Priority: P2)

**Goal**: `event_pending`-driven draining, delivered within (backplane poll period + 2 ×
`TRUNK_T_poll_us`), in order, even under sustained event generation.

**Independent Test**: `quickstart.md` §6 — a node raises an event at a known simulated instant;
measure simulated time to callback delivery against the calculable bound.

### Tests for User Story 4 ⚠️

- [ ] T038 [P] [US4] Write `tests/unit/test_core_events.cpp` (write first, RED): spec's three
      Acceptance Scenarios — a nonzero `event_pending` count drains fully within the bound
      (AS1); multiple queued events for one node are delivered in module-queued order (AS2); a
      node that queues events faster than one drain cycle empties them is not abandoned —
      draining continues across superframes under the demand budget (AS3); a
      `[concurrent-load]`-tagged case shared with `test_core_scheduler.cpp` (T025 AS3) for
      SC-002 under a simultaneous parameter burst — plus, from the 2026-09-21 hardening
      amendment (spec FR-025/FR-026, SC-006): a node whose status poll always reports
      `event_pending > 0` is drained no faster than one event per its own superframe turn
      regardless of the `remaining_count` it claims, and never delays any OTHER node's own
      pending event past the SC-002 bound; once its measured delivery rate exceeds the
      configured budget it is reported `LifecycleKind::NodeEventFault` and stops being drained
      until its rate recovers, reported `NodeEventFaultCleared`

### Implementation for User Story 4

- [ ] T039 [US4] Implement `event_pending` tracking from every status poll's decoded
      `StatusBlock` and `EventDrainItem` enqueueing (spec FR-015); `GET_EVENT` issuance and
      `GetEventResp` decode in the drain loop (T029), delivering
      `LifecycleKind::ModuleEvent` per drained event, re-enqueueing while
      `remaining_count > 0` (subject to T029's K=1-per-turn cap) — make T038 pass. Amended
      2026-09-21 (spec FR-025/FR-026): increment `events_drained_this_window` on every
      delivered `ModuleEvent`; once `event_rate_window_start_us` (data-model.md §3) has elapsed
      a configured window, compare the count against a configured rate budget — over budget
      sets `event_faulted = true`, enqueues `LifecycleKind::NodeEventFault`, and the drain loop
      (T029) skips that node's `EventDrainItem`s entirely until a later window's count falls
      back under budget, which clears the flag and enqueues `NodeEventFaultCleared`.
- [ ] T040 [US4] Full `./pipeline.sh` + `./pipeline.sh esp32`; raise `UNIT_TEST_FLOOR`; local
      mutation run; confirm the SC-002 latency assertion holds in both the isolated and
      concurrent-load cases

**Checkpoint**: User Stories 1-4 independently functional.

---

## Phase 7: User Story 5 - The application always knows which nodes are live (Priority: P3)

**Goal**: presence (discovered/removed/rediscovered), derived module liveness
(SUSPECT/OFFLINE/RECOVERED), and forwarded backplane/bus health (R-03) — all through the same
lifecycle callback.

**Independent Test**: `quickstart.md` §7 — discovery → removal → rediscovery in order;
separately, a module gone quiet without its slot emptying is reported unreachable and later
recovered; a whole backplane going down is reported distinctly from any one node's own
transition.

### Tests for User Story 5 ⚠️

- [ ] T041 [P] [US5] Write `tests/unit/test_core_lifecycle.cpp` (write first, RED): spec's six
      Acceptance Scenarios — discovery reports live (AS1); a slot-reported removal reports gone
      (AS2); rediscovery reuses the same reporting path (AS3); a module that stops answering
      without its slot emptying reports `NodeSuspect` then `NodeOffline` at the same
      failure-count/elapsed-time bounds `HealthTracker`'s own symbols name (AS4); it recovers
      on the next good answer (AS5); every enrolled node on one backplane going quiet at once
      reports a bus fault, distinct from any single node's transition, and again on clear (AS6)

### Implementation for User Story 5

- [ ] T042 [US5] Complete `CoreEngine::on_notice()` (R-03): forward every
      `link::Notice` for a backplane address into the matching `LifecycleKind` (`Backplane*`,
      `BusFault`/`BusRecovered`, addr 0), and cascade every `NodeRecord` behind that backplane
      into an unreachable state immediately, independent of its own `consecutive_failures` —
      make part of T041 pass
- [ ] T043 [US5] Implement per-module liveness derivation (R-03): `consecutive_failures`
      counted from status-poll/demand-item outcomes addressed to that module, `NodeSuspect`
      at `TRUNK_suspect_after_failures`, `NodeOffline` after `TRUNK_offline_after_suspect_ms`
      of simulated time, `NodeRecovered` on the next valid answer, symbols reused from the
      generated header (not restated) — make the rest of T041 pass
- [ ] T044 [US5] Full `./pipeline.sh` + `./pipeline.sh esp32`; raise `UNIT_TEST_FLOOR`; local
      mutation run

**Checkpoint**: all five user stories independently functional. Feature complete.

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: closing items that span every story, mirroring
`specs/002-trunk-link-layer/tasks.md`'s own T046-T051.

- [ ] T045 [P] Complete `esp32-host/main/core_smoke.cpp`: references `CoreEngine`,
      `link::Master`, `link::HealthTracker` so every translation unit of `omgp_core` links on
      Xtensa; `./pipeline.sh esp32` green
- [ ] T046 [P] Write `core/README.md`: purpose, the portable-subset constraints and their
      enforcement, the superframe/demand-budget model in one paragraph, API pointer to
      `specs/003-host-core-engine/contracts/core-cpp.md`, and the R-02/R-03 architecture notes
      summarised (mirrors `link/README.md`'s own shape)
- [ ] T047 [P] Run `clang-format -i` over `core/ tests/unit/test_core_*.cpp
      tests/support/mock_l3_node.*`; `./pipeline.sh quality` clean including clang-tidy over
      `core/`
- [ ] T048 Execute every step of `specs/003-host-core-engine/quickstart.md` on the cmake path;
      record the actual outputs of its discriminating checks (§2, §4) in the PR body
- [ ] T049 Final `UNIT_TEST_FLOOR` raise; append to `docs/OPEN-QUESTIONS.md` any assumption
      implementation turned into a question (append-only; e.g. the free-pool node-ID
      exhaustion policy, R-06) with recommendation + "Ruling: pending"

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: no dependencies — can start immediately. `BP_SLOT_MAP` (T005-T010) must
  finish before User Story 1's `reconcile_slot_map()` (T018) reads it.
- **Foundational (Phase 2)**: depends on Setup completion — BLOCKS all user stories.
- **User Stories (Phase 3-7)**: all depend on Foundational; in priority order P1(US1)/
  P1(US2)/P2(US3)/P2(US4)/P3(US5). US2-US5 each read `CoreEngine`'s shared `run_superframe()`
  drain loop (T029), which US1 does not touch but US2 builds — **US2's T027-T030 should land
  before US3/US4/US5's own implementation tasks**, even though all five stories' *tests* can be
  written the moment Foundational is done. This is the one real cross-story ordering
  constraint; every other pairing is independent.
- **Polish (Phase 8)**: depends on every user story targeted for this delivery being complete.

### Within Each User Story

- Tests MUST be written and FAIL before implementation (constitution Principle II).
- Node-ID/discovery state before scheduling; scheduling before channel/event/lifecycle logic
  that reads it.
- Story complete (its own closing pipeline task) before moving to the next priority, unless
  staffed in parallel.

### Parallel Opportunities

- T002, T003, T004 (Setup) — different files, no shared dependency.
- T006 and T008 (Phase 1) — the C++ test and the Python reference test, both written before
  their respective implementations, in different languages/files.
- T011 and T012 (Foundational) — types header and test double, different files. T014 needs
  T011 (the types it exercises) but not T012, so it can run alongside T012 once T011 lands.
- T017 (US1 test), T025+T026 (US2 tests), T033 (US3 test), T038 (US4 test), T041 (US5 test) —
  once Foundational is done, every story's own test file can be written in parallel; only the
  *implementation* ordering note above applies.

---

## Parallel Example: Phase 1 Setup

```bash
# Launch independent Setup tasks together:
Task: "Extend the bootstrap g++ path in pipeline.sh for core/*.cpp"
Task: "Create esp32-host/components/omgp_core/CMakeLists.txt + placeholder core_smoke.cpp"
Task: "Change tools/check_embedded.py --cite-dirs to include core"
```

## Parallel Example: User Story tests, once Foundational is done

```bash
Task: "Write tests/unit/test_core_discovery.cpp (US1)"
Task: "Write tests/unit/test_core_scheduler.cpp (US2)"
Task: "Write tests/unit/test_core_params.cpp (US2)"
Task: "Write tests/unit/test_core_channel.cpp (US3)"
Task: "Write tests/unit/test_core_events.cpp (US4)"
Task: "Write tests/unit/test_core_lifecycle.cpp (US5)"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (`BP_SLOT_MAP` must exist before discovery can read it)
2. Complete Phase 2: Foundational (CRITICAL — blocks all stories)
3. Complete Phase 3: User Story 1
4. **STOP and VALIDATE**: `quickstart.md` §3 — a cold rig discovers itself deterministically
5. This is spec.md's own MVP framing ("nothing else in this feature has anything to control
   until discovery has found it")

### Incremental Delivery

1. Setup + Foundational → foundation ready, `BP_SLOT_MAP` real
2. + User Story 1 → discovery works → demo-able (MVP)
3. + User Story 2 → scheduling never starves under load, parameters fully reported
4. + User Story 3 → channel switching completes and is reported
5. + User Story 4 → bounded event latency, even under concurrent load
6. + User Story 5 → full lifecycle picture, including derived module liveness and forwarded
   backplane/bus health
7. + Polish → both builds green, `core/README.md`, quickstart recorded

### Parallel Team Strategy

Once Foundational (Phase 2) is done, US3/US4/US5's *test* tasks (T033, T038, T041) can be
written by separate people immediately; their *implementation* tasks should wait on US2's
T027-T030 landing in `run_superframe()`'s shared drain loop, per the ordering note above —
otherwise two people editing the same drain loop for different stories at once is the one real
merge-conflict risk this feature has.

## Notes

- [P] tasks = different files, no dependencies on incomplete tasks.
- [Story] label maps task to specific user story for traceability.
- R-NN references are `research.md` decisions; AS/FR/SC references are `spec.md`.
- Commit after each task or logical (RED, then GREEN) group, per this repo's established
  pattern (`test(...)`, then `fix(...)`/`feat(...)` commits).
- Verify tests fail before implementing.
- Hardening amendment (2026-09-21): T025, T028, T029, T038 and T039 were amended in place to
  fold in two pre-existing human-ruled findings this feature's first draft did not carry
  forward (#159 bounded event drain, #162 measured-time superframe budgeting — both
  `docs/OPEN-QUESTIONS.md` "#110 F6"/"#110 F2c", human-ruled 2026-09-06). No task IDs were
  added or renumbered, to avoid invalidating the dependency/cross-reference numbering already
  established on the filed GitHub issues (#672-#720) and `tools/tasks-to-issues.py`'s `T\d+`
  format requirement. `Closes #159` (event-rate/bounded-drain) belongs on whichever PR lands
  T039; `Closes #162` (measured-time budget/demotion) belongs on whichever PR lands T028/T029.
