# Contract: `core/` C++ API

Embedded path (CLAUDE.md rule 5): C++17, `-fno-exceptions -fno-rtti`, no heap after init.
Depends on `link/master.hpp`, `link/health.hpp`, `link/clock.hpp`, `link/link_types.hpp`,
`l3/*.hpp`, and the generated protocol header only (`research.md` R-02) — never
`link/responder.hpp`, `link/frame.hpp`, `link/byte_wire.hpp` directly, never `sim/`, `cli/`, or
a platform header.

## Types (`core_types.hpp`)

Every struct in `data-model.md` (§2-10), plus:

```cpp
enum class CoreStatus : uint8_t {
    Ok = 0,
    NoFreeNodeId,     // R-06 pool exhausted for a newly-occupied slot
    QueueFull,        // a demand-item ring (R-07) had no room; caller's operation was refused,
                       // nothing partially queued
    NotDiscovered,    // a parameter/channel operation named a node_id not yet Discovered
    RequestIdReused,  // GetParam called with the parameter FIFO already at capacity (R-10)
    InvalidValue,     // added 2026-10-08 (T027): a SetParam value above LIMIT_param_value_max,
                       // refused at the API boundary with nothing queued. See
                       // docs/OPEN-QUESTIONS.md 2026-10-08 — the block above had no status for
                       // a value the protocol cannot carry, and the only alternative report
                       // channel was FR-023's ParamSetFailed at issue time.
};
```

## Engine (`core_engine.hpp`)

