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
// FIRST in its own include block, which the repo's formatter preserves rather than merges: the
// blank line stays a block boundary only because .clang-format sets no IncludeBlocks key and so
// inherits LLVM's `Preserve`. That is a CONTROL (the current contents of .clang-format), not a
// language guarantee — adding `IncludeBlocks: Merge`/`Regroup` there would merge the blocks and
// void this evidence with nothing failing. While it holds, no other
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

TEST_CASE("Ring::at reads the i-th item from the front, and clamps an out-of-range index "
          "[core][types]") {
    // Added with T029, whose demand drain peeks a ring's head without popping it (FR-005's
    // carry-over by identity) — the real callers are CoreEngine::rotate_past_demoted(),
    // drain_events() and drain_params(), none of which counts anything. Three things matter
    // and each is asserted: the index is from the FRONT and not from the buffer's base, so a
    // wrapped ring reads correctly; it is read-only, so it cannot reorder a FIFO; and an
    // out-of-range index returns the front rather than reading past the buffer.
    omgp::core::Ring<uint8_t, 4> ring;
    for (uint8_t i = 0; i < 4u; ++i) {
        REQUIRE(ring.push(static_cast<uint8_t>(10u + i)));
    }
    CHECK(ring.at(0) == 10u);
    CHECK(ring.at(1) == 11u);
    CHECK(ring.at(3) == 13u);

    SECTION("a WRAPPED ring still reads from the front") {
        // head_ is no longer 0, which is what tells `head_ + i` from `head_ - i` and from a
        // plain `i`: pop two and push two, so the front sits in the middle of the buffer.
        uint8_t out = 0;
        REQUIRE(ring.pop(out));
        REQUIRE(out == 10u);
        REQUIRE(ring.pop(out));
        REQUIRE(out == 11u);
        REQUIRE(ring.push(20u));
        REQUIRE(ring.push(21u));
        REQUIRE(ring.size() == 4u);
        CHECK(ring.at(0) == 12u);
        CHECK(ring.at(1) == 13u);
        CHECK(ring.at(2) == 20u);
        CHECK(ring.at(3) == 21u);
    }

    SECTION("an index at or past size() returns the front, and reads nothing out of range") {
        CHECK(ring.at(4) == ring.at(0));
        CHECK(ring.at(400) == ring.at(0));
    }

    SECTION("the clamp is at size(), not at capacity: a PARTLY filled ring clamps too") {
        // A full ring cannot show this: at capacity, `head_ + count_` wraps back to the front
        // anyway, so an index of exactly size() reads the right answer by accident. With two of
        // four slots used, index 2 is a real out-of-range index whose slot holds something else.
        omgp::core::Ring<uint8_t, 4> partial;
        REQUIRE(partial.push(70u));
        REQUIRE(partial.push(71u));
        REQUIRE(partial.size() == 2u);
        CHECK(partial.at(0) == 70u);
        CHECK(partial.at(1) == 71u);
        CHECK(partial.at(2) == 70u); // clamped to the front, not storage_[head_ + 2]
        CHECK(partial.at(3) == 70u);
    }

    SECTION("at() does not consume: the front is unchanged and pop() still yields it") {
        CHECK(ring.at(0) == 10u);
        CHECK(ring.size() == 4u);
        uint8_t out = 0;
        REQUIRE(ring.pop(out));
        CHECK(out == 10u);
    }
}
