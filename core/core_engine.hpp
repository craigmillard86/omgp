// OMGP host-core — CoreEngine: the portable, MCU-independent scheduler that the ESP32-S3
// firmware and F4's virtual rig both drive through the same code (docs/omgp-spec-v0.7.md §30
// "MCU Independence and OMGP Core", §39 "Software Architecture").
//
// At T015 this is a SKELETON. It owns its collaborators, declares its fixed-size state, and
// implements exactly one piece of real behaviour: the pending-delivery rings and
// drain_callbacks() (data-model.md §8a), which is what makes spec FR-021 — "the scheduler
// proceeds regardless of how long the application takes" — true by construction rather than by
// convention. Everything else here is construction, declarations, and empty or trivially
// returning bodies; the scheduling itself arrives with T019/T029, discovery with T018/T020-T022,
// the parameter/channel API with T027, and the on_notice() body with T042.
//
// trunk §6: run_superframe() is one superframe, called once per TRUNK_T_poll_us of simulated
// time by the caller's own loop — this engine owns no clock of its own and reads no wall clock
// (CLAUDE.md rule 3); time arrives as the now_us parameter and through the injected Clock.
// trunk §7's retry, timeout and health rules stay inside link::Master and link::HealthTracker,
// which this engine owns and never reaches past: core/ never sees a frame byte.
// protocol-l3 §3 is the message vocabulary the later stories encode; nothing here interprets a
// payload.
//
// Contract: specs/003-host-core-engine/contracts/core-cpp.md §Engine (the public API and the
// private member list below are that section's, verbatim in shape).
// Data model: data-model.md §3 (NodeRecord table), §4 (BackplaneRecord table), §5 (descriptor
// cache), §6 (demand rings), §8a (pending-delivery rings), §9 (SuperframeBudget).
// research.md: R-02 (dependency surface), R-03 (this engine IS the link::HealthListener), R-04
// (function-pointer-plus-context callbacks, no std::function), R-07 (three demand rings), R-12
// (descriptor-cache capacity).
// Makes tests/unit/test_core_callback_queue.cpp (T014, #685) pass.
//
// DEPENDENCY SURFACE (research.md R-02, a STATED divergence from CLAUDE.md's literal
// "OMGPTransport" wording, already ruled in the artefacts and not re-litigated here): core/
// includes link/master.hpp, link/health.hpp, link/clock.hpp and link/link_types.hpp directly,
// because transport/ is empty and F2's own contracts/link-cpp.md "What F3/F4 need" names those
// types as this feature's interface. link::ByteWire reaches the constructor below through
// link/master.hpp, never by including link/byte_wire.hpp here. Nothing from link/responder.hpp,
// link/frame.hpp, sim/, cli/, transport/ or a platform header.
//
// Embedded path (CLAUDE.md rule 5): C++17, no exceptions, no RTTI, no heap — every table is an
// inline fixed-size array, sized by a generated symbol, and nothing is constructed after init.
#pragma once

#include "core/core_types.hpp"
#include "l3/l3_types.hpp"
#include "link/clock.hpp"
#include "link/health.hpp"
#include "link/link_types.hpp"
#include "link/master.hpp"
#include "omgp_protocol.h"

#include <cstddef>
#include <cstdint>

