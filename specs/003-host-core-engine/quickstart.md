# Quickstart: proving the host-core control engine works (feature 003)

Runnable validation scenarios, one per user story. Each states what the output would be if
the claim were false (CLAUDE.md working agreement). Prerequisites: features 001 and 002's own
(`specs/002-trunk-link-layer/quickstart.md` §prerequisites), unchanged.

## 1. Build + every unit test in one invocation (all stories)

```bash
./pipeline.sh codegen quality build unit
```
Expected: `unit: executed <n> check(s)` above the raised `UNIT_TEST_FLOOR`, `test_core_*`
binaries listed by ctest alongside the existing `test_link_*`/`test_l3_*` ones, `==> pipeline
green`. If `core/` were not part of the build, `test_core_*` would be absent and the floor
would fail.

## 2. `BP_SLOT_MAP`'s new payload round-trips (R-01, prerequisite for US1)

```bash
python3 tools/diffcheck.py                            # includes the new bp_slot_map_* vectors
```
Expected: the L3 payload differential line reports the new vectors byte-identical both ways.
**Discriminating check** (do not commit): in `l3/l3_payload.cpp`, off-by-one the bitmap length
computation (`ceil(slot_count/8)` → `slot_count/8`) → `decode_bp_slot_map_resp` reports
`LengthMismatch` on `msg_bp_slot_map_resp` (`slot_count = 4`; `ceil(4/8) = 1` byte per bitmap,
but plain integer division gives 0 — a slot_count that is *not* a multiple of 8 is required for
this to discriminate at all, unlike the full-occupancy vector's `slot_count = 232`, exactly
`8 × 29`), diffcheck fails naming the vector; restore → passes.

## 3. A newly powered rig discovers itself (US1)

```bash
./build/native/test_core_discovery -s
```
Expected, driving `MockL3Node` scripts for 3 backplanes totalling 12 modules from a cold
`CoreEngine`: every backplane reaches `link::HealthState::ENROLLED`, every occupied slot gets
a node id, every node reaches `DiscoveryState::Discovered`, and the recorded operation
transcript (one line per `Master::begin` call: opcode, dst, node_id, superframe number) is
byte-identical across two runs from the same script. **Discriminating check**: script one
slot's module to never answer `IDENTIFY` (`SilenceStep`) → that node stays `Identifying`
forever, but the other 11 still reach `Discovered` and the run still terminates (no hang).

## 4. Status polling never stops for parameter traffic (US2, SC-002/SC-003)

```bash
./build/native/test_core_scheduler -s
```
Expected: with a fully discovered rig, 40 queued `set_param` calls, every superframe from
submission to completion still calls `Master::begin` for every enrolled backplane's status
poll before any parameter-queue item (transcript order asserted, not just eventual delivery);
the burst's last item completes after `ceil(40 / items-per-superframe)` superframes, never in
fewer (budget honoured) and never stalling past that (no starvation). **Discriminating check**:
swap the demand-drain order in `core_engine.cpp` to param-queue-first → a concurrent
`EventDrainItem` scripted for the same superframes is delayed past `backplane poll period + 2
× TRUNK_T_poll_us`, and the SC-002 latency assertion in this same binary fails naming the
event's actual vs. bound latency.

**SC-007 (2026-09-21 hardening amendment)**: a backplane scripted to answer every status poll
costing several times an honest poll's duration (padded `ERROR.detail`) has its own modules'
demand items demoted within a bounded number of superframes — `LifecycleKind::Demoted`
delivered, `demoted_is_backplane` set — while every OTHER enrolled backplane's own status poll
still appears in every superframe's transcript throughout, unaffected. **Discriminating
check**: hardcode the worst-case-bound estimate instead of using
`last_measured_duration_us` → the demotion never fires (or fires against the wrong target),
and the transcript assertion naming which backplane got demoted, and by which superframe,
fails.

