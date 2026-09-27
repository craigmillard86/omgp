// CoreEngine's pending-delivery rings and drain_callbacks() — the mechanism that makes spec
// FR-021 ("the scheduler proceeds regardless of how long the application takes") true by
// construction rather than by convention: run_superframe() enqueues, and drain_callbacks() is
// the only place either CoreCallbacks function pointer is ever invoked.
// trunk §6: the rings exist because a superframe's outcomes are queued, not delivered inline —
// a superframe is TRUNK_T_poll_us long whatever the application does with a notice.
// Spec: specs/003-host-core-engine/data-model.md §8a (the two rings, dropped_deliveries_ and the
// drop-newest rule), §7 (LifecycleKind/LifecycleEvent), §8 (ParamRequestId/ParamResult);
// contracts/core-cpp.md §Engine (drain_callbacks(max_deliveries), "the ONLY place either
// callback is ever invoked — never from run_superframe()"); research.md R-04 (function-pointer
// -plus-context callbacks, no std::function); tasks.md T014, made to pass by T015.
//
// WHAT THIS FILE IS AN ORACLE FOR, and what it is not (CLAUDE.md rule 11): every claim below is
// about CoreEngine's queue behaviour. Ring<T, N>'s own container behaviour is
// tests/unit/test_core_types.cpp's (T011) — nothing here re-tests wraparound or pop-on-empty at
// container level. "No callback from run_superframe()" is asserted here for the T015 body (a
// receive drain and nothing else); it is NOT a standing guard for the bodies T019/T029/T042 add
// later, which will enqueue and must be re-asserted by their own tests.
#include "core/core_engine.hpp"

#include "catch_amalgamated.hpp"
#include "fake_clock.hpp"
#include "heap_guard.hpp"
#include "mock_wire.hpp"
#include "omgp_protocol.h"

#include <cstddef>
#include <cstdint>

using omgp::core::CoreCallbacks;
using omgp::core::CoreEngine;
using omgp::core::DiscoveryState;
using omgp::core::LifecycleEvent;
using omgp::core::LifecycleKind;
using omgp::core::ParamRequestId;
using omgp::core::ParamResult;

// The ring capacity under test, by its generated symbol — never the literal 128
// (data-model.md §8a "Limits", protocol/omgp-protocol.yaml limits.max_nodes).
static constexpr size_t kCap = omgp::LIMIT_max_nodes;

// --- the test-only enqueue seam ---------------------------------------------------------------
// core/core_engine.hpp declares `struct CoreEngineTestSeam;` and befriends it without ever
// defining it; this is that definition. At T015 no production path enqueues yet (the real
// producers are T017/T018/T027/T042), so the rings can only be loaded through a seam — and a
// friend defined HERE costs the production build nothing (a friend declaration emits no code)
// and cannot be reached by an application that has not itself defined this type, which a public
// enqueue method could not claim. Deliberately just the two enqueue points: no ring accessor, no
// drop-counter setter, nothing that would let a test reach past the public API for anything the
// public API already exposes (dropped_deliveries()).
namespace omgp {
namespace core {
struct CoreEngineTestSeam {
    static bool enqueue_lifecycle(CoreEngine& engine, const LifecycleEvent& ev) {
        return engine.enqueue_lifecycle(ev);
    }
    static bool enqueue_param_result(CoreEngine& engine, ParamRequestId id,
                                     const ParamResult& result) {
        return engine.enqueue_param_result(id, result);
    }
};
} // namespace core
} // namespace omgp

using Seam = omgp::core::CoreEngineTestSeam;