namespace omgp {
namespace core {

// One line of the operation transcript (tasks.md T023): the superframe this request belonged
// to, and the request itself as it went to link::Master::begin — the L3 opcode (protocol-l3
// §3.1), the trunk address the frame was addressed to (trunk §5: a backplane, always, since
// module ids never appear as L2 addresses) and the L3 node id inside it (the backplane's own
// address for a backplane-targeted message, a module's id for a bridged one, §8).
struct TranscriptEntry {
    uint32_t superframe;
    uint8_t opcode;
    uint8_t dst;
    uint8_t node_id;
};

// The transcript sink: a plain function pointer plus one opaque context, the same shape R-04
// chose for CoreCallbacks and for the same reasons (no std::function, no virtual, nothing to
// construct). Installed only through CoreEngineTestSeam — there is no public setter, because
// this is a test instrument and not part of the application-facing API.
using TranscriptFn = void (*)(void* ctx, const TranscriptEntry& entry);

// The test-only enqueue seam, DECLARED here and never defined in core/: at T015 no production
// path enqueues onto the §8a rings yet (the real producers are T018/T020-T022/T027/T042), so
// T014's test is the only thing that can load them. tests/unit/test_core_callback_queue.cpp
// supplies the definition. Chosen over a public enqueue method, and over a gated one, for two
// reasons: a friend declaration emits no code at all, so it costs the production build nothing
// whether or not anyone defines this type (tasks.md T023's "gated so it costs nothing when
// unused", achieved by construction rather than by an #ifdef the two build paths would both have
// to set); and it cannot be reached by an application that has not itself defined
// omgp::core::CoreEngineTestSeam, which a public method could not claim. It is deliberately not
// a general enqueue API and must not grow into one.
struct CoreEngineTestSeam;

class CoreEngine final : public link::HealthListener { // R-03: this engine IS the health listener
  public:
    // wire/host_addr construct the link::Master this engine owns internally (R-02); callbacks is
    // copied by value (R-04) and its ctx must outlive every call below. `clock` is the injected
    // monotonic time source (CLAUDE.md rule 3), shared with the owned link::HealthTracker.
    CoreEngine(link::ByteWire& wire, Clock& clock, uint8_t host_addr, CoreCallbacks callbacks);

    // One step of the superframe scheduler (spec FR-001/FR-002/FR-003/FR-004, trunk §6):
    // drains link::Master's receive path, applies whatever transaction just concluded, and —
    // if the wire is free — issues the next request this superframe's plan calls for, in
    // trunk §6's order: one status poll per enrolled backplane (GET_STATUS and BP_SLOT_MAP on
    // alternate superframes), then demand items up to the superframe budget, then exactly one
    // enrolment probe. It invokes NO callback, directly or transitively (spec FR-021):
    // outcomes are enqueued onto the §8a rings below, and only drain_callbacks() calls back.
    //
    // ONE link::Master::begin() PER CALL, at most, and that is not an implementation detail:
    // link::Master runs one transaction at a time (trunk §3) and this engine owns no way to
    // move time — the injected Clock is a reader (CLAUDE.md rule 3), so a transaction begun at
    // `now_us` cannot also complete at `now_us`. A superframe's plan therefore SPANS calls, and
    // the superframe counter advances when the previous plan is exhausted, not on a `now_us`
    // boundary. That is contracts/core-cpp.md's own "the caller decides cadence, this engine
    // only assumes it is called often enough that no superframe is skipped", made concrete: the
    // caller drives this once per TRUNK_T_poll_us of simulated time (or faster — a faster
    // cadence issues the same requests in the same order, which is what spec SC-005 asks for),
    // and each plan's scheduled traffic is held inside one TRUNK_T_poll_us by the budget below.
    //
    // WHAT THE BUDGET DOES AND DOES NOT BOUND (CLAUDE.md rule 11). The demand-traffic budget
    // (spec FR-004/FR-005/FR-027) is debited by the MEASURED duration of this superframe's own
    // transactions, and a demand item is admitted only while its own estimate still fits — with
    // one stated exception, the first demand item of a superframe, which is admitted whatever
    // the estimate says. Without that exception a rig whose mandatory status polls alone
    // approach TRUNK_T_poll_us (three backplanes at the reference rate already spend over half
    // of it) admits NO demand item, ever, and discovery never progresses — a livelock, not slow
    // progress. The status polls (FR-002) and the enrolment probe (FR-003) are unconditional
    // and outside the budget entirely (FR-028), so a superframe whose probe times out on a
    // silent address overruns TRUNK_T_poll_us; trunk §7 contemplates exactly that ("the
    // superframe that issues it stretches accordingly"). Both are DIVERGENCES from FR-005's
    // literal "total scheduled traffic MUST NOT exceed its period", recorded with their
    // reasoning in docs/OPEN-QUESTIONS.md (2026-09-28).
    void run_superframe(uint64_t now_us);