```cpp
class CoreEngine {
  public:
    // wire/host_addr construct the link::Master this engine owns internally (R-02); callbacks
    // is copied by value (R-04) and must outlive every call below.
    CoreEngine(link::ByteWire& wire, Clock& clock, uint8_t host_addr, CoreCallbacks callbacks);

    // Runs exactly one superframe (spec FR-001): drains link::Master's receive path, issues
    // this superframe's status polls (FR-002) and one enrolment probe (FR-003) — both
    // unconditional, never skipped or reduced by demotion (FR-028) — then demand items up to
    // the remaining budget in event/desc-chunk/param order (FR-020, R-07), each node's event
    // drain capped at one per its own turn (FR-025) with its delivery rate tracked (FR-026),
    // and finally advances link::HealthTracker::tick(). The remaining demand budget itself is
    // computed from this superframe's own measured status-poll/probe durations, not an assumed
    // bound (FR-027); a backplane or node whose measured cost stays high across several
    // superframes has its own demand-item priority reduced and reported (FR-028, never its
    // mandatory status poll or the probe). Called once per TRUNK_T_poll_us of simulated time by
    // the caller's own loop — this engine does not own or read a wall clock (CLAUDE.md rule 3);
    // the caller decides cadence, this engine only assumes it is called often enough that no
    // superframe is skipped (an assumption, not enforced — mirrors F3 obligation 2 in
    // contracts/link-cpp.md, one layer up). Never invokes a callback itself (spec FR-021, R-04
    // correction) — outcomes are enqueued onto the pending-delivery rings (data-model.md §8a);
    // only drain_callbacks() below calls back into the application.
    void run_superframe(uint64_t now_us);

    // Invokes at most `max_deliveries` queued CoreCallbacks calls (default: everything
    // currently pending). The ONLY place either callback is ever invoked — never from
    // run_superframe() — which is what makes "the scheduler proceeds regardless of how long
    // the application takes" true by construction (spec FR-021; data-model.md §8a; R-04).
    // Call it as often as the application wants delivery latency to be low; skipping it for
    // several superframes just lets the pending rings fill (and, once full, refuse the
    // newest arrival and count it — never silently evict an older notice).
    void drain_callbacks(size_t max_deliveries = SIZE_MAX);

    // Queues a parameter set; fails closed (CoreStatus, not queued) rather than silently
    // dropping if the parameter FIFO (R-07) is full. Failure of the transaction itself, once
    // queued, is reported via CoreCallbacks::on_lifecycle (LifecycleKind::ParamSetFailed,
    // spec FR-023), delivered on a later drain_callbacks() call — this return value is only
    // about whether it was accepted for scheduling.
    CoreStatus set_param(uint8_t node_id, uint8_t param_id, uint8_t scope, uint16_t value);

    // Queues a parameter get; returns the request id through `out_id` on CoreStatus::Ok. The
    // value or failure (spec FR-023/FR-024) arrives later via
    // CoreCallbacks::on_param_result, tagged with that same id (R-10).
    CoreStatus get_param(uint8_t node_id, uint8_t param_id, uint8_t scope, ParamRequestId& out_id);

    // Requests a channel switch (spec FR-011/FR-012/FR-013): accepted immediately (queued as a
    // demand item) or refused (CoreStatus, nothing changes) if a switch is already outstanding
    // for this node (spec Assumption: at most one per node; the second request is refused, not
    // silently queued behind the first — the queueing described in the spec's Assumptions is
    // this refusal plus the caller's own retry, not internal buffering).
    CoreStatus select_channel(uint8_t node_id, uint8_t channel);

    // Read-only introspection for tests and a host application that wants a snapshot beyond
    // the callback stream (not a substitute for it — spec User Story 5 is push-based).
    DiscoveryState discovery_state(uint8_t node_id) const;
    bool node_in_use(uint8_t node_id) const;

  private:
    link::Master master_;
    link::HealthTracker health_;   // this engine IS the link::HealthListener (R-03)
    Clock& clock_;
    CoreCallbacks callbacks_;

    NodeRecord nodes_[LIMIT_max_nodes];
    BackplaneRecord backplanes_[ADDR_backplane_max - ADDR_backplane_min + 1];
    DescriptorCacheEntry descriptors_[32];  // R-12

    // R-07 demand rings, drained in this fixed order:
    Ring<EventDrainItem, LIMIT_max_nodes> event_queue_;
    Ring<DescChunkItem, LIMIT_max_nodes> desc_queue_;
    Ring<ParamOpItem, LIMIT_max_nodes> param_queue_;

    // Pending-delivery rings (data-model.md §8a, R-04 correction): run_superframe() enqueues,
    // drain_callbacks() is the only reader.
    Ring<LifecycleEvent, LIMIT_max_nodes> pending_lifecycle_;
    Ring<ParamResultDelivery, LIMIT_max_nodes> pending_param_results_;
    uint32_t dropped_deliveries_ = 0;

    SuperframeBudget budget_;

    // The superframe the plan now being executed IS (trunk §6): 0 before the first plan opens,
    // incremented by open_superframe(), so the first plan is superframe 1. A COUNT OF PLANS, not
    // a function of now_us — no Clock read and no wall-clock access contributes to it (CLAUDE.md
    // rule 3), which is what makes the transcript below insensitive to the caller's step size
    // (spec SC-005).
    uint32_t superframe_ = 0;

    // The operation transcript's sink (T023, below). Null unless a test installs one.
    TranscriptFn transcript_ = nullptr;
    void* transcript_ctx_ = nullptr;

    // link::HealthListener:
    void on_notice(link::Notice notice, uint8_t addr) override;  // R-03: forwards
                                                                   // backplane/bus notices,
                                                                   // degrades every node behind
                                                                   // `addr` immediately
};
```

## Node-ID assignment (R-06)

```cpp
// Called once per BP_SLOT_MAP response, for every slot whose `changed` bit is set (data-model
// §10). Newly occupied: assigns the next free id in ADDR_module_min..max, canonical order
// (ascending backplane address, then ascending slot index) — CoreStatus::NoFreeNodeId
// enqueued onto pending_lifecycle_ if the pool is exhausted, never silently dropped; nothing
// is reported as NodeDiscovered here (spec User Story 5 AS1: "fully identified and
// described", not merely present — that is T021's job, on reaching DiscoveryState::Discovered,
// as NodeDiscovered or NodeRediscovered depending on whether this node id has ever reached
// Discovered before). Newly vacated: frees the slot's id back to the pool, clears the freed
// NodeRecord's descriptor pointer (decrementing that DescriptorCacheEntry's refcount,
// data-model.md §5), and enqueues NodeRemoved onto pending_lifecycle_ — always, whether or
// not that node had reached Discovered yet.
void reconcile_slot_map(uint8_t backplane_addr, const l3::BpSlotMapResp& resp, uint64_t now_us);
```

