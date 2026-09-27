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
#include "link/clock.hpp"
#include "link/health.hpp"
#include "link/link_types.hpp"
#include "link/master.hpp"
#include "omgp_protocol.h"

#include <cstddef>
#include <cstdint>

namespace omgp {
namespace core {

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

class CoreEngine : public link::HealthListener { // R-03: this engine IS the health listener
  public:
    // wire/host_addr construct the link::Master this engine owns internally (R-02); callbacks is
    // copied by value (R-04) and its ctx must outlive every call below. `clock` is the injected
    // monotonic time source (CLAUDE.md rule 3), shared with the owned link::HealthTracker.
    CoreEngine(link::ByteWire& wire, Clock& clock, uint8_t host_addr, CoreCallbacks callbacks);

    // One superframe (spec FR-001). At T015 it does exactly one thing: drain link::Master's
    // receive path. No status poll, no enrolment probe, no demand item, no health tick — those
    // are T019 (#690) and T028/T029 (#699/#700). It invokes NO callback, directly or
    // transitively (spec FR-021): outcomes are enqueued onto the §8a rings below, and only
    // drain_callbacks() calls back into the application.
    void run_superframe(uint64_t now_us);

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
};

} // namespace core
} // namespace omgp