    // research.md R-06 / contracts/core-cpp.md §Node-ID assignment. Called once per BP_SLOT_MAP
    // response — by run_superframe() on the real path, and directly by tests that want to hand
    // the engine a response the wire cannot carry.
    //
    // Reconciles against `resp.occupied` as ABSOLUTE state, per the human ruling of 2026-09-22
    // (docs/OPEN-QUESTIONS.md, item 3): `changed` is a drain-on-send delta a lost response can
    // silently drop, so it is a hint and never the trigger. A slot newly occupied relative to
    // this backplane's own record gets the first free id in ADDR_module_min..ADDR_module_max
    // (ascending slot index within this response, ascending backplane address across them);
    // a slot newly unoccupied has its id released to the pool, its NodeRecord cleared, its
    // DescriptorCacheEntry refcount decremented and LifecycleKind::NodeRemoved enqueued —
    // whether or not it had reached Discovered. Pool exhaustion enqueues
    // LifecycleKind::NodeIdPoolExhausted for that slot and never wraps onto a live id —
    // EDGE-triggered, once per transition into the shortage per slot, never once per sweep that
    // re-observes the same standing shortage (BackplaneRecord::exhaustion_reported; a repeat per
    // sweep would fill §8a's drop-newest ring with one unchanged condition and refuse the
    // presence events behind it). The slot is reconsidered on every sweep either way, and a slot
    // that empties, is assigned an id, or leaves slot_count is reported again if it later
    // transitions back into the shortage. A slot whose notice the ring itself REFUSED does not
    // count as reported, so it is re-offered on the next sweep rather than lost for ever.
    // Nothing is reported as NodeDiscovered here: that fires on reaching Discovered (T022).
    //
    // TOTAL for any input (spec FR-030's forward-compatibility rule, CLAUDE.md rule 7): a
    // slot_count above LIMIT_bp_slot_map_max_slots, a bitmap shorter than slot_count implies,
    // and bits set at or past slot_count are each ignored by bound rather than followed into an
    // out-of-range table write. The wire path cannot deliver the first of those —
    // l3::decode_bp_slot_map_resp refuses it (l3/l3_payload.cpp) — so this bound is this
    // engine's own, independent of the codec's, and is exercised by a direct call.
    void reconcile_slot_map(uint8_t backplane_addr, const l3::BpSlotMapResp& resp, uint64_t now_us);

    // Invokes at most `max_deliveries` queued CoreCallbacks calls (default: everything currently
    // pending). The ONLY place either callback is ever invoked — never from run_superframe()
    // (contracts/core-cpp.md §Engine; data-model.md §8a; R-04). Call it as often as the
    // application wants delivery latency to be low; skipping it for several superframes just
    // lets the pending rings fill (and, once full, refuse the newest arrival and count it —
    // never silently evict an older notice).
    //
    // `max_deliveries` is ONE budget spent across both pending rings, which are served in
    // alternation: a bounded drain never empties one ring before looking at the other, so neither
    // the lifecycle stream nor the parameter-result stream can starve while the other is served
    // (§8a fixes FIFO within each ring and nothing across the two — do not build on which ring a
    // given call starts with). A delivery whose CoreCallbacks pointer is null is consumed and
    // charged to the budget like any other.
    //
    // NOT re-entrant: called from inside a callback it delivers nothing and returns immediately —
    // the items stay queued for the drain already in progress, or for the next top-level call.
    // That is a bound on stack depth, not a convenience: recursing once per queued item would
    // cost up to LIMIT_max_nodes frames in one call on a target with a fixed task stack.
    void drain_callbacks(size_t max_deliveries = SIZE_MAX);

    // Read-only introspection for tests and for a host application that wants a snapshot beyond
    // the callback stream (not a substitute for it — spec User Story 5 is push-based). Both are
    // bounds-safe for ANY uint8_t: an id outside ADDR_module_min..ADDR_module_max names no table
    // entry and reads as "not in use, not yet discovered" rather than indexing past the table.
    DiscoveryState discovery_state(uint8_t node_id) const;
    bool node_in_use(uint8_t node_id) const;