namespace {

// research.md R-04: the two callbacks are plain function pointers plus one opaque ctx. This is
// that ctx — counters plus a transcript of what arrived, so FIFO order is asserted by identity
// and not only by count.
struct Recorder {
    static constexpr size_t kSlots = kCap + 8; // headroom: a refused enqueue must never deliver
    unsigned lifecycle_calls = 0;
    unsigned param_calls = 0;
    LifecycleKind kinds[kSlots]{};
    uint8_t node_ids[kSlots]{};
    ParamRequestId ids[kSlots]{};
    uint16_t values[kSlots]{};
    // Re-entrancy probe (only the "a callback that enqueues" case sets these): the event a
    // callback pushes back onto the ring from inside its own delivery.
    CoreEngine* engine = nullptr;
    unsigned reentrant_enqueues_left = 0;
    // Re-entrant-DRAIN probe (only the "a callback that calls drain_callbacks()" case sets
    // reentrant_drain): nesting depth measured on the callback itself, so a drain that recursed
    // once per queued item is visible as a depth, not only as a count.
    bool reentrant_drain = false;
    unsigned depth = 0;
    unsigned max_depth = 0;
};

void on_lifecycle_cb(void* ctx, LifecycleEvent ev) {
    auto* r = static_cast<Recorder*>(ctx);
    ++r->depth;
    if (r->depth > r->max_depth) {
        r->max_depth = r->depth;
    }
    if (r->lifecycle_calls < Recorder::kSlots) {
        r->kinds[r->lifecycle_calls] = ev.kind;
        r->node_ids[r->lifecycle_calls] = ev.node_id;
    }
    ++r->lifecycle_calls;
    if (r->reentrant_enqueues_left > 0 && r->engine != nullptr) {
        --r->reentrant_enqueues_left;
        LifecycleEvent late{};
        late.kind = LifecycleKind::BusFault;
        late.node_id = 0xEE; // identity tag: this one must not be delivered by the drain in
                             // progress (see the re-entrancy case)
        Seam::enqueue_lifecycle(*r->engine, late);
    }
    if (r->reentrant_drain && r->engine != nullptr) {
        r->engine->drain_callbacks(); // must not recurse — see the re-entrant-drain case
    }
    --r->depth;
}

void on_param_cb(void* ctx, ParamRequestId id, ParamResult result) {
    auto* r = static_cast<Recorder*>(ctx);
    if (r->param_calls < Recorder::kSlots) {
        r->ids[r->param_calls] = id;
        r->values[r->param_calls] = result.value;
    }
    ++r->param_calls;
}

// One engine over the repo's existing test doubles: tests/support/mock_wire.hpp's ByteWire and
// tests/support/fake_clock.hpp's injected Clock (CLAUDE.md rule 3 — the only time source here;
// nothing sleeps and nothing reads a wall clock). No second wire or clock abstraction.
struct Rig {
    omgp_test::FakeClock clock;
    omgp_test::MockWire wire{clock};
    Recorder rec{};
    CoreEngine engine{wire, clock, omgp::ADDR_host,
                      CoreCallbacks{&rec, on_lifecycle_cb, on_param_cb}};
};

// `node_id` is used here purely as an identity tag on a synthesised event — 0..kCap-1 spans
// values that are not module addresses (ADDR_module_min is 0x10). Nothing in this file claims
// those ids are routable; §8a's rings queue whatever the engine hands them, and T017/T018 are
// where real ids are assigned.
LifecycleEvent lifecycle(LifecycleKind kind, uint8_t tag) {
    LifecycleEvent ev{};
    ev.kind = kind;
    ev.node_id = tag;
    return ev;
}

ParamResult ok_result(uint16_t value) {
    ParamResult r{};
    r.ok = true;
    r.value = value;
    return r;
}

} // namespace

TEST_CASE("a freshly constructed CoreEngine has nothing pending, nothing dropped, and an empty "
          "node table for every uint8_t id",
          "[core][callbacks]") {
    Rig rig;

    // contracts/core-cpp.md §Engine: the read-only introspection accessors are bounds-safe for
    // ANY uint8_t, including the ids no module address reaches. Under the native preset's ASan
    // an out-of-range read here would abort the case rather than return a value.
    for (unsigned n = 0; n <= 0xFF; ++n) {
        const auto id = static_cast<uint8_t>(n);
        REQUIRE_FALSE(rig.engine.node_in_use(id));
        REQUIRE(rig.engine.discovery_state(id) == DiscoveryState::Undiscovered);
    }

    REQUIRE(rig.engine.dropped_deliveries() == 0);
    rig.engine.drain_callbacks();
    REQUIRE(rig.rec.lifecycle_calls == 0);
    REQUIRE(rig.rec.param_calls == 0);
    REQUIRE(rig.engine.dropped_deliveries() == 0);
}

