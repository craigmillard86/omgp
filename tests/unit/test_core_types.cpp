// core/core_types.hpp's one piece of real behaviour: Ring<T, N>, the fixed-capacity FIFO the
// demand rings (data-model.md §6) and the pending-delivery rings (§8a) are declared in terms of.
// The contract under test is §8a's drop-newest rule: a full ring refuses the NEWEST item and
// never evicts or reorders an older, possibly more urgent, notice. COUNTING a refusal
// (CoreEngine::dropped_deliveries_) is T015's, not this container's, so nothing here asserts a
// statistic.
// trunk §6: the rings exist because a superframe's outcomes are queued, not delivered inline.
// Spec: specs/003-host-core-engine/data-model.md §6, §8a; tasks.md T011 (ruling 2026-09-27 on
// issue #682: T011 carries its own Ring test; CoreEngine-level queue behaviour stays T014).
//
// This translation unit's only project include under test is core/core_types.hpp, and it comes
// FIRST in its own include block — clang-format never reorders across a blank line — so no other
// project header is included on its behalf. Compiling this file therefore demonstrates exactly
// one thing about includes: core_types.hpp needs no l3/, link/ or Catch2 header included first,
// under the native preset's -Wall -Wextra -Werror.
// It is NOT an oracle for the header's own <cstddef>/<cstdint> lines. core_types.hpp:15 includes
// the generated omgp_protocol.h first, and that header includes both itself
// (build/gen/omgp_protocol.h:6-7), so deleting either line from core_types.hpp would compile this
// TU identically — and per CLAUDE.md's working agreements, evidence whose output is unchanged when
// the claim is false supports nothing and must not be cited for it.
#include "core/core_types.hpp"

#include "catch_amalgamated.hpp"
#include "heap_guard.hpp"

using omgp::core::ParamRequestId;
using omgp::core::ParamResult;
using omgp::core::ParamResultDelivery;
using omgp::core::Ring;

TEST_CASE("a fresh Ring is empty, not full, and reports its template capacity", "[core]") {
    Ring<int, 4> r;
    STATIC_REQUIRE(Ring<int, 4>::capacity() == 4);
    REQUIRE(r.size() == 0);
    REQUIRE(r.empty());
    REQUIRE_FALSE(r.full());
}

TEST_CASE("pop on an empty Ring returns false and leaves the out parameter untouched", "[core]") {
    Ring<int, 4> r;
    int out = 0x5A;
    REQUIRE_FALSE(r.pop(out));
    REQUIRE(out == 0x5A); // an empty pop must not manufacture a value
    REQUIRE(r.empty());
}

TEST_CASE("Ring pops in FIFO order, oldest first", "[core]") {
    Ring<int, 4> r;
    for (int i = 1; i <= 4; ++i) {
        REQUIRE(r.push(i));
    }
    REQUIRE(r.size() == 4);
    REQUIRE(r.full());
    for (int expect = 1; expect <= 4; ++expect) {
        int out = -1;
        REQUIRE(r.pop(out));
        REQUIRE(out == expect);
    }
    REQUIRE(r.empty());
}

TEST_CASE("size/empty/full track each push and pop", "[core]") {
    Ring<int, 3> r;
    REQUIRE(r.push(10));
    REQUIRE(r.size() == 1);
    REQUIRE_FALSE(r.empty());
    REQUIRE_FALSE(r.full());
    REQUIRE(r.push(11));
    REQUIRE(r.push(12));
    REQUIRE(r.size() == 3);
    REQUIRE(r.full());
    int out = 0;
    REQUIRE(r.pop(out));
    REQUIRE(out == 10);
    REQUIRE(r.size() == 2);
    REQUIRE_FALSE(r.full());
}

// data-model.md §8a, the load-bearing rule: refuse the newest, keep every older item exactly as
// it was. An evict-oldest ring would pass the "push returns false" half of this and fail here.
TEST_CASE("push onto a full Ring refuses the newest item and mutates nothing", "[core]") {
    Ring<int, 3> r;
    REQUIRE(r.push(1));
    REQUIRE(r.push(2));
    REQUIRE(r.push(3));
    REQUIRE(r.full());

    REQUIRE_FALSE(r.push(99));
    REQUIRE_FALSE(r.push(98)); // a second refusal is equally inert
    REQUIRE(r.size() == 3);
    REQUIRE(r.full());

    // Contents unchanged AND unreordered: 1, 2, 3 — no 98/99 anywhere, 1 still at the front.
    for (int expect = 1; expect <= 3; ++expect) {
        int out = -1;
        REQUIRE(r.pop(out));
        REQUIRE(out == expect);
    }
    REQUIRE(r.empty());
}

