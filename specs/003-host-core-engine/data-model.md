# Data Model: Host-Core Control Engine

Entity shapes for `core/`. Embedded path (CLAUDE.md rule 5): fixed-size, no heap after init,
no exceptions, no RTTI. Every constant cited by generated symbol (rule 4); no restated
literals. `research.md` decisions are cited as `R-NN`.

## 1. Constants (all generated; never restated)

| Symbol | Value | Used for |
|---|---|---|
| `LIMIT_max_nodes` | 128 | node table size (R-05) |
| `ADDR_module_min` / `ADDR_module_max` | 0x10 / 0x7F | node table index range, id-pool bounds |
| `ADDR_backplane_min` / `ADDR_backplane_max` | 0x01 / 0x0F | backplane table (reuses `link/`'s `kAddrCount`-shaped range) |
| `LIMIT_max_l3_payload` | 64 | per-message payload cap; bounds `BP_SLOT_MAP` bitmap sizes (R-01) and `READ_DESC` chunk size (R-09) |
| `LIMIT_max_descriptor_bytes` | 2048 | descriptor cache entry capacity |
| `TRUNK_T_poll_us` | 2000 | superframe period (R-11) |
| `TRUNK_suspect_after_failures` | 3 | reused for module-level liveness counting (R-03) |
| `TRUNK_offline_after_suspect_ms` | 1000 | reused for module-level liveness aging (R-03) |

New, this feature (not yet generated — added by the `BP_SLOT_MAP` task, R-01):
`LIMIT_bp_slot_map_max_slots` (recommended 248, the largest `slot_count` whose two bitmaps
still fit `LIMIT_max_l3_payload`), added to `protocol/omgp-protocol.yaml` `limits` alongside
the new `l3_payloads.BP_SLOT_MAP` entry.

## 2. Discovery state (per module)

```cpp
enum class DiscoveryState : uint8_t { Undiscovered, Identifying, ReadingDescriptor, Discovered };
```

`Undiscovered`: node id assigned (R-06), nothing read yet. `Identifying`: `IDENTIFY` in flight
or its result (MODEL_ID + CRC) known but not yet checked against the descriptor cache.
`ReadingDescriptor`: cache miss; `READ_DESC` chunks (R-09) in flight, `bytes_received <
desc_len`. `Discovered`: descriptor known (fresh read or cache hit, SC-004), channel/parameter
operations and status polling begin.

## 3. Node record (module)

Fixed table, `LIMIT_max_nodes` entries, indexed by `node_id - ADDR_module_min` (R-05):

```cpp
struct NodeRecord {
    bool in_use = false;              // this index currently assigned to a slot (R-06)
    uint8_t backplane_addr = 0;       // owning backplane's trunk address (0 = none)
    uint8_t slot = 0;                 // slot index within that backplane's BP_SLOT_MAP
    DiscoveryState discovery = DiscoveryState::Undiscovered;
    uint16_t model_vendor = 0, model_hw_rev = 0, model_fw_rev = 0;  // ModelIdRec, once known
    uint16_t desc_crc = 0;            // IdentifyResp::desc_crc, once known
    const DescriptorCacheEntry* descriptor = nullptr;  // set once Discovered (cache hit or fresh read)
    // Liveness (R-03): independent of link::HealthTracker, which never sees module ids.
    uint8_t consecutive_failures = 0; // status-poll / demand-item failures since last success
    uint64_t suspect_since_us = 0;    // set when consecutive_failures reaches TRUNK_suspect_after_failures
    bool suspect_or_worse = false;    // true once SUSPECT; cleared by a valid answer
    bool offline = false;             // true once TRUNK_offline_after_suspect_ms elapses in SUSPECT
    // Channel-switch (spec User Story 3):
    bool switch_outstanding = false;
    uint8_t switch_channel = 0;
    uint64_t switch_deadline_us = 0;  // set from the descriptor's SwitchingRec::settle_ms
};
```

A node degrades to unreachable immediately when `backplane_addr`'s own `link::HealthTracker`
state is SUSPECT/OFFLINE or the trunk is `bus_fault()` (R-03), independent of
`consecutive_failures` — reported once as its own lifecycle event, not conflated with the
backplane's own forwarded notice.

## 4. Backplane record

Fixed table, `ADDR_backplane_max - ADDR_backplane_min + 1` (15) entries, indexed by
`addr - ADDR_backplane_min` — mirrors `link/`'s own backplane addressing, kept separate from
`link::HealthTracker`'s own record (that one is L2's; this is discovery's):

```cpp
struct BackplaneRecord {
    bool enrolled = false;               // link::Notice::ENROLLED seen for this address
    uint8_t slot_count = 0;              // from the last BP_SLOT_MAP response (R-01)
    uint8_t node_id_by_slot[248] = {};   // slot -> module node id (0 = unassigned); sized to
                                          // LIMIT_bp_slot_map_max_slots, not slot_count, so a
                                          // backplane cannot overrun this table by lying about
                                          // its own slot_count (bounds-checked on every write)
};
```

## 5. Descriptor cache

Keyed by `(model_vendor, model_hw_rev, model_fw_rev, desc_crc)` — the full `ModelIdRec` triple
plus the descriptor CRC (spec FR-008; `l3::ModelIdRec` has three `u16` fields, not one scalar).
Fixed-capacity table, 32 entries (R-12) — generous against realistic rigs with many identical
modules; a full one refuses new entries and every subsequent unique descriptor is read fresh
rather than cached, a graceful degradation, not a crash:

```cpp
struct DescriptorCacheEntry {
    bool in_use = false;
    uint16_t model_vendor = 0, model_hw_rev = 0, model_fw_rev = 0, desc_crc = 0;
    uint16_t len = 0;                                   // <= LIMIT_max_descriptor_bytes
    uint8_t blob[LIMIT_max_descriptor_bytes];            // raw TLV bytes, l3::RecordCursor over it
    uint32_t refcount = 0;                               // nodes currently pointing at this entry:
                                                           // incremented when a NodeRecord::descriptor
                                                           // is set to this entry (a fresh read's own
                                                           // completion, or a cache hit), decremented
                                                           // when that NodeRecord's slot is freed
                                                           // (reconcile_slot_map, R-06) — an entry with
                                                           // refcount == 0 is eligible for reuse by a
                                                           // later miss, never reclaimed while any node
                                                           // still points at it
};
```

## 6. Demand items (R-07)

Three fixed-capacity ring buffers (`LIMIT_max_nodes` entries each), drained in this fixed
order every superframe after status polls and the enrolment probe (spec FR-020, R-07):

```cpp
struct EventDrainItem { uint8_t node_id; };  // GET_EVENT to node_id; repeated while event_pending > 0

struct DescChunkItem { uint8_t node_id; uint16_t offset; };  // next READ_DESC chunk (R-09)

struct ParamOpItem {
    uint8_t node_id;
    enum class Kind : uint8_t { Set, Get } kind;
    uint8_t param_id, scope;
    uint16_t value;        // Set only
    ParamRequestId request_id;  // Get only (R-10); Set carries none — fire-and-forget except
                                 // for its own failure report (FR-023)
};
```

## 7. Lifecycle event (spec FR-016/017/022, callback R-04)

One discriminated struct, not a class hierarchy (matches `link::Notice` + `addr`):

```cpp
enum class LifecycleKind : uint8_t {
    NodeDiscovered, NodeRemoved, NodeRediscovered,          // presence: NodeRemoved fires on
                                                             // BP_SLOT_MAP occupancy loss;
                                                             // NodeDiscovered/NodeRediscovered
                                                             // fire on reaching Discovered
                                                             // (spec US5 AS1: "fully
                                                             // identified and described", not
                                                             // merely present)
    NodeSuspect, NodeOffline, NodeRecovered,                // module liveness, derived (R-03)
    BackplaneSuspect, BackplaneOffline, BackplaneRecovered, // forwarded link::Notice (R-03)
    BusFault, BusRecovered,                                 // forwarded link::Notice (bus-level, addr = 0)
    ChannelSettled, ChannelSwitchTimedOut,                  // spec User Story 3
    ModuleEvent,                                            // a drained l3::GetEventResp
    ParamSetFailed,                                         // spec FR-023 (Set only; Get failure is a ParamResult, R-10)
};

struct LifecycleEvent {
    LifecycleKind kind;
    uint8_t node_id;              // module or backplane trunk address; 0 for BusFault/BusRecovered
    // Payload, meaningful only for the matching kind:
    uint8_t module_event_type = 0, module_event_remaining = 0;  // ModuleEvent
    const uint8_t* module_event_detail = nullptr; uint8_t module_event_detail_len = 0;
    uint8_t failed_param_id = 0, failed_reason = 0;              // ParamSetFailed
};
```

## 8. Parameter request result (R-10)

```cpp
using ParamRequestId = uint8_t;  // opaque; scoped to outstanding requests, reused once delivered

struct ParamResult {
    bool ok;
    uint16_t value = 0;     // meaningful only if ok
    uint8_t reason = 0;     // meaningful only if !ok (protocol-l3.md §3.1 error codes, or a
                             // link/transaction-level failure — see contracts/core-cpp.md)
};
```

## 8a. Pending-delivery queues (spec FR-021, R-04 correction)

Two fixed-capacity rings, `LIMIT_max_nodes` entries each — `run_superframe()` enqueues onto
these; only `CoreEngine::drain_callbacks()` (caller-invoked, never called from inside
`run_superframe()`) actually calls `CoreCallbacks::on_lifecycle`/`on_param_result`. This is
what makes "the scheduler proceeds regardless of how long the application takes" true by
construction, not by convention (R-04):

```cpp
struct ParamResultDelivery { ParamRequestId id; ParamResult result; };

// Ring<LifecycleEvent, LIMIT_max_nodes> pending_lifecycle_;
// Ring<ParamResultDelivery, LIMIT_max_nodes> pending_param_results_;
// uint32_t dropped_deliveries_ = 0;  // enqueue onto a full ring refuses (drop-newest) and
                                       // counts here rather than silently reordering or
                                       // evicting an older, possibly more urgent, notice
```

## 9. Superframe budget state (R-11)

```cpp
struct SuperframeBudget {
    uint64_t period_us;        // TRUNK_T_poll_us, read from the generated header once
    uint64_t remaining_us;     // reset to period_us at the start of every superframe
};
```

Debited by each scheduled transaction's own worst-case wire time at the rate `Master::
bit_rate()` currently reports (the same reasoning `link/health.cpp`'s `kOutcomeWindowUs`
already applies at L2) — never by a fixed per-item cost (R-11).

## 10. `BP_SLOT_MAP` response payload (R-01, new protocol addition)

```
u8    slot_count
bytes occupied [ceil(slot_count/8)]   // bit i = slot i occupied
bytes changed  [ceil(slot_count/8)]   // bit i = slot i changed since the last response
```

`l3::BpSlotMapResp` (new type, `l3_types.hpp`), `encode_bp_slot_map_resp` /
`decode_bp_slot_map_resp` (new, `l3_payload.hpp`), a new golden vector
`bp_slot_map_full_occupancy` (`slot_count = 248`, every bit set, exercising the maximum size
this payload can take before exceeding `LIMIT_max_l3_payload`).