TEST_CASE("enqueueing onto either pending ring invokes neither callback", "[core][callbacks]") {
    Rig rig;

    for (uint8_t i = 0; i < 3; ++i) {
        REQUIRE(Seam::enqueue_lifecycle(rig.engine, lifecycle(LifecycleKind::NodeDiscovered, i)));
    }
    for (uint8_t i = 0; i < 4; ++i) {
        REQUIRE(Seam::enqueue_param_result(rig.engine, i, ok_result(static_cast<uint16_t>(i))));
    }

    // spec FR-021 / data-model.md §8a: enqueue is not delivery.
    REQUIRE(rig.rec.lifecycle_calls == 0);
    REQUIRE(rig.rec.param_calls == 0);
    REQUIRE(rig.engine.dropped_deliveries() == 0);
}

TEST_CASE("run_superframe() invokes no callback, with deliveries pending, across advancing time",
          "[core][callbacks]") {
    Rig rig;
    REQUIRE(Seam::enqueue_lifecycle(rig.engine, lifecycle(LifecycleKind::NodeSuspect, 1)));
    REQUIRE(Seam::enqueue_param_result(rig.engine, 7, ok_result(0x1234)));

    // trunk §6: one call per superframe period of simulated time, the cadence the caller owns
    // (contracts/core-cpp.md §Engine). The wire has no script and no handler, so nothing arrives.
    for (unsigned i = 0; i < 8; ++i) {
        rig.clock.advance(omgp::TRUNK_T_poll_us);
        rig.engine.run_superframe(rig.clock.now_us());
        REQUIRE(rig.rec.lifecycle_calls == 0);
        REQUIRE(rig.rec.param_calls == 0);
    }

    // Still pending, not discarded: the one drain below delivers exactly what was queued.
    rig.engine.drain_callbacks();
    REQUIRE(rig.rec.lifecycle_calls == 1);
    REQUIRE(rig.rec.param_calls == 1);
}

TEST_CASE("drain_callbacks() is the only invoker and delivers every pending item exactly once",
          "[core][callbacks]") {
    Rig rig;
    constexpr unsigned kLifecycle = 3;
    constexpr unsigned kParams = 5;

    for (unsigned i = 0; i < kLifecycle; ++i) {
        REQUIRE(Seam::enqueue_lifecycle(
            rig.engine, lifecycle(LifecycleKind::NodeDiscovered, static_cast<uint8_t>(i))));
    }
    for (unsigned i = 0; i < kParams; ++i) {
        REQUIRE(Seam::enqueue_param_result(rig.engine, static_cast<ParamRequestId>(i),
                                           ok_result(static_cast<uint16_t>(i))));
    }
    REQUIRE(rig.rec.lifecycle_calls == 0);
    REQUIRE(rig.rec.param_calls == 0);

    rig.engine.drain_callbacks();
    REQUIRE(rig.rec.lifecycle_calls == kLifecycle);
    REQUIRE(rig.rec.param_calls == kParams);

    // Exactly once: a second drain finds the rings empty.
    rig.engine.drain_callbacks();
    REQUIRE(rig.rec.lifecycle_calls == kLifecycle);
    REQUIRE(rig.rec.param_calls == kParams);
    REQUIRE(rig.engine.dropped_deliveries() == 0);
}

TEST_CASE("draining empty rings is a no-op, and repeating it changes nothing",
          "[core][callbacks]") {
    Rig rig;

    rig.engine.drain_callbacks();
    REQUIRE(rig.rec.lifecycle_calls == 0);
    REQUIRE(rig.rec.param_calls == 0);
    REQUIRE(rig.engine.dropped_deliveries() == 0);

    rig.engine.drain_callbacks(4);
    rig.engine.drain_callbacks();
    REQUIRE(rig.rec.lifecycle_calls == 0);
    REQUIRE(rig.rec.param_calls == 0);
    REQUIRE(rig.engine.dropped_deliveries() == 0);
}