TEST_CASE("a refused push leaves room for exactly one item once one is popped", "[core]") {
    Ring<int, 2> r;
    REQUIRE(r.push(1));
    REQUIRE(r.push(2));
    REQUIRE_FALSE(r.push(3));

    int out = -1;
    REQUIRE(r.pop(out));
    REQUIRE(out == 1);
    REQUIRE(r.push(3)); // the same item, offered again, now fits
    REQUIRE(r.full());
    REQUIRE(r.pop(out));
    REQUIRE(out == 2); // still FIFO across the refusal
    REQUIRE(r.pop(out));
    REQUIRE(out == 3);
    REQUIRE(r.empty());
}

// The modular index is the only arithmetic in the container: drive it past N so the write cursor
// wraps behind the read cursor and back.
TEST_CASE("Ring keeps FIFO order across the wrap point", "[core]") {
    Ring<int, 3> r;
    int out = -1;
    for (int i = 0; i < 20; ++i) {
        REQUIRE(r.push(i));
        REQUIRE(r.pop(out));
        REQUIRE(out == i); // one in, one out: the cursors advance together, wrapping every 3
    }
    REQUIRE(r.empty());

    // Now leave a resident item so the wrap happens with the ring partly occupied.
    REQUIRE(r.push(100));
    for (int i = 101; i < 120; ++i) {
        REQUIRE(r.push(i));
        REQUIRE(r.pop(out));
        REQUIRE(out == i - 1); // the older item always leaves first
    }
    REQUIRE(r.size() == 1);
    REQUIRE(r.pop(out));
    REQUIRE(out == 119);
}

TEST_CASE("Ring<T, 1> accepts one item, then refuses", "[core]") {
    Ring<int, 1> r;
    REQUIRE(r.push(7));
    REQUIRE(r.full());
    REQUIRE_FALSE(r.push(8));
    int out = -1;
    REQUIRE(r.pop(out));
    REQUIRE(out == 7);
    REQUIRE_FALSE(r.pop(out));
    REQUIRE(out == 7); // unchanged by the failed pop
}

// CLAUDE.md rule 5 / plan.md Constraints: no dynamic allocation in core/. The storage is one
// inline T[N] member, so neither construction nor any operation may reach the allocator.
TEST_CASE("Ring allocates nothing on construction, push, or pop", "[core]") {
    HEAP_FREE_SCOPE({
        Ring<ParamResultDelivery, 8> r;
        ParamResultDelivery d{};
        d.id = 3;
        d.result.ok = true;
        d.result.value = 0x1234;
        for (int i = 0; i < 8; ++i) {
            REQUIRE(r.push(d));
        }
        REQUIRE_FALSE(r.push(d));
        ParamResultDelivery got{};
        while (r.pop(got)) {
        }
    });
}

// The §8a shape the rings actually carry, at the capacity §8a specifies: drop-newest on a ring of
// real ParamResultDelivery items, with the payload preserved through the wrap.
TEST_CASE("Ring<ParamResultDelivery, LIMIT_max_nodes> is drop-newest at the §8a capacity",
          "[core]") {
    Ring<ParamResultDelivery, omgp::LIMIT_max_nodes> r;
    STATIC_REQUIRE(Ring<ParamResultDelivery, omgp::LIMIT_max_nodes>::capacity() ==
                   omgp::LIMIT_max_nodes);

    for (size_t i = 0; i < omgp::LIMIT_max_nodes; ++i) {
        ParamResultDelivery d{};
        d.id = static_cast<ParamRequestId>(i);
        d.result.ok = true;
        d.result.value = static_cast<uint16_t>(0x1000 + i);
        REQUIRE(r.push(d));
    }
    REQUIRE(r.full());

    ParamResultDelivery newest{};
    newest.id = 0xFF;
    newest.result.ok = false;
    newest.result.reason = 0xEE;
    REQUIRE_FALSE(r.push(newest));
    REQUIRE(r.size() == omgp::LIMIT_max_nodes);

    for (size_t i = 0; i < omgp::LIMIT_max_nodes; ++i) {
        ParamResultDelivery got{};
        REQUIRE(r.pop(got));
        REQUIRE(got.id == static_cast<ParamRequestId>(i)); // oldest first, none evicted
        REQUIRE(got.result.ok);
        REQUIRE(got.result.value == static_cast<uint16_t>(0x1000 + i));
    }
    REQUIRE(r.empty());
}
