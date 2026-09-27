// OMGP host-core — CoreEngine implementation (skeleton, T015). See core/core_engine.hpp for the
// contract, the dependency-surface ruling (research.md R-02) and what is deliberately absent.
// trunk §6: run_superframe() is one superframe and here drains only link::Master's receive path;
// trunk §7's retry/timeout/health rules live in link/, which this file never reaches past.
// Makes tests/unit/test_core_callback_queue.cpp (T014) pass.
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
    // Discarded reads of the state this skeleton declares but does not yet touch — the
    // link/health.cpp:78 idiom, kept for the same reason: a private member nothing in the TU
    // references is what clang's -Wunused-private-field (in -Wall, and this project builds
    // -Werror) reports, and the contract requires these declarations to exist from this task on.
    // PRECAUTIONARY, not measured: the native build is g++, which has no such warning, and the
    // quality stage's clang-tidy branch did not run on this change (see the PR body), so nothing
    // here demonstrates that removing these lines would fail. Each named consumer is the task
    // that removes its line:
    //   clock_        — T019/T029's superframe cadence bookkeeping
    //   backplanes_   — T018's reconcile_slot_map()
    //   descriptors_  — T021's descriptor-cache lookup
    //   event_queue_/desc_queue_/param_queue_ — T029's demand drain (R-07)
    //   budget_       — T029's budget accounting; data-model.md §9 seeds period_us from
    //                   TRUNK_T_poll_us at the start of a superframe, so nothing here restates a
    //                   timing value (CLAUDE.md rule 4) and budget_ stays zero-initialised as
    //                   #686's acceptance criteria require.
    (void)clock_;
    (void)backplanes_;
    (void)descriptors_;
    (void)event_queue_;
    (void)desc_queue_;
    (void)param_queue_;
    (void)budget_;
}

void CoreEngine::run_superframe(uint64_t now_us) {
    // trunk §6: the receive drain, and nothing else, at this task. link::Master::poll() is the
    // only receive path (specs/002-trunk-link-layer/data-model.md §4); its MasterEvent is
    // discarded because no transaction is ever begun yet — T019 (#690) adds the status polls and
    // the enrolment probe, T029 (#700) the demand items and the budget, T042 (#713) the
    // health-notice forwarding. No callback is invoked here, directly or transitively: nothing
    // in this body enqueues, and drain_callbacks() is the only invoker either way (spec FR-021).
    (void)master_.poll(now_us);
}

void CoreEngine::drain_callbacks(size_t max_deliveries) {
    // contracts/core-cpp.md §Engine: the ONLY site that invokes either CoreCallbacks pointer.
    //
    // The bound is min(max_deliveries, what was pending AT ENTRY). Snapshotting the two sizes is
    // what makes "everything currently pending" literal: a callback that enqueues (only an
    // engine-internal producer or T014's seam can, since the rings are private) has its item
    // delivered by the NEXT call, not this one, so this loop cannot be extended by its own
    // deliveries. True by construction — the counters are read once, before any callback runs.
    size_t remaining = max_deliveries;
    size_t lifecycle_budget = pending_lifecycle_.size();
    size_t param_budget = pending_param_results_.size();

    // Lifecycle events first, then parameter results. The order BETWEEN the two rings is not
    // fixed by data-model.md §8a — only FIFO within each ring is — so this is a local choice,
    // stated here rather than left implicit: a lifecycle notice (a node going OFFLINE, a bus
    // fault) is the more urgent of the two when a bounded drain can only deliver some of both.
    // Nothing should be built on the cross-ring order; §8a's guarantee is per-ring.
    LifecycleEvent ev{};
    while (remaining > 0 && lifecycle_budget > 0 && pending_lifecycle_.pop(ev)) {
        --lifecycle_budget;
        --remaining;
        // A null pointer is a host that wants the polling loop without this stream (R-04:
        // CoreCallbacks' members default to nullptr). The delivery is still CONSUMED — calling
        // through the null pointer would be undefined behaviour, and leaving the item queued
        // would fill the ring and start counting drops against an application that asked for
        // nothing.
        if (callbacks_.on_lifecycle != nullptr) {
            callbacks_.on_lifecycle(callbacks_.ctx, ev);
        }
    }

    ParamResultDelivery delivery{};
    while (remaining > 0 && param_budget > 0 && pending_param_results_.pop(delivery)) {
        --param_budget;
        --remaining;
        // R-10: the result is correlated to its request by the id it is delivered with.
        if (callbacks_.on_param_result != nullptr) {
            callbacks_.on_param_result(callbacks_.ctx, delivery.id, delivery.result);
        }
    }
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

uint32_t CoreEngine::dropped_deliveries() const {
    return dropped_deliveries_;
}

void CoreEngine::on_notice(link::Notice notice, uint8_t addr) {
    // R-03 / trunk §6: forwarding a backplane or bus notice as a LifecycleEvent, and degrading
    // every node behind `addr`, is T042 (#713). It compiles and does nothing observable here, by
    // design — CoreEngine satisfies link::HealthListener from its first commit so nothing later
    // has to change its base list. In particular it does NOT call back: §8a's rings are the only
    // delivery path, and only drain_callbacks() reads them (spec FR-021).
    (void)notice;
    (void)addr;
}

} // namespace core
} // namespace omgp