TEST_CASE("deliveries arrive in enqueue order in both rings", "[core][callbacks]") {
    Rig rig;

    // Distinct kinds AND distinct tags, so a reordered delivery fails on identity rather than
    // being invisible behind a matching count (data-model.md §8a: "never silently reordering").
    REQUIRE(Seam::enqueue_lifecycle(rig.engine, lifecycle(LifecycleKind::NodeDiscovered, 11)));
    REQUIRE(Seam::enqueue_lifecycle(rig.engine, lifecycle(LifecycleKind::NodeSuspect, 22)));
    REQUIRE(Seam::enqueue_lifecycle(rig.engine, lifecycle(LifecycleKind::NodeRemoved, 33)));

    // R-10 correlation: a ParamResult is matched to its request by the id it is delivered with.
    REQUIRE(Seam::enqueue_param_result(rig.engine, 5, ok_result(0x0501)));
    REQUIRE(Seam::enqueue_param_result(rig.engine, 2, ok_result(0x0202)));
    REQUIRE(Seam::enqueue_param_result(rig.engine, 9, ok_result(0x0903)));

    rig.engine.drain_callbacks();

    REQUIRE(rig.rec.lifecycle_calls == 3);
    REQUIRE(rig.rec.kinds[0] == LifecycleKind::NodeDiscovered);
    REQUIRE(rig.rec.node_ids[0] == 11);
    REQUIRE(rig.rec.kinds[1] == LifecycleKind::NodeSuspect);
    REQUIRE(rig.rec.node_ids[1] == 22);
    REQUIRE(rig.rec.kinds[2] == LifecycleKind::NodeRemoved);
    REQUIRE(rig.rec.node_ids[2] == 33);

    REQUIRE(rig.rec.param_calls == 3);
    REQUIRE(rig.rec.ids[0] == 5);
    REQUIRE(rig.rec.values[0] == 0x0501);
    REQUIRE(rig.rec.ids[1] == 2);
    REQUIRE(rig.rec.values[1] == 0x0202);
    REQUIRE(rig.rec.ids[2] == 9);
    REQUIRE(rig.rec.values[2] == 0x0903);
}

TEST_CASE("drain_callbacks(n) delivers at most n and leaves the remainder pending, in order",
          "[core][callbacks]") {
    Rig rig;
    for (uint8_t i = 0; i < 5; ++i) {
        REQUIRE(Seam::enqueue_lifecycle(
            rig.engine, lifecycle(LifecycleKind::ModuleEvent, static_cast<uint8_t>(100 + i))));
    }

    rig.engine.drain_callbacks(2);
    REQUIRE(rig.rec.lifecycle_calls == 2);
    REQUIRE(rig.rec.node_ids[0] == 100);
    REQUIRE(rig.rec.node_ids[1] == 101);

    // The other three are still queued — not dropped, not reordered.
    rig.engine.drain_callbacks();
    REQUIRE(rig.rec.lifecycle_calls == 5);
    REQUIRE(rig.rec.node_ids[2] == 102);
    REQUIRE(rig.rec.node_ids[3] == 103);
    REQUIRE(rig.rec.node_ids[4] == 104);
    REQUIRE(rig.engine.dropped_deliveries() == 0);
}

TEST_CASE("drain_callbacks(0) delivers nothing and leaves everything pending",
          "[core][callbacks]") {
    Rig rig;
    REQUIRE(Seam::enqueue_lifecycle(rig.engine, lifecycle(LifecycleKind::BusFault, 0)));
    REQUIRE(Seam::enqueue_param_result(rig.engine, 1, ok_result(7)));

    rig.engine.drain_callbacks(0);
    REQUIRE(rig.rec.lifecycle_calls == 0);
    REQUIRE(rig.rec.param_calls == 0);

    rig.engine.drain_callbacks();
    REQUIRE(rig.rec.lifecycle_calls == 1);
    REQUIRE(rig.rec.param_calls == 1);
}