    // Whether `addr` is an enrolled backplane (data-model.md §4's BackplaneRecord::enrolled,
    // set from link::Notice::ENROLLED — R-03). Joins the read-only introspection group above
    // for the same reason dropped_deliveries() did: enrolment is the precondition for every
    // status poll this engine issues (FR-002) and is otherwise observable only as an absence in
    // the transcript. Bounds-safe for ANY uint8_t: an address outside
    // ADDR_backplane_min..ADDR_backplane_max names no record and reads false.
    bool backplane_enrolled(uint8_t addr) const;

    // data-model.md §8a: how many deliveries have been REFUSED by a full pending ring since
    // construction. §8a lists the counter as engine state with no reader; this accessor joins
    // the read-only introspection group above so the drop-newest rule is observable at all
    // (ruling recorded in PR for #685/#686). Never decreases; saturating behaviour past
    // UINT32_MAX is not modelled, and at one drop per superframe that is ~99 days of wrap-free
    // counting — stated, not enforced.
    uint32_t dropped_deliveries() const;

    // link::HealthListener (R-03).
    void on_notice(link::Notice notice, uint8_t addr) override;

  private:
    friend struct CoreEngineTestSeam;

    // data-model.md §8a: the two enqueue points. Both are drop-NEWEST — a full ring refuses this
    // item, leaves every queued item untouched, and counts the refusal in dropped_deliveries_,
    // rather than evicting an older (possibly more urgent) notice or reordering the queue.
    // Return false = refused, nothing queued. Private: the application never enqueues; the
    // engine's own producers (T018/T020-T022/T027/T042) and T014's seam do.
    bool enqueue_lifecycle(const LifecycleEvent& ev);
    bool enqueue_param_result(ParamRequestId id, const ParamResult& result);

    // data-model.md §3 / R-05: nodes_ is indexed by `node_id - ADDR_module_min`. Returns
    // kNoNodeIndex for any id outside ADDR_module_min..ADDR_module_max, which is what makes the
    // two public accessors bounds-safe for every uint8_t.
    static constexpr size_t kNoNodeIndex = static_cast<size_t>(-1);
    static size_t node_index(uint8_t node_id);
    // data-model.md §4: backplanes_ is indexed by `addr - ADDR_backplane_min`; kNoNodeIndex for
    // any address outside ADDR_backplane_min..ADDR_backplane_max, same contract as above.
    static size_t backplane_index(uint8_t addr);

    // --- the superframe plan (trunk §6, spec FR-001..FR-005) --------------------------------

    // Where the CURRENT superframe's plan has got to. trunk §6 numbers the three parts; this
    // walks them in that order and never goes backwards within one superframe, which is what
    // makes "every enrolled backplane's status poll precedes every other transaction of that
    // superframe" (FR-002) true by construction rather than by a comparison at the end.
    enum class Phase : uint8_t { Poll, Demand, Probe, Closed };
    // What the transaction currently outstanding at link::Master IS, so its terminal
    // MasterEvent can be attributed. One at a time (trunk §3), so one value, not a table.
    enum class TxKind : uint8_t { None, StatusPoll, SlotMap, Probe, Identify, ReadDesc };

