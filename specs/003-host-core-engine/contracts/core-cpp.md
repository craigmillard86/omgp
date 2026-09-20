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
    // this superframe's status polls (FR-002), one enrolment probe (FR-003), then demand
    // items up to the remaining budget in event/desc-chunk/param order (FR-020, R-07), and
    // finally advances link::HealthTracker::tick(). Called once per TRUNK_T_poll_us of
    // simulated time by the caller's own loop — this engine does not own or read a wall clock
    // (CLAUDE.md rule 3); the caller decides cadence, this engine only assumes it is called
    // often enough that no superframe is skipped (an assumption, not enforced — mirrors F3
    // obligation 2 in contracts/link-cpp.md, one layer up). Never invokes a callback itself
    // (spec FR-021, R-04 correction) — outcomes are enqueued onto the pending-delivery rings
    // (data-model.md §8a); only drain_callbacks() below calls back into the application.
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