TEST_CASE("drain_callbacks(n)'s bound is ONE budget spent across both rings, not n per ring",
          "[core][callbacks]") {
    Rig rig;
    constexpr unsigned kEach = 3;
    for (unsigned i = 0; i < kEach; ++i) {
        REQUIRE(Seam::enqueue_lifecycle(
            rig.engine, lifecycle(LifecycleKind::NodeDiscovered, static_cast<uint8_t>(10 + i))));
        REQUIRE(Seam::enqueue_param_result(rig.engine, static_cast<ParamRequestId>(i),
                                           ok_result(static_cast<uint16_t>(0x200 + i))));
    }

    // T014's acceptance criterion and contracts/core-cpp.md §Engine: "at most `max_deliveries`
    // queued CoreCallbacks calls" — ONE bound over both rings. The only case that can observe it
    // is a bounded drain with BOTH rings loaded: with either ring empty, a per-ring bound and a
    // shared bound are indistinguishable.
    rig.engine.drain_callbacks(4);
    REQUIRE(rig.rec.lifecycle_calls + rig.rec.param_calls == 4);

    // The remaining two are pending, not dropped: the totals close on the next drain.
    rig.engine.drain_callbacks();
    REQUIRE(rig.rec.lifecycle_calls == kEach);
    REQUIRE(rig.rec.param_calls == kEach);
    REQUIRE(rig.engine.dropped_deliveries() == 0);

    // FIFO within each ring is untouched by whatever interleaving the drain chose (data-model.md
    // §8a guarantees per-ring order and nothing across the two).
    for (unsigned i = 0; i < kEach; ++i) {
        REQUIRE(rig.rec.node_ids[i] == static_cast<uint8_t>(10 + i));
        REQUIRE(rig.rec.ids[i] == static_cast<ParamRequestId>(i));
        REQUIRE(rig.rec.values[i] == static_cast<uint16_t>(0x200 + i));
    }
}

TEST_CASE("a bounded drain serves both rings, so neither stream starves under a steady arrival "
          "rate",
          "[core][callbacks]") {
    Rig rig;
    constexpr unsigned kTicks = 16;

    // A host that bounds per-tick callback latency by draining one delivery per superframe, while
    // both streams produce one item per superframe. A drain that always empties one ring before
    // looking at the other delivers that ring 16 times and the other NEVER — and the starved
    // ring's items are then destroyed by the drop-newest rule once it fills (data-model.md §8a),
    // which is silent because dropped_deliveries() is one counter for both rings.
    for (unsigned i = 0; i < kTicks; ++i) {
        REQUIRE(Seam::enqueue_lifecycle(
            rig.engine, lifecycle(LifecycleKind::ModuleEvent, static_cast<uint8_t>(i))));
        REQUIRE(Seam::enqueue_param_result(rig.engine, static_cast<ParamRequestId>(i),
                                           ok_result(static_cast<uint16_t>(i))));
        rig.engine.drain_callbacks(1);
    }

    REQUIRE(rig.rec.lifecycle_calls + rig.rec.param_calls == kTicks); // the bound still holds
    // Fairness, not a fixed cross-ring order: whichever ring a drain starts with, the two streams
    // stay within one delivery of each other, so both make progress at half the drain rate.
    const unsigned lo = rig.rec.lifecycle_calls < rig.rec.param_calls ? rig.rec.lifecycle_calls
                                                                      : rig.rec.param_calls;
    const unsigned hi = rig.rec.lifecycle_calls < rig.rec.param_calls ? rig.rec.param_calls
                                                                      : rig.rec.lifecycle_calls;
    REQUIRE(lo > 0);
    REQUIRE(hi - lo <= 1);
    REQUIRE(rig.engine.dropped_deliveries() == 0); // 16 arrivals per ring, capacity kCap
}