## Operation transcript (test-only observation, tasks.md T023)

```cpp
// One line of the transcript: the four fields tasks.md T023 fixes, and no fifth.
struct TranscriptEntry {
    uint32_t superframe;  // the plan this request belonged to (superframe_ above)
    uint8_t opcode;       // the L3 opcode (protocol-l3 §3.1)
    uint8_t dst;          // the trunk address the frame went to (trunk §5: a backplane, always)
    uint8_t node_id;      // the L3 node id inside it (protocol-l3 §8)
};

// The sink: a plain function pointer plus one opaque context — the shape R-04 chose for
// CoreCallbacks, for the same reasons (no std::function, no virtual, nothing to construct).
using TranscriptFn = void (*)(void* ctx, const TranscriptEntry& entry);

// Declared in core/core_engine.hpp and befriended there; DEFINED ONLY by a test binary, which is
// the sole way to install a sink. There is no public setter.
struct CoreEngineTestSeam;
```

What the transcript is for: spec SC-001/AS2 ("two runs of the same scripted scenario produce an
identical sequence of operations") and SC-005 ("the same sequence however coarsely the simulated
clock is advanced") are claims about the ORDER of transactions, which no end-state assertion can
check. Its rules, each labelled per CLAUDE.md rule 11:

1. **One line per request this engine issues, in issue order, recorded at the `core/` call
   site.** `begin_request()` (`core/core_engine.cpp`) is this engine's ONLY caller of
   `link::Master::begin`, and the sink is invoked there — *proved by construction* (single call
   site; `link/` is not modified and records nothing, so the L2/L3 opacity invariant holds).
   *Demonstrated by* `begin_request: the transmitted L3 message is the header and payload
   verbatim…` and the whole-sequence comparisons in `tests/unit/test_core_discovery.cpp`.

2. **A `begin()` that is REFUSED is not recorded** — neither one `link::Master` refuses (`Busy`
   on an open transaction, `PayloadTooLong`, `ReservedAddress`) nor one this engine's own L3
   encoder refuses before reaching `Master`. The transcript is a record of requests *issued*, not
   of calls attempted. Two reasons, both load-bearing: a `TranscriptEntry` carries no `Status`
   (four fields, rule above), so a refused line would be indistinguishable from an issued one and
   would weaken rather than strengthen the sequence comparison; and whether a given call is
   refused is a function of wire state at that instant, so recording refusals would make the
   transcript depend on the caller's step size — exactly what SC-005 denies. A `begin()` the
   `Master` *accepts* but defers to honour `T_gap` IS recorded, at the accepting call, because it
   is an issued transaction (`link/master.hpp:66`: `busy()` is already true). *Demonstrated by*
   `begin_request: a request the link refuses (transaction open) is not recorded as issued` and
   the payload-limit half of the test named in 1. *Measured, not assumed, and only for SC-005*:
   a recorder that emitted per transmission instead of per transaction was patched into
   `run_superframe()` and failed SC-005 — the step-size criterion this second reason rests on —
   as well as AS1 and the test in 3 (#694, PR body). AS2 did not fail and could not: it replays
   one script at one cadence twice, so a per-transmission recorder adds the same extra lines to
   both transcripts. The first reason (no `Status` field) is an argument from the four-field
   entry, not a measurement.
   This is the ruling on issue #694's open AC3, taken on 2026-10-07 against the shipped,
   tested behaviour; the alternative (record refusals too) was rejected for the two reasons above
   and would need a fifth field to be meaningful, which is a tasks.md amendment.

3. **An L2 retry of an already-begun transaction adds no further line.** trunk §7's retry
   re-sends the same seq from inside `link::Master` (`fire_pending()`); `core/` neither sees nor
   initiates it, and `master_.busy()` keeps this engine from beginning anything while the
   transaction is open. *Proved by construction* (the retransmission path contains no `core/`
   code), and *demonstrated by* `trunk §7: an L2 retry of an already-begun transaction adds no
   further transcript line`, which pins `Master::attempts() == 2` and two request frames at the
   double against exactly one transcript line.

4. **Cost when unused: two null pointers and one null test per `begin_request()`** — stated
   precisely rather than as "nothing". This is the non-owning-sink mechanism, not the
   compile-time gate T023 allowed as an equal alternative; `#ifdef` was rejected so that the
   native and ESP-IDF builds compile the identical `core/` sources (CLAUDE.md rule 10). *Proved
   by construction*: the sink is null at construction, there is no public setter, and a build
   that never defines `CoreEngineTestSeam` has no way to install one — a friend declaration emits
   no code.

5. **`core/` holds no transcript storage at all.** The entry is a stack temporary handed to the
   sink; the buffer belongs to whoever installs one. So there is no fixed-capacity ring here and
   `data-model.md` §8a's drop-newest rule does not apply to the transcript — §8a governs the two
   pending-delivery rings, which are `core/` state. The only buffer that exists is the
   `std::vector<TranscriptEntry>` in `tests/unit/test_core_discovery.cpp`, host-only code where
   the full language is allowed (CLAUDE.md rule 5). *Consequence, stated rather than guarded*:
   nothing in `core/` bounds what a sink retains, so a sink on the embedded path would have to
   bound itself. No production path installs one today — that is a property of the repo's current
   contents, a control and not a guarantee.

6. **Observation only: `run_superframe()` schedules the same transactions in the same order with
   and without a sink installed.** *Proved by construction*: the sink is invoked after
   `master_.begin()` has already returned `Ok`, it is passed values the engine had already
   computed, no `begin()` call exists in order to be transcribed, and nothing on the path reads
   the sink's state or the `Clock`; the `SuperframeBudget` debit stays in `complete_request()`,
   untouched. *Demonstrated by* every other case in the discovery suite, which installs a sink
   and asserts the engine's own behaviour through it.

## What this feature needs from `link/` and `l3/` (interface note, mirrors `link-cpp.md`'s own "What F3/F4 need")

`link::Master::begin/poll/busy/attempts/set_bit_rate/stats/bus_stats`; `link::HealthTracker::
poll_due/next_probe/on_result/tick/mark_polled/bus_fault/bit_rate/set_bit_rate` and its
`HealthListener` (which `CoreEngine` implements, R-03); `link::Clock`. From `l3/`: every
encoder/decoder in `l3_header.hpp`, `l3_payload.hpp` and `l3_descriptor.hpp` this feature's
opcodes use (`IDENTIFY`, `READ_DESC`, `SELECT_CHANNEL`, `SET_PARAM`, `GET_PARAM`, `GET_STATUS`,
`GET_EVENT`, and the new `BP_SLOT_MAP` pair from R-01/`bp-slot-map.md`), plus
`descriptor_crc`/`RecordCursor`/the typed `decode_*` functions for descriptor caching. Nothing
from `link/responder.hpp`, `link/frame.hpp`, or `link/byte_wire.hpp` directly (R-02) — those
are reached only through `Master`.

Three obligations this engine keeps on itself, the same shape as `link-cpp.md`'s "What F3/F4
need" obligations one layer down: (1) while `health_.bus_fault()`, issue only what
`health_.next_probe()` returns — no status polls (`poll_due()` is already false), no demand
items; (2) call `health_.next_probe()` and `health_.mark_polled()` exactly once per probe/poll
actually issued, never speculatively; (3) any rate change goes to both `master_.set_bit_rate()`
and `health_.set_bit_rate()` in the same step (this feature does not itself expose a rate
selector to its own caller in v1 — out of scope, spec input's "routing policy" boundary reads
as "no rate policy either" absent a stated requirement — but the obligation is recorded here
because `run_superframe()` is the one caller of both, and violating it in the loop body would
be exactly the F2 red-team-round-1-on-#523 failure mode one layer up).
