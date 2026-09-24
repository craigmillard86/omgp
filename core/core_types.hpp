// OMGP host-core — shared value types every other core/ component builds on.
// protocol-l3 §2 (the host maps (backplane, slot) -> node ID; NodeRecord/BackplaneRecord are
// that map's tables), §3 (status/error vocabulary ParamResult::reason carries), §4 (descriptor
// TLV bytes DescriptorCacheEntry::blob holds verbatim); trunk §6 (superframe period, the
// SuperframeBudget), §7 (SUSPECT/OFFLINE thresholds NodeRecord's liveness fields mirror at
// module level, research.md R-03).
// Embedded path (CLAUDE.md rule 5): C++17, no exceptions, no RTTI, no heap, no OS.
// Declarations only — this header holds no engine, no scheduling and no codec; the one piece
// of behaviour is Ring<T, N>, the fixed-capacity container the demand rings (data-model.md §6)
// and the pending-delivery rings (§8a) are declared in terms of.
// Spec: specs/003-host-core-engine/data-model.md §1-9 and §8a;
//       specs/003-host-core-engine/contracts/core-cpp.md "Types".
#pragma once

#include "omgp_protocol.h"

#include <cstddef>
#include <cstdint>

namespace omgp {
namespace core {

// --- Guards on the YAML-editable operands these tables are sized and indexed by -------------
// Same shape as link/link_types.hpp:43/:52 one layer down: a protocol edit that breaks a
// structural assumption fails the build HERE, with a message naming what it broke, rather
// than as an out-of-range table access at some unrelated call site.

// data-model.md §3 / research.md R-05: NodeRecord tables are indexed by
// `node_id - ADDR_module_min`, so the whole module address range must fit inside a
// LIMIT_max_nodes-entry table. Widening the address range past the table size in the YAML
// must fail here, not as an out-of-bounds write the first time the widened id is assigned.
static_assert(ADDR_module_max >= ADDR_module_min,
              "ADDR_module_min..ADDR_module_max must not be inverted or the node index wraps");
static_assert(static_cast<size_t>(ADDR_module_max) - static_cast<size_t>(ADDR_module_min) + 1 <=
                  static_cast<size_t>(LIMIT_max_nodes),
              "LIMIT_max_nodes must cover ADDR_module_min..ADDR_module_max: the node table is "
              "indexed by node_id - ADDR_module_min (data-model.md §3, R-05)");

// data-model.md §4: BackplaneRecord tables are sized
// `ADDR_backplane_max - ADDR_backplane_min + 1` and indexed by `addr - ADDR_backplane_min`.
// An inverted range would wrap that subtraction into a huge size_t table size.
static_assert(ADDR_backplane_max >= ADDR_backplane_min,
              "ADDR_backplane_min..ADDR_backplane_max must not be inverted or the backplane "
              "table size wraps (data-model.md §4)");

// data-model.md §4: node_id_by_slot is sized LIMIT_bp_slot_map_max_slots and indexed by a slot
// number carried in BP_SLOT_MAP's own uint8_t slot_count field (§10). Raising the YAML cap
// past what that field can express would make the upper slots unaddressable — and the bound
// that stops a lying slot_count from overrunning the table silently useless.
static_assert(LIMIT_bp_slot_map_max_slots > 0 && LIMIT_bp_slot_map_max_slots <= UINT8_MAX,
              "LIMIT_bp_slot_map_max_slots must be a nonzero value a uint8_t slot_count can "
              "express (data-model.md §4, §10; research.md R-01)");

// data-model.md §5: DescriptorCacheEntry::len (uint16_t) must be able to hold any length
// blob[LIMIT_max_descriptor_bytes] can contain.
static_assert(LIMIT_max_descriptor_bytes <= UINT16_MAX,
              "DescriptorCacheEntry::len is uint16_t: LIMIT_max_descriptor_bytes must fit it "
              "(data-model.md §5)");

// --- §Types (contracts/core-cpp.md) --------------------------------------------------------

// Result of the CoreEngine operations that can be refused at admission time. Errors by return
// value; Ok == 0. Separate vocabulary from omgp::link::Status and omgp::l3::Status — those are
// L2 and codec outcomes; this one is the host-core's own.
enum class CoreStatus : uint8_t {
    Ok = 0,
    NoFreeNodeId,    // research.md R-06 id pool exhausted for a newly-occupied slot
    QueueFull,       // a demand-item ring (R-07) had no room; the caller's operation was
                     // refused, nothing partially queued
    NotDiscovered,   // a parameter/channel operation named a node_id not yet Discovered
    RequestIdReused, // GetParam called with the parameter FIFO already at capacity (R-10)
};

// --- §2 Discovery state (per module) -------------------------------------------------------

// Undiscovered: node id assigned (R-06), nothing read yet. Identifying: IDENTIFY in flight, or
// its result (MODEL_ID + CRC) known but not yet checked against the descriptor cache.
// ReadingDescriptor: cache miss; READ_DESC chunks (R-09) in flight, bytes_received < desc_len.
// Discovered: descriptor known (fresh read or cache hit); channel/parameter operations and
// status polling begin.
enum class DiscoveryState : uint8_t { Undiscovered, Identifying, ReadingDescriptor, Discovered };

struct DescriptorCacheEntry; // §5, below — NodeRecord holds a pointer to one

// --- §3 Node record (module) ---------------------------------------------------------------

// Fixed table, LIMIT_max_nodes entries, indexed by `node_id - ADDR_module_min` (R-05). The 16
// index positions no module address reaches are deliberate headroom, not a second bound to
// keep in step with the generated symbol.
struct NodeRecord {
    bool in_use = false;        // this index currently assigned to a slot (R-06)
    uint8_t backplane_addr = 0; // owning backplane's trunk address (0 = none)
    uint8_t slot = 0;           // slot index within that backplane's BP_SLOT_MAP
    DiscoveryState discovery = DiscoveryState::Undiscovered;
    // l3::ModelIdRec's three u16 fields, once known (protocol-l3 §4.1); part of the descriptor
    // cache key (§5, spec FR-008) — held as plain integers so core/ needs no l3/ include here.
    uint16_t model_vendor = 0, model_hw_rev = 0, model_fw_rev = 0;
    uint16_t desc_crc = 0;                            // IdentifyResp::desc_crc, once known
    const DescriptorCacheEntry* descriptor = nullptr; // set once Discovered (hit or fresh read)
    // Liveness (R-03): independent of link::HealthTracker, which never sees module ids. The
    // thresholds are trunk §7's own, reused one layer up at module level.
    uint8_t consecutive_failures = 0; // status-poll / demand-item failures since last success
    uint64_t suspect_since_us = 0;    // set when consecutive_failures reaches
                                      // TRUNK_suspect_after_failures
    bool suspect_or_worse = false;    // true once SUSPECT; cleared by a valid answer
    bool offline = false;             // true once TRUNK_offline_after_suspect_ms elapses in
                                      // SUSPECT
    // Channel-switch (spec User Story 3):
    bool switch_outstanding = false;
    uint8_t switch_channel = 0;
    uint64_t switch_deadline_us = 0; // set from the descriptor's l3::SwitchingRec::settle_ms
    // Event-rate / flood defence (spec FR-025/FR-026, research.md R-07 correction):
    uint8_t events_drained_this_window = 0; // count within event_rate_window_start_us
    uint64_t event_rate_window_start_us = 0;
    bool event_faulted = false; // true once the rate budget is exceeded; stops draining
    // Transaction cost / demotion (spec FR-027/FR-028, research.md R-11 correction):
    uint64_t last_measured_duration_us = 0; // 0 = never measured; seeded conservatively on use
    uint8_t consecutive_overrun_superframes = 0;
    bool demoted = false; // demand items drop to the back of their FIFO (R-07)
};

// §3: a node degrades to unreachable immediately when its backplane_addr's own
// link::HealthTracker state is SUSPECT/OFFLINE or the trunk is bus_fault() (R-03), independent
// of consecutive_failures — reported once as its own lifecycle event, not conflated with the
// backplane's own forwarded notice. event_faulted and demoted are independent flags: a
// flooding node is not necessarily an expensive one, and vice versa; each has its own
// lifecycle report (§7).

// --- §4 Backplane record -------------------------------------------------------------------

// Fixed table, ADDR_backplane_max - ADDR_backplane_min + 1 entries, indexed by
// `addr - ADDR_backplane_min` — mirrors link/'s own backplane addressing (trunk §5), kept
// separate from link::HealthTracker's own record: that one is L2's, this is discovery's.
struct BackplaneRecord {
    bool enrolled = false;  // link::Notice::ENROLLED seen for this address
    uint8_t slot_count = 0; // from the last BP_SLOT_MAP response (R-01)
    // slot -> module node id (0 = unassigned). Sized to LIMIT_bp_slot_map_max_slots, NOT to
    // slot_count, so a backplane cannot overrun this table by lying about its own slot_count
    // (bounds-checked on every write, by whichever code writes it — this header only fixes
    // the size).
    uint8_t node_id_by_slot[LIMIT_bp_slot_map_max_slots] = {};
    // Transaction cost / demotion for this backplane's OWN status poll (spec FR-027/FR-028,
    // R-11 correction) — separate from any NodeRecord's own fields: a backplane's status poll
    // and its modules' demand traffic are measured and demoted independently, since a slow
    // module bus does not necessarily mean a slow backplane-level poll itself.
    uint64_t last_measured_duration_us = 0;
    uint8_t consecutive_overrun_superframes = 0;
    bool demoted = false; // demotes only this backplane's MODULES' demand-item priority
                          // (R-07); the backplane's own status poll (FR-002) and the
                          // enrolment probe (FR-003) are never affected — see FR-028
};

// --- §5 Descriptor cache -------------------------------------------------------------------

// Keyed by (model_vendor, model_hw_rev, model_fw_rev, desc_crc) — the full l3::ModelIdRec
// triple plus the descriptor CRC (spec FR-008). The table is fixed-capacity (32 entries,
// R-12); a full one refuses new entries and every subsequent unique descriptor is read fresh
// rather than cached — graceful degradation, not a crash.
struct DescriptorCacheEntry {
    bool in_use = false;
    uint16_t model_vendor = 0, model_hw_rev = 0, model_fw_rev = 0, desc_crc = 0;
    uint16_t len = 0;                           // <= LIMIT_max_descriptor_bytes
    uint8_t blob[LIMIT_max_descriptor_bytes]{}; // raw TLV bytes (protocol-l3 §4), read back
                                                // with an l3::RecordCursor over it
    // Nodes currently pointing at this entry: incremented when a NodeRecord::descriptor is set
    // to this entry (a fresh read's own completion, or a cache hit), decremented when that
    // NodeRecord's slot is freed (reconcile_slot_map, R-06). An entry with refcount == 0 is
    // eligible for reuse by a later miss; an entry is never reclaimed while any node still
    // points at it.
    uint32_t refcount = 0;
};

// --- §8 Parameter request result (R-10) ----------------------------------------------------
// Declared ahead of §6 because ParamOpItem carries a ParamRequestId.

// Opaque; scoped to outstanding requests only and reused once its result has been delivered —
// NOT a monotonically increasing global counter, which is why it stays a uint8_t sized against
// the parameter FIFO's own fixed capacity rather than an unbounded value (R-10).
using ParamRequestId = uint8_t;

struct ParamResult {
    bool ok = false;
    uint16_t value = 0; // meaningful only if ok
    uint8_t reason = 0; // meaningful only if !ok: a protocol-l3 §3.1 error code, or a
                        // link/transaction-level failure (contracts/core-cpp.md)
};

// --- §6 Demand items (R-07) ----------------------------------------------------------------
// Three fixed-capacity rings (LIMIT_max_nodes entries each), drained in this fixed order every
// superframe after the status polls and the enrolment probe (spec FR-020, R-07).

// GET_EVENT to node_id; re-queued while the node still reports events pending, capped at one
// drain per its own superframe turn (spec FR-025, R-07 correction).
struct EventDrainItem {
    uint8_t node_id = 0;
};

// Next READ_DESC chunk for node_id, starting at offset (R-09).
struct DescChunkItem {
    uint8_t node_id = 0;
    uint16_t offset = 0;
};

struct ParamOpItem {
    uint8_t node_id = 0;
    enum class Kind : uint8_t { Set, Get };
    Kind kind = Kind::Set;
    uint8_t param_id = 0, scope = 0;
    uint16_t value = 0;            // Set only
    ParamRequestId request_id = 0; // Get only (R-10); a Set carries none — fire-and-forget
                                   // except for its own failure report (spec FR-023)
};

// --- §7 Lifecycle event --------------------------------------------------------------------
// One discriminated struct, not a class hierarchy — matching link::Notice + addr one layer
// down (spec FR-016/FR-017/FR-022/FR-026/FR-028; callback R-04).

enum class LifecycleKind : uint8_t {
    // Presence: NodeRemoved fires on BP_SLOT_MAP occupancy loss; NodeDiscovered and
    // NodeRediscovered fire on reaching DiscoveryState::Discovered (spec US5 AS1: "fully
    // identified and described", not merely present).
    NodeDiscovered,
    NodeRemoved,
    NodeRediscovered,
    // Module liveness, derived (R-03).
    NodeSuspect,
    NodeOffline,
    NodeRecovered,
    // Forwarded link::Notice (R-03).
    BackplaneSuspect,
    BackplaneOffline,
    BackplaneRecovered,
    // Forwarded link::Notice, bus-level (node_id = 0).
    BusFault,
    BusRecovered,
    // Spec User Story 3.
    ChannelSettled,
    ChannelSwitchTimedOut,
    // A drained l3::GetEventResp.
    ModuleEvent,
    // Spec FR-023 (Set only; a Get failure is a ParamResult, R-10).
    ParamSetFailed,
    // Spec FR-026: a node's event-delivery rate exceeded budget / recovered
    // (research.md R-07 correction).
    NodeEventFault,
    NodeEventFaultCleared,
    // Spec FR-028: a node's, or a backplane's own modules', demand-traffic priority reduced /
    // restored (research.md R-11 correction) — never the backplane's own status poll or the
    // enrolment probe, which demotion never touches.
    Demoted,
    DemotionCleared,
};

struct LifecycleEvent {
    LifecycleKind kind = LifecycleKind::NodeDiscovered;
    uint8_t node_id = 0; // module or backplane trunk address; 0 for BusFault/BusRecovered
    // Payload, meaningful only for the matching kind:
    uint8_t module_event_type = 0, module_event_remaining = 0; // ModuleEvent
    const uint8_t* module_event_detail = nullptr;
    uint8_t module_event_detail_len = 0;
    uint8_t failed_param_id = 0, failed_reason = 0; // ParamSetFailed
    bool demoted_is_backplane = false;              // Demoted/DemotionCleared: node_id names a
                                                    // module, or (this true) a backplane whose
                                                    // MODULES were demoted
};

// --- §8a Pending-delivery element ----------------------------------------------------------

// One queued on_param_result call (spec FR-021, R-04 correction): run_superframe() enqueues,
// only CoreEngine::drain_callbacks() dequeues and calls back.
struct ParamResultDelivery {
    ParamRequestId id = 0;
    ParamResult result;
};

// --- Ring<T, N> ----------------------------------------------------------------------------

// Fixed-capacity FIFO over exactly N inline T slots: no pointer, no allocator, no growth, and
// nothing to construct after init (CLAUDE.md rule 5). It is the container behind both the
// three demand rings (§6, R-07) and the two pending-delivery rings (§8a, R-04 correction).
//
// The contract it serves is drop-newest (§8a): push() onto a full ring refuses the NEWEST item
// and leaves every queued item untouched, rather than evicting an older — and possibly more
// urgent — notice or silently reordering the queue. COUNTING such a refusal
// (CoreEngine::dropped_deliveries_) belongs to CoreEngine, not here: this container reports the
// refusal to its caller and keeps no statistics of its own.
//
// Behaviour is exercised first by tests/unit/test_core_callback_queue.cpp (T014); this header
// establishes only the structural facts (capacity, storage, no allocation) at compile time.
template <typename T, size_t N> class Ring {
  public:
    static_assert(N > 0, "Ring capacity must be nonzero: a zero-length ring can never accept "
                         "an item and its modular index is undefined");

    // Appends `item` at the back. Returns false and changes nothing when already full.
    bool push(const T& item) {
        if (count_ == N) {
            return false;
        }
        storage_[(head_ + count_) % N] = item;
        ++count_;
        return true;
    }

    // Removes the front (oldest) item into `out`. Returns false and leaves `out` untouched
    // when empty.
    bool pop(T& out) {
        if (count_ == 0) {
            return false;
        }
        out = storage_[head_];
        head_ = (head_ + 1) % N;
        --count_;
        return true;
    }

    size_t size() const {
        return count_;
    }
    bool empty() const {
        return count_ == 0;
    }
    bool full() const {
        return count_ == N;
    }
    static constexpr size_t capacity() {
        return N;
    }

  private:
    T storage_[N]{};
    size_t head_ = 0; // index of the oldest item
    size_t count_ = 0;
};

// --- §9 Superframe budget state (R-11) -----------------------------------------------------

struct SuperframeBudget {
    uint64_t period_us = 0;    // TRUNK_T_poll_us (trunk §6), read from the generated header
                               // once — never restated as a literal
    uint64_t remaining_us = 0; // reset to period_us at the start of every superframe, then
                               // debited by each scheduled transaction's own cost estimate:
                               // that target's last_measured_duration_us (§3, §4) once one
                               // exists, else the worst-case wire time at the rate
                               // link::Master::bit_rate() currently reports — never a fixed
                               // per-item cost, and never the worst-case bound once a real
                               // measurement exists (R-11, corrected per spec FR-027)
};

// --- Callbacks (research.md R-04) ----------------------------------------------------------

// Plain function pointers plus one opaque context, copied by value at CoreEngine construction
// — no std::function, no virtual interface, no template (a portability constraint stricter
// than link/'s own HealthListener, chosen deliberately in R-04). Neither pointer is ever
// invoked from run_superframe(): outcomes are enqueued onto the §8a rings and delivered only
// by CoreEngine::drain_callbacks(), which is what makes "the scheduler proceeds regardless of
// how long the application takes" (spec FR-021) true by construction.
struct CoreCallbacks {
    void* ctx = nullptr;
    void (*on_lifecycle)(void* ctx, LifecycleEvent ev) = nullptr;
    void (*on_param_result)(void* ctx, ParamRequestId id, ParamResult result) = nullptr;
};

} // namespace core
} // namespace omgp