TEST_CASE("drain_callbacks() called from inside a callback delivers nothing and does not recurse",
          "[core][callbacks]") {
    Rig rig;
    rig.rec.engine = &rig.engine;
    rig.rec.reentrant_drain = true; // every delivery calls drain_callbacks() again
    constexpr unsigned kQueued = 64;

    for (unsigned i = 0; i < kQueued; ++i) {
        REQUIRE(Seam::enqueue_lifecycle(
            rig.engine, lifecycle(LifecycleKind::NodeSuspect, static_cast<uint8_t>(i))));
    }

    rig.engine.drain_callbacks();

    // CLAUDE.md rule 5 territory: a drain that re-enters delivers one more item per nested call
    // and so recurses once per queued item — ~kCap frames on a target with a fixed task stack.
    // The nested call is a no-op instead, so the depth is 1 whatever the queue holds.
    REQUIRE(rig.rec.max_depth == 1);
    // Exactly-once, and in order: the outer drain still delivers everything that was pending.
    REQUIRE(rig.rec.lifecycle_calls == kQueued);
    for (unsigned i = 0; i < kQueued; ++i) {
        REQUIRE(rig.rec.node_ids[i] == static_cast<uint8_t>(i));
    }
    REQUIRE(rig.engine.dropped_deliveries() == 0);

    // Nothing was left behind by the suppressed nested calls, and re-entry left no latch set: a
    // later top-level drain still works.
    rig.rec.reentrant_drain = false;
    REQUIRE(Seam::enqueue_lifecycle(rig.engine, lifecycle(LifecycleKind::NodeRemoved, 0xAB)));
    rig.engine.drain_callbacks();
    REQUIRE(rig.rec.lifecycle_calls == kQueued + 1);
    REQUIRE(rig.rec.node_ids[kQueued] == 0xAB);
}

TEST_CASE("the lifecycle ring refuses the NEWEST enqueue when full, counts it, and keeps the "
          "oldest LIMIT_max_nodes",
          "[core][callbacks]") {
    Rig rig;

    for (size_t i = 0; i < kCap; ++i) {
        REQUIRE(Seam::enqueue_lifecycle(
            rig.engine, lifecycle(LifecycleKind::NodeDiscovered, static_cast<uint8_t>(i))));
    }
    REQUIRE(rig.engine.dropped_deliveries() == 0);

    // data-model.md §8a: "enqueue onto a full ring refuses (drop-newest) and counts here rather
    // than silently reordering or evicting an older, possibly more urgent, notice".
    REQUIRE_FALSE(Seam::enqueue_lifecycle(rig.engine, lifecycle(LifecycleKind::BusFault, 0xEE)));
    REQUIRE(rig.engine.dropped_deliveries() == 1);

    rig.engine.drain_callbacks();

    // Exactly kCap delivered, and they are the FIRST kCap enqueued: an implementation that
    // evicted the oldest to make room would deliver kCap items too, but its first item would be
    // tag 1 and its last the refused 0xEE. Both ends are asserted, so that swap fails here.
    REQUIRE(rig.rec.lifecycle_calls == kCap);
    REQUIRE(rig.rec.node_ids[0] == 0);
    REQUIRE(rig.rec.node_ids[kCap - 1] == static_cast<uint8_t>(kCap - 1));
    REQUIRE(rig.rec.kinds[kCap - 1] == LifecycleKind::NodeDiscovered);
    for (size_t i = 0; i < kCap; ++i) {
        REQUIRE(rig.rec.node_ids[i] == static_cast<uint8_t>(i));
    }
    REQUIRE(rig.engine.dropped_deliveries() == 1);
}

TEST_CASE("the param-result ring refuses the NEWEST enqueue when full, counts it, and keeps the "
          "oldest LIMIT_max_nodes",
          "[core][callbacks]") {
    Rig rig;

    for (size_t i = 0; i < kCap; ++i) {
        const auto id = static_cast<ParamRequestId>(i);
        const auto value = static_cast<uint16_t>(0x100 + i);
        REQUIRE(Seam::enqueue_param_result(rig.engine, id, ok_result(value)));
    }
    REQUIRE(rig.engine.dropped_deliveries() == 0);
    REQUIRE_FALSE(Seam::enqueue_param_result(rig.engine, 0xEE, ok_result(0xFFFF)));
    REQUIRE(rig.engine.dropped_deliveries() == 1);

    rig.engine.drain_callbacks();
    REQUIRE(rig.rec.param_calls == kCap);
    REQUIRE(rig.rec.ids[0] == 0);
    REQUIRE(rig.rec.values[0] == 0x100);
    REQUIRE(rig.rec.ids[kCap - 1] == static_cast<ParamRequestId>(kCap - 1));
    REQUIRE(rig.rec.values[kCap - 1] == static_cast<uint16_t>(0x100 + kCap - 1));
}