## 5. A channel switch completes and the application is told (US3)

```bash
./build/native/test_core_channel -s
```
Expected: `select_channel` queues immediately; a `ChannelStep{settles: true, ...}` script
delivers `LifecycleKind::ChannelSettled` at the scripted delay; a `ChannelStep{settles:
false}` script delivers `LifecycleKind::ChannelSwitchTimedOut` at exactly the descriptor's
`SwitchingRec::settle_ms`, not before and not after; a second `select_channel` for the same
node while one is outstanding returns `CoreStatus` refused, not queued.

## 6. An event reaches the application within a bounded time (US4, SC-002)

```bash
./build/native/test_core_events -s
```
Expected: a `StatusStep` reporting `event_pending > 0` followed by `EventStep`s is fully
drained (`NONE` reached) within `(backplane poll period) + 2 × TRUNK_T_poll_us` of simulated
time in every case the binary drives, including the SC-002 "with concurrent parameter-set
traffic" case (shared with #4's binary via a tagged `[concurrent-load]` case).

**SC-006 (2026-09-21 hardening amendment)**: a node scripted to always report `event_pending
> 0` (a flood) is drained no faster than one event per its own superframe turn regardless of
`remaining_count`, and a second, well-behaved node's own single event still lands inside the
SC-002 bound in the same run; once the flooding node's measured delivery rate exceeds budget,
`LifecycleKind::NodeEventFault` is delivered and no further `Master::begin` calls target that
node's `GET_EVENT` until a later window brings its rate back down. **Discriminating check**:
remove the K=1 cap (drain a node's whole backlog per turn) → the well-behaved node's event
latency assertion fails once the flooding node's script is added to the same run.

## 7. The application always knows which nodes are live (US5, R-03)

```bash
./build/native/test_core_lifecycle -s
```
Expected: `NodeDiscovered` → (slot map `changed` bit clears occupancy) → `NodeRemoved` →
(occupied again) → `NodeRediscovered`, in that order; separately, a node scripted `Silence`
on its own status-level demand items (not its slot) produces `NodeSuspect` then `NodeOffline`
at the same failure-count/elapsed-time bounds `TRUNK_suspect_after_failures`/
`TRUNK_offline_after_suspect_ms` name, then `NodeRecovered` on the next scripted answer;
a whole backplane's nodes going quiet at the L2 level (scripted through `MockL3Node`'s
backplane-level silence, not per-module) produces `BackplaneSuspect`/`BackplaneOffline` and,
once every enrolled backplane is down, `BusFault` — each reported exactly once
(`link::HealthListener`'s own SC-006 guarantee, forwarded verbatim per R-03).

## 8. Parameter failures and `GetParam` are reported, not dropped (spec FR-023/FR-024)

```bash
./build/native/test_core_params -s
```
Expected: a `ParamStep{ok: false, error_code: ...}` script makes a queued `set_param`
produce `LifecycleKind::ParamSetFailed` with that code; `get_param` returns a
`ParamRequestId` immediately and `on_param_result` later delivers the scripted value (or
failure) tagged with that same id — two outstanding `get_param` calls for the same
`(node_id, param_id)` are delivered with two distinct ids, each to its own caller-tracked
slot in the test.

## 9. Both builds green, embedded-path scan covers `core/` (CLAUDE.md rules 5/10)

```bash
./pipeline.sh                                        # all default stages
./pipeline.sh esp32                                  # IDF component omgp_core links core_smoke.cpp
python3 tools/check_embedded.py                      # --cite-dirs gains `core`
```
Expected: `==> pipeline green` twice, no `check_embedded` findings. Add a bare `200` where a
`TRUNK_T_resp_us`-derived value belongs in `core/` → `core/*.cpp:<n>: protocol literal 200
(TRUNK_T_resp_us)`; remove a `// trunk §` / `// protocol-l3 §` citation from a `core/` file →
`no spec citation`.