    void open_superframe(uint64_t now_us);
    // Issues the next request this superframe's plan calls for; false when the plan is spent.
    bool issue_next(uint64_t now_us);
    bool issue_demand(uint64_t now_us);
    // What trying to resume one node's interrupted descriptor read did: nothing that needed the
    // wire (the entry was reclaimed or had already completed, both settled in place), a request
    // issued, or the superframe budget / link::Master refusing — which ends the demand phase.
    enum class Resume { Nothing, Issued, Refused };
    // Resumes `node`'s read at the bytes its cache entry already holds. Shared by issue_demand()'s
    // two resumption passes — the recovery sweep and the stalled-node retry — so that a node
    // resumed by either is resumed identically; the passes differ only in WHICH nodes they offer.
    Resume resume_desc_read(uint8_t node_id, NodeRecord& node, uint64_t now_us);
    // issue_demand()'s two discovery passes, which ALTERNATE by superframe parity because each
    // can have work for ever and so would starve the other if either were permanently first (see
    // issue_demand()). Same tri-state as resume_desc_read(): nothing to do, a request issued, or
    // the budget / link::Master refusing — which ends the demand phase for this superframe.
    Resume scan_identify(uint64_t now_us);      // spec FR-006: nodes not yet identified
    Resume retry_stalled_desc(uint64_t now_us); // nodes whose last chunk made no progress
    // One READ_DESC for `node_id` at `offset`, at the R-09 chunk size (protocol-l3 §3.1).
    bool begin_desc_chunk(uint8_t node_id, uint16_t offset, uint64_t now_us);
    // Encodes one L3 request (protocol-l3 §3) into request_buf_ and hands it to
    // link::Master::begin() addressed to `dst` (trunk §5). Records the transcript line and the
    // outstanding-transaction state on success; returns false, having changed neither, when the
    // codec or the Master refuses.
    bool begin_request(uint8_t dst, uint8_t node_id, uint8_t opcode, const uint8_t* payload,
                       uint8_t len, TxKind kind, uint64_t now_us);
    // Applies one terminal MasterEvent to whatever begin_request() last sent.
    void complete_request(const link::MasterEvent& ev, uint64_t now_us);
    void on_status_block(uint64_t now_us);
    void on_slot_map(const link::MasterEvent& ev, uint64_t now_us);
    void on_identify(const link::MasterEvent& ev, uint64_t now_us);
    void on_desc_chunk(const link::MasterEvent& ev, uint64_t now_us);

    // --- the descriptor cache (spec FR-008, R-12) -------------------------------------------

    // The entry keyed by (module_type, desc_len, desc_crc), complete or still filling, or null.
    DescriptorCacheEntry* find_descriptor(uint8_t module_type, uint16_t desc_len, uint16_t crc);
    // A free entry (never used, or used with refcount 0 and complete), reset and keyed, or null
    // when the table is full — R-12's graceful degradation, not a crash.
    DescriptorCacheEntry* claim_descriptor(uint8_t module_type, uint16_t desc_len, uint16_t crc);
    // Attaches `entry` to every node still waiting on it, each reaching Discovered and
    // enqueueing its own NodeDiscovered / NodeRediscovered (spec FR-017, data-model.md §7).
    void attach_descriptor(DescriptorCacheEntry& entry);
    void release_descriptor(NodeRecord& node);
    void mark_discovered(NodeRecord& node, uint8_t node_id, const DescriptorCacheEntry& entry);

    // data-model.md §9 / R-11: this target's own measured cost if it has one, else the
    // worst-case wire time at the rate now in use — never a fixed per-item constant.
    uint64_t cost_estimate(uint64_t last_measured_us) const;
    // Whether a demand item costing `estimate` may be scheduled into what is left of this
    // superframe. See run_superframe()'s note for the first-item exception.
    bool admit_demand(uint64_t estimate) const;

    link::Master master_;
    link::HealthTracker health_; // constructed with (clock, *this) — R-03
    Clock& clock_;
    CoreCallbacks callbacks_;

    NodeRecord nodes_[LIMIT_max_nodes];
    BackplaneRecord backplanes_[ADDR_backplane_max - ADDR_backplane_min + 1];
    // R-12's own stated descriptor-cache capacity, not a protocol value: there is no
    // `descriptor_cache_entries` symbol in protocol/omgp-protocol.yaml to reference, and the 32s
    // the generated header does define (TRUNK_escape_xor, TLV_PARAM, OP_BP_SLOT_MAP,
    // ADDR_module_i2c_base) are unrelated coincidences. The suppression has to sit on the line
    // itself — tools/check_embedded.py matches `// literal-ok:` per line, not per block.
    DescriptorCacheEntry descriptors_[32]; // literal-ok: R-12's capacity (see note above)

    // R-07 demand rings, drained in this fixed order once T029 lands the loop:
    Ring<EventDrainItem, LIMIT_max_nodes> event_queue_;
    Ring<DescChunkItem, LIMIT_max_nodes> desc_queue_;
    Ring<ParamOpItem, LIMIT_max_nodes> param_queue_;