TEST_CASE("the two pending rings are independent", "[core][callbacks]") {
    Rig rig;

    SECTION("a full lifecycle ring does not refuse a param result") {
        for (size_t i = 0; i < kCap; ++i) {
            REQUIRE(Seam::enqueue_lifecycle(
                rig.engine, lifecycle(LifecycleKind::NodeDiscovered, static_cast<uint8_t>(i))));
        }
        REQUIRE_FALSE(
            Seam::enqueue_lifecycle(rig.engine, lifecycle(LifecycleKind::BusFault, 0xEE)));
        REQUIRE(rig.engine.dropped_deliveries() == 1);

        REQUIRE(Seam::enqueue_param_result(rig.engine, 3, ok_result(0x0303)));
        REQUIRE(rig.engine.dropped_deliveries() == 1); // unchanged by the successful enqueue
    }

    SECTION("a full param-result ring does not refuse a lifecycle event") {
        for (size_t i = 0; i < kCap; ++i) {
            const auto id = static_cast<ParamRequestId>(i);
            REQUIRE(Seam::enqueue_param_result(rig.engine, id, ok_result(id)));
        }
        REQUIRE_FALSE(Seam::enqueue_param_result(rig.engine, 0xEE, ok_result(0xFFFF)));
        REQUIRE(rig.engine.dropped_deliveries() == 1);

        REQUIRE(Seam::enqueue_lifecycle(rig.engine, lifecycle(LifecycleKind::NodeRemoved, 4)));
        REQUIRE(rig.engine.dropped_deliveries() == 1);
    }
}

TEST_CASE("a default drain delivers what was pending when it was called, not what a callback "
          "enqueues during it",
          "[core][callbacks]") {
    Rig rig;
    rig.rec.engine = &rig.engine;
    rig.rec.reentrant_enqueues_left = 1; // the first delivery pushes one more event back on

    REQUIRE(Seam::enqueue_lifecycle(rig.engine, lifecycle(LifecycleKind::NodeDiscovered, 1)));
    REQUIRE(Seam::enqueue_lifecycle(rig.engine, lifecycle(LifecycleKind::NodeDiscovered, 2)));

    rig.engine.drain_callbacks();

    // Two, not three: "everything CURRENTLY pending" (contracts/core-cpp.md §Engine) is the set
    // at entry, which bounds the loop by construction — an unbounded "until empty" loop would
    // deliver the re-entrant 0xEE here and, with a callback that always re-enqueues, never
    // return.
    REQUIRE(rig.rec.lifecycle_calls == 2);
    REQUIRE(rig.rec.node_ids[0] == 1);
    REQUIRE(rig.rec.node_ids[1] == 2);

    // The re-entrant event is queued, not lost: the next drain delivers it.
    rig.engine.drain_callbacks();
    REQUIRE(rig.rec.lifecycle_calls == 3);
    REQUIRE(rig.rec.node_ids[2] == 0xEE);
    REQUIRE(rig.rec.kinds[2] == LifecycleKind::BusFault);
}

TEST_CASE("an engine whose CoreCallbacks pointers are null consumes its pending deliveries "
          "without calling anything",
          "[core][callbacks]") {
    omgp_test::FakeClock clock;
    omgp_test::MockWire wire{clock};
    // CoreCallbacks' three members default to nullptr (core_types.hpp): a host that wants only
    // the polling loop constructs one. Delivering through a null pointer would be UB, so a
    // queued item is CONSUMED and nothing is invoked — the rings must not fill up instead.
    CoreEngine engine{wire, clock, omgp::ADDR_host, CoreCallbacks{}};

    for (size_t i = 0; i < kCap; ++i) {
        REQUIRE(Seam::enqueue_lifecycle(
            engine, lifecycle(LifecycleKind::NodeDiscovered, static_cast<uint8_t>(i))));
        REQUIRE(Seam::enqueue_param_result(engine, static_cast<ParamRequestId>(i), ok_result(0)));
    }
    engine.drain_callbacks();
    REQUIRE(engine.dropped_deliveries() == 0);

    // Both rings are empty again, so the next kCap enqueues all succeed and nothing is counted.
    for (size_t i = 0; i < kCap; ++i) {
        REQUIRE(Seam::enqueue_lifecycle(
            engine, lifecycle(LifecycleKind::NodeDiscovered, static_cast<uint8_t>(i))));
        REQUIRE(Seam::enqueue_param_result(engine, static_cast<ParamRequestId>(i), ok_result(0)));
    }
    REQUIRE(engine.dropped_deliveries() == 0);
}

