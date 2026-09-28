// OMGP host-core — CoreEngine implementation. See core/core_engine.hpp for the contract, the
// dependency-surface ruling (research.md R-02) and the stated divergences.
//
// AT THIS COMMIT this file is DECLARATION-ONLY for User Story 1: the API
// tests/unit/test_core_discovery.cpp (T017, #688) compiles and links against exists, and does
// nothing. run_superframe() still drains link::Master's receive path and nothing else (its T015
// body), reconcile_slot_map() assigns no id, and no backplane is ever enrolled. That is
// deliberate — CLAUDE.md rule 8: the test lands failing first, and the recorded failing run is
// this PR's evidence that it can fail. T018-T023 (#689-#694) fill these bodies in the commits
// that follow this one.
//
// trunk §6: run_superframe() is one step of one superframe; trunk §7's retry/timeout/health
// rules live in link/, which this file never reaches past (core/ sees no frame byte).
// protocol-l3 §3/§3.1 is the message vocabulary the following commits encode.
#include "core/core_engine.hpp"

namespace omgp {
namespace core {

CoreEngine::CoreEngine(link::ByteWire& wire, Clock& clock, uint8_t host_addr,
                       CoreCallbacks callbacks)
    // R-02: both collaborators are owned BY VALUE — no references, no pointers, no heap.
    // R-03: `*this` is the link::HealthListener the tracker notifies. Legal here: the
    // HealthListener base subobject is fully constructed before any member initialiser runs, and
    // HealthTracker's own constructor does not notify (link/health.cpp).
    : master_(wire, clock, host_addr), health_(clock, *this), clock_(clock), callbacks_(callbacks),
      // Every table's start-of-life value is the default member initialiser declared beside the
      // field in core/core_types.hpp, so these are empty braces rather than positional lists to
      // keep in step with those structs (the idiom link/health.cpp:73-77 uses one layer down).
      nodes_{}, backplanes_{}, descriptors_{}, event_queue_{}, desc_queue_{}, param_queue_{},
      pending_lifecycle_{}, pending_param_results_{}, budget_{} {
    // Discarded reads of the state this file declares but does not yet touch — the
    // link/health.cpp:78 idiom, kept for the same reason: a private member nothing in the TU
    // references is what clang's -Wunused-private-field (in -Wall, and this project builds
    // -Werror) reports, and the contract requires these declarations to exist from T015 on.
    // PRECAUTIONARY, not measured: the native build is g++, which has no such warning.
    (void)clock_;
    (void)descriptors_;
    (void)event_queue_;
    (void)desc_queue_;
    (void)param_queue_;
    (void)budget_;
}

void CoreEngine::run_superframe(uint64_t now_us) {
    // trunk §6: the receive drain, and nothing else, at this commit. link::Master::poll() is the
    // only receive path (specs/002-trunk-link-layer/data-model.md §4); its MasterEvent is
    // discarded because no transaction is ever begun yet — T019 (#690) adds the status polls and
    // the enrolment probe, T020-T022 (#691-#693) the discovery transactions. No callback is
    // invoked here, directly or transitively: nothing in this body enqueues, and
    // drain_callbacks() is the only invoker either way (spec FR-021).
    (void)master_.poll(now_us);
}

void CoreEngine::reconcile_slot_map(uint8_t backplane_addr, const l3::BpSlotMapResp& resp,
                                    uint64_t now_us) {
    // T018 (#689): R-06's first-fit assignment, FR-010's release and the rule 7 bounds land in
    // the next commit. Declared here so T017's test compiles against the real signature.
    (void)backplane_addr;
    (void)resp;
    (void)now_us;
}

void CoreEngine::drain_callbacks(size_t max_deliveries) {
    // contracts/core-cpp.md §Engine: the ONLY site that invokes either CoreCallbacks pointer.
    //
    // Not re-entrant, BY CONSTRUCTION rather than by precondition: a call made from inside a
    // callback delivers nothing and returns at once. Without this latch each nested call would
    // deliver one more item and recurse again, so the stack would grow with the queue depth — up
    // to LIMIT_max_nodes frames in one top-level call, which on the target's fixed task stack is
    // an overflow rather than a deep-but-survivable call (CLAUDE.md rule 5's "fixed-size" reading
    // of the same rule that bans the heap). Nothing is lost: the item a suppressed nested call
    // would have delivered is still queued, and the drain already in progress or the next
    // top-level one delivers it. Recorded, with the alternatives, in docs/OPEN-QUESTIONS.md
    // (2026-09-27).
    if (draining_) {
        return;
    }
    draining_ = true;

    // The bound is min(max_deliveries, what was pending AT ENTRY) — ONE budget spent across both
    // rings, not one per ring (contracts/core-cpp.md §Engine: "at most `max_deliveries` queued
    // CoreCallbacks calls"). Snapshotting the two sizes is what makes "everything currently
    // pending" literal: a callback that enqueues (only an engine-internal producer or T014's seam
    // can, since the rings are private) has its item delivered by the NEXT call, not this one, so
    // this loop cannot be extended by its own deliveries. True by construction — the counters are
    // read once, before any callback runs.
    size_t remaining = max_deliveries;
    size_t lifecycle_budget = pending_lifecycle_.size();
    size_t param_budget = pending_param_results_.size();

    // The two rings are served in ALTERNATION, with the starting ring carried across calls in
    // next_is_param_. data-model.md §8a fixes FIFO within each ring and says nothing across the
    // two, so the cross-ring order is this engine's choice — but "all of one ring, then the
    // other" is not a free choice: under a bounded drain whose budget is at or below one ring's
    // arrival rate, the second ring is reached NEVER, and its deliveries are then destroyed by
    // the drop-newest rule (per R-10 a ParamRequestId is only released when its result is
    // delivered, so a permanently starved result ring is how the get_param id pool would wedge).
    // Alternating bounds each stream's starvation at one delivery behind the other instead.
    // Nothing should be built on WHICH ring a given drain starts with; the guarantee here is
    // per-ring FIFO plus this fairness, not a fixed interleaving.
    LifecycleEvent ev{};
    ParamResultDelivery delivery{};
    while (remaining > 0 && (lifecycle_budget > 0 || param_budget > 0)) {
        const bool take_param = param_budget > 0 && (next_is_param_ || lifecycle_budget == 0);
        if (take_param) {
            if (!pending_param_results_.pop(delivery)) {
                break; // unreachable while param_budget <= size(): defensive, not a known path
            }
            --param_budget;
            --remaining;
            // Argued like link/master.cpp:400 and link/health.cpp:700, not assumed: the mutant
            // either reads back false and coincides with the original, or leaves next_is_param_
            // set through every param delivery — which serves the param ring until its budget is
            // spent and starves the lifecycle ring behind it, so "a bounded drain serves both
            // rings" (hi - lo <= 1 across 16 ticks) would see 1 and 15. That case ran, and this
            // mutant survived it (CI run 36321491019), so it reads false.
            // mutant-ok(equivalent, cxx_assign_const): the mutation and the original coincide.
            next_is_param_ = false;
            // R-10: the result is correlated to its request by the id it is delivered with.
            if (callbacks_.on_param_result != nullptr) {
                callbacks_.on_param_result(callbacks_.ctx, delivery.id, delivery.result);
            }
        } else {
            if (!pending_lifecycle_.pop(ev)) {
                break; // likewise
            }
            --lifecycle_budget;
            --remaining;
            next_is_param_ = true;
            // A null pointer is a host that wants the polling loop without this stream (R-04:
            // CoreCallbacks' members default to nullptr). The delivery is still CONSUMED —
            // calling through the null pointer would be undefined behaviour, and leaving the item
            // queued would fill the ring and start counting drops against an application that
            // asked for nothing. The budget is charged either way: `max_deliveries` bounds the
            // deliveries a call consumes, not only the ones that reach a function pointer.
            if (callbacks_.on_lifecycle != nullptr) {
                callbacks_.on_lifecycle(callbacks_.ctx, ev);
            }
        }
    }

    // Same argument, same evidence: the mutant either reads back false, or leaves the latch set
    // after this top-level drain returns and makes every later drain_callbacks() a no-op — which
    // "drain_callbacks(n) delivers at most n and leaves the remainder pending" (5 delivered after
    // the second drain) and the re-entrancy case's closing "a later top-level drain still works"
    // would both fail on. Both ran, and this mutant survived them (CI run 36321491019).
    // mutant-ok(equivalent, cxx_assign_const): the mutation and the original coincide.
    draining_ = false;
}

bool CoreEngine::enqueue_lifecycle(const LifecycleEvent& ev) {
    // data-model.md §8a: drop-NEWEST and count. Ring<T, N>::push() already refuses rather than
    // evicting (core/core_types.hpp); the counting is this engine's, per §8a.
    if (!pending_lifecycle_.push(ev)) {
        ++dropped_deliveries_;
        return false;
    }
    return true;
}

bool CoreEngine::enqueue_param_result(ParamRequestId id, const ParamResult& result) {
    ParamResultDelivery delivery{};
    delivery.id = id;
    delivery.result = result;
    if (!pending_param_results_.push(delivery)) {
        ++dropped_deliveries_; // §8a: one counter for both rings
        return false;
    }
    return true;
}

size_t CoreEngine::node_index(uint8_t node_id) {
    // data-model.md §3 / R-05: the table is indexed by `node_id - ADDR_module_min`, and
    // core/core_types.hpp's static_asserts already pin that the whole module address range fits
    // inside LIMIT_max_nodes entries. Any other id — the host's own address, a backplane
    // address, 0xFF — names no entry.
    if (node_id < ADDR_module_min || node_id > ADDR_module_max) {
        return kNoNodeIndex;
    }
    return static_cast<size_t>(node_id) - static_cast<size_t>(ADDR_module_min);
}

size_t CoreEngine::backplane_index(uint8_t addr) {
    // data-model.md §4 / trunk §5: 0x01..0x0F are the backplane addresses; the host's own
    // address and every module id name no BackplaneRecord.
    if (addr < ADDR_backplane_min || addr > ADDR_backplane_max) {
        return kNoNodeIndex;
    }
    return static_cast<size_t>(addr) - static_cast<size_t>(ADDR_backplane_min);
}

DiscoveryState CoreEngine::discovery_state(uint8_t node_id) const {
    const size_t idx = node_index(node_id);
    if (idx == kNoNodeIndex) {
        return DiscoveryState::Undiscovered;
    }
    return nodes_[idx].discovery;
}

bool CoreEngine::node_in_use(uint8_t node_id) const {
    const size_t idx = node_index(node_id);
    if (idx == kNoNodeIndex) {
        return false;
    }
    return nodes_[idx].in_use;
}

bool CoreEngine::backplane_enrolled(uint8_t addr) const {
    const size_t idx = backplane_index(addr);
    if (idx == kNoNodeIndex) {
        return false;
    }
    return backplanes_[idx].enrolled;
}

uint32_t CoreEngine::dropped_deliveries() const {
    return dropped_deliveries_;
}

void CoreEngine::on_notice(link::Notice notice, uint8_t addr) {
    // R-03 / trunk §6: recording link::Notice::ENROLLED against data-model.md §4's
    // BackplaneRecord::enrolled is T019's (#690), and forwarding the SUSPECT/OFFLINE/RECOVERED
    // and bus notices as LifecycleEvents is T042's (#713). It compiles and does nothing
    // observable here, by design — CoreEngine satisfies link::HealthListener from its first
    // commit so nothing later has to change its base list. In particular it does NOT call back:
    // §8a's rings are the only delivery path, and only drain_callbacks() reads them (FR-021).
    (void)notice;
    (void)addr;
}

} // namespace core
} // namespace omgp