    // Pending-delivery rings (data-model.md §8a, R-04 correction): run_superframe()'s producers
    // enqueue, drain_callbacks() is the only reader.
    Ring<LifecycleEvent, LIMIT_max_nodes> pending_lifecycle_;
    Ring<ParamResultDelivery, LIMIT_max_nodes> pending_param_results_;
    uint32_t dropped_deliveries_ = 0;
    // drain_callbacks() state, both of them flags rather than anything the application sets:
    // which ring the NEXT delivery comes from (carried across calls — a per-call reset would
    // re-starve the second ring whenever the budget is one), and whether a drain is already in
    // progress on this stack (the re-entrancy latch). Not part of §8a's queue state.
    bool next_is_param_ = false;
    bool draining_ = false;

    SuperframeBudget budget_;

    // --- superframe plan state (trunk §6) ---------------------------------------------------
    // The superframe the plan now being executed IS. Starts at 0 and is incremented by
    // open_superframe(), so the first plan is superframe 1 and 0 means "no plan has ever run" —
    // a distinction the transcript needs, since every line carries a plan's number.
    uint32_t superframe_ = 0;
    bool superframe_open_ = false;
    Phase phase_ = Phase::Closed;
    // Next backplane address the Poll phase will consider; walks ADDR_backplane_min upward once
    // per superframe, so every enrolled backplane is polled exactly once (FR-002).
    uint8_t poll_cursor_ = ADDR_backplane_min;
    // Where the IDENTIFY scan (FR-006) starts, as an OFFSET from ADDR_module_min: advanced past
    // each id it issues for, so a node that never answers cannot hold the scan's head and starve
    // every higher id (see issue_demand()). Persists across superframes deliberately — a per-
    // superframe reset would restore exactly the starvation it exists to prevent.
    uint8_t identify_cursor_ = 0;
    // The same, for the stalled-node descriptor retry (retry_stalled_desc()): where that
    // pass starts looking, as an offset from ADDR_module_min, advanced past each id it issues
    // for. Two stalled nodes would otherwise share the fate a single cursor gives them — the
    // lower id taking every retry the budget admits — for the same reason identify_cursor_
    // exists. Persists across superframes, deliberately.
    uint8_t desc_retry_cursor_ = 0;
    bool probe_issued_ = false;  // this superframe's one enrolment probe (FR-003)
    bool demand_issued_ = false; // whether the first-item exception has been spent
    // The head of the descriptor-chunk queue, popped but not yet admitted by the budget: held
    // HERE rather than pushed back so FR-005's carry-over keeps its FIFO position (a push would
    // send it to the back, behind items queued after it).
    bool desc_held_ = false;
    DescChunkItem desc_hold_{};

    // --- the one outstanding transaction (trunk §3) -----------------------------------------
    TxKind tx_kind_ = TxKind::None;
    uint8_t tx_addr_ = 0;       // trunk address the frame went to (a backplane, §5)
    uint8_t tx_node_ = 0;       // L3 node id inside it (§8: the backplane bridges by node id)
    uint16_t tx_offset_ = 0;    // ReadDesc only: the offset this chunk asked for (R-09)
    uint64_t tx_issued_us_ = 0; // for FR-027's measurement — never a wall clock (rule 3)
    uint8_t l3_seq_ = 0;        // protocol-l3 §3: request/response correlation tag
    // The rate this engine last put on the wire, so a probe handed out at the SAME rate does
    // not call link::Master::set_bit_rate() and count a rate change that did not happen.
    uint32_t wire_rate_ = TRUNK_bit_rate;
    uint8_t request_buf_[LIMIT_max_l3_message] = {};

    // The transcript sink (T023). Two null pointers and one null test per begin_request() is
    // the WHOLE cost when nothing is installed — stated precisely rather than as "costs
    // nothing": the branch is emitted, and this is not an #ifdef, deliberately, so the native
    // and ESP-IDF builds compile the identical core/ sources (CLAUDE.md rule 10).
    TranscriptFn transcript_ = nullptr;
    void* transcript_ctx_ = nullptr;
};

} // namespace core
} // namespace omgp