TEST_CASE("a bounded drain with null CoreCallbacks consumes exactly max_deliveries, no more",
          "[core][callbacks]") {
    omgp_test::FakeClock clock;
    omgp_test::MockWire wire{clock};
    CoreEngine engine{wire, clock, omgp::ADDR_host, CoreCallbacks{}};

    // A full ring is the oracle here: with no callback to count, the only way to observe how many
    // deliveries a drain consumed is how much room it freed. An implementation that charged
    // `max_deliveries` only when a callback is non-null would empty the whole ring below.
    for (size_t i = 0; i < kCap; ++i) {
        REQUIRE(Seam::enqueue_lifecycle(
            engine, lifecycle(LifecycleKind::NodeDiscovered, static_cast<uint8_t>(i))));
    }
    REQUIRE(engine.dropped_deliveries() == 0);

    engine.drain_callbacks(2);

    // Exactly two slots free: two enqueues fit, the third is refused and counted (§8a).
    REQUIRE(Seam::enqueue_lifecycle(engine, lifecycle(LifecycleKind::BusFault, 0xE1)));
    REQUIRE(Seam::enqueue_lifecycle(engine, lifecycle(LifecycleKind::BusFault, 0xE2)));
    REQUIRE(engine.dropped_deliveries() == 0);
    REQUIRE_FALSE(Seam::enqueue_lifecycle(engine, lifecycle(LifecycleKind::BusFault, 0xE3)));
    REQUIRE(engine.dropped_deliveries() == 1);
}

TEST_CASE("neither enqueue nor drain_callbacks() reaches the allocator", "[core][callbacks]") {
    Rig rig;
    bool all_accepted = true;
    bool overflow_refused = false;

    // CLAUDE.md rule 5: no dynamic allocation in core/. HEAP_FREE_SCOPE
    // (tests/support/heap_guard.hpp) REQUIREs omgp_test::heap_calls unchanged across the scope;
    // a std::vector or std::function behind either path would fail on the count. No Catch2
    // assertion goes INSIDE the scope: the expression decomposition allocates, which would
    // charge the guard for the test's own bookkeeping — the outcomes are accumulated and
    // asserted after it.
    HEAP_FREE_SCOPE({
        for (size_t i = 0; i < kCap; ++i) {
            const auto id = static_cast<ParamRequestId>(i);
            const auto ev = lifecycle(LifecycleKind::NodeDiscovered, static_cast<uint8_t>(i));
            if (!Seam::enqueue_lifecycle(rig.engine, ev)) {
                all_accepted = false;
            }
            if (!Seam::enqueue_param_result(rig.engine, id, ok_result(id))) {
                all_accepted = false;
            }
        }
        // Including the refused (drop-newest) path.
        overflow_refused =
            !Seam::enqueue_lifecycle(rig.engine, lifecycle(LifecycleKind::BusFault, 0xEE));
    });
    REQUIRE(all_accepted);
    REQUIRE(overflow_refused);

    HEAP_FREE_SCOPE({
        rig.engine.drain_callbacks(2);
        rig.engine.drain_callbacks();
        rig.engine.drain_callbacks(); // and the empty-ring path
    });
    REQUIRE(rig.rec.lifecycle_calls == kCap);
    REQUIRE(rig.rec.param_calls == kCap);
    REQUIRE(rig.engine.dropped_deliveries() == 1);
}
