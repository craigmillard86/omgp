// Trunk L2 Responder engine (spec 002 US3, T033). trunk §3 (media access — response
// scheduling inside the turnaround window) and §7 (retries, single-frame replay);
// contracts/link-cpp.md "Responder engine"; data-model.md §5 "Responder". Written from
// the C++ contract and spec.md User Story 3 (Acceptance Scenarios 1-5) and FR-014/015/
// 016/017 ahead of link/responder.{hpp,cpp} (T035) — this and its implementation land
// together per the maintainer's 2026-09-06 "one dispatch unit" ruling on #51/#53. Every
// assertion is driven through the scripted MockWire + FakeClock harness with simulated
// time advanced explicitly (CLAUDE.md rule 3): nothing here sleeps or reads a wall
// clock. Requests are placed on the wire via MockWire::inject_bytes() (a raw station's
// bytes, per contracts/mock-wire.md), not MockWire::transmit()/next_step(): this suite
// exercises the real Responder answering, not the mock's own scripted echo.
#include "catch_amalgamated.hpp"
#include "fake_clock.hpp"
#include "heap_guard.hpp"
#include "link/frame.hpp"
#include "link/link_types.hpp"
#include "link/responder.hpp"
#include "mock_wire.hpp"
#include "omgp_protocol.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace omgp::link;
using omgp_test::FakeClock;
using omgp_test::MockWire;

namespace {

// RequestHandler test double: echoes the request payload back and counts invocations —
// a retry replay must never bump `calls` (spec US3 AS2/FR-015).
struct RecordingHandler : RequestHandler {
    int calls = 0;
    // Set instead of REQUIRE'd inline: this can run inside a HEAP_FREE_SCOPE (whose own
    // REQUIRE would count against the guard) and on link/'s -fno-exceptions stack, where a
    // failing REQUIRE has no unwind support (review @033182a finding 3). Callers assert it
    // once poll() has returned.
    bool cap_violation = false;

    size_t handle(const uint8_t* req, size_t len, uint8_t* resp, size_t cap) override {
        ++calls;
        if (len > cap) {
            cap_violation = true;
            return 0;
        }
        if (len > 0)
            std::memcpy(resp, req, len);
        return len;
    }
};

// RequestHandler test double that misbehaves (contracts/link-cpp.md: a RequestHandler is
// application code, not a trusted part of the engine): claims to have written more bytes
// than its own `cap` -- which the Responder's own `resp_len > sizeof payload` check
// (link/responder.cpp, before the uint8_t cast) refuses, so encode_frame() is never even
// reached. Never writes past `cap` itself; only the returned count lies.
struct OversizedHandler : RequestHandler {
    int calls = 0;

    size_t handle(const uint8_t*, size_t, uint8_t*, size_t cap) override {
        ++calls;
        return cap + 1;
    }
};

// red-team @033182a finding 2: a handler that violates its own contract by returning more
// than `cap` (here, a value that wraps a uint8_t length cast back into range) -- a wider
// overflow than OversizedHandler's cap+1 above, which stays within a uint8_t.
struct OverlongHandler : RequestHandler {
    size_t ret = 0;
    int calls = 0;

    size_t handle(const uint8_t*, size_t, uint8_t* resp, size_t cap) override {
        ++calls;
        std::memset(resp, 0xEE, cap);
        return ret;
    }
};

uint64_t byte_us() {
    return byte_time_us(omgp::TRUNK_bit_rate);
}

// The bytes a real Master would transmit for a request to `dst` from `src`.
std::vector<uint8_t> request_bytes(uint8_t dst, uint8_t src, bool retry, uint8_t seq,
                                   const uint8_t* payload, size_t len) {
    FrameFields f{dst, src, /*response=*/false, retry, seq, static_cast<uint8_t>(len), payload};
    uint8_t out[kMaxWire];
    size_t written = 0;
    REQUIRE(encode_frame(f, out, sizeof out, written) == Status::Ok);
    return std::vector<uint8_t>(out, out + written);
}

// A request-shaped frame with one interior (payload) byte flipped so its CRC no longer
// matches, checked through an independent Deframer to land as exactly one
// Discard::BadCrc — mirrors test_link_master.cpp's crc_bad_response().
std::vector<uint8_t> crc_bad_request(uint8_t dst, uint8_t src, uint8_t seq, const uint8_t* payload,
                                     size_t len) {
    std::vector<uint8_t> bad = request_bytes(dst, src, false, seq, payload, len);
    REQUIRE(bad.size() > 6);
    bad[5] = static_cast<uint8_t>(bad[5] ^ 0x02);
    REQUIRE(bad[5] != omgp::TRUNK_flag_byte);
    REQUIRE(bad[5] != omgp::TRUNK_escape_byte);
    Deframer probe;
    FrameView v{};
    for (uint8_t b : bad)
        REQUIRE_FALSE(probe.feed(b, v));
    REQUIRE(probe.stats().discarded[static_cast<size_t>(Discard::BadCrc)] == 1);
    return bad;
}

// Places `bytes` on the wire as a raw station's transmission (contracts/mock-wire.md
// "inject_bytes"), one byte_time_us() apart starting at `start_us`; returns the instant
// its last byte ends (request_end).
uint64_t inject_request(MockWire& wire, const std::vector<uint8_t>& bytes, uint64_t start_us) {
    wire.inject_bytes(bytes.data(), bytes.size(), start_us);
    return start_us + static_cast<uint64_t>(bytes.size()) * byte_us();
}

constexpr uint8_t kPeer = omgp::ADDR_host; // the "master" address in these tests
constexpr uint8_t kMyAddr = 0x01;

} // namespace

// --- US3 AS1: happy path, T_turn_min timing --------------------------------------------

TEST_CASE("a default-constructed Responder transmits its response's first byte exactly "
          "at request_end + T_turn_min_us",
          "[link][timing:T_turn_min]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload[] = {0x11, 0x22, 0x33};
    const auto req = request_bytes(kMyAddr, kPeer, false, 0, payload, sizeof payload);
    const uint64_t request_end = inject_request(wire, req, 1000);
    const uint64_t deadline = request_end + omgp::TRUNK_T_turn_min_us;

    // Draining the request and deciding/encoding the answer is allocation-free
    // (CLAUDE.md rule 5) — nothing is due yet, one byte_time before the deadline.
    // wire.advance_to()'s own REQUIRE machinery may allocate, so only the engine call
    // itself (poll(), production code) is wrapped, not the test harness's clock step.
    wire.advance_to(deadline - 1);
    HEAP_FREE_SCOPE({ responder.poll(deadline - 1); });
    REQUIRE_FALSE(handler.cap_violation);
    REQUIRE(wire.transcript_size() == 0);
    REQUIRE(handler.calls == 1);

    // The respond half (wire_.transmit) is covered by its own guard too, not just the
    // handle-and-encode half above (review @033182a finding 2; precedent
    // test_link_master.cpp:2947-2953).
    wire.advance_to(deadline);
    HEAP_FREE_SCOPE({ responder.poll(deadline); });
    REQUIRE_FALSE(handler.cap_violation);
    REQUIRE(wire.transcript_size() == 1);
    const auto& tx = wire.transcript(0);
    REQUIRE(tx.tx_start_us == deadline);
    REQUIRE(tx.dst == kPeer);
    REQUIRE(tx.src == kMyAddr);
    REQUIRE(tx.response);
    REQUIRE_FALSE(tx.retry);
    REQUIRE(tx.seq == 0);
    REQUIRE(tx.len == sizeof payload);
    REQUIRE(std::memcmp(tx.payload, payload, sizeof payload) == 0);
    REQUIRE(responder.stats().late_responses == 0);
    REQUIRE(responder.stats().transactions == 1);
}

// --- turnaround_us clamp (data-model.md §5), verified via actual transmit time ---------

TEST_CASE("turnaround_us is clamped into [TRUNK_T_turn_min_us, TRUNK_T_turn_max_us]",
          "[link][timing:T_turn_max]") {
    SECTION("above T_turn_max clamps to T_turn_max") {
        FakeClock clock;
        MockWire wire(clock);
        RecordingHandler handler;
        Responder responder(wire, clock, handler, kMyAddr, omgp::TRUNK_T_turn_max_us + 1000);

        const uint8_t payload[] = {0xAA};
        const auto req = request_bytes(kMyAddr, kPeer, false, 0, payload, sizeof payload);
        const uint64_t request_end = inject_request(wire, req, 500);
        const uint64_t deadline = request_end + omgp::TRUNK_T_turn_max_us;

        wire.advance_to(deadline - 1, responder);
        REQUIRE(wire.transcript_size() == 0);
        wire.advance_to(deadline, responder);
        REQUIRE(wire.transcript_size() == 1);
        REQUIRE(wire.transcript(0).tx_start_us == deadline);
        // The value the caller asked for was never in range, so this attempt itself is
        // on-schedule for the CLAMPED deadline, not a late one (FR-014).
        REQUIRE(responder.stats().late_responses == 0);
    }

    SECTION("below T_turn_min clamps to T_turn_min") {
        FakeClock clock;
        MockWire wire(clock);
        RecordingHandler handler;
        Responder responder(wire, clock, handler, kMyAddr, omgp::TRUNK_T_turn_min_us - 1);

        const uint8_t payload[] = {0xAA};
        const auto req = request_bytes(kMyAddr, kPeer, false, 0, payload, sizeof payload);
        const uint64_t request_end = inject_request(wire, req, 500);
        const uint64_t deadline = request_end + omgp::TRUNK_T_turn_min_us;

        wire.advance_to(deadline - 1, responder);
        REQUIRE(wire.transcript_size() == 0);
        wire.advance_to(deadline, responder);
        REQUIRE(wire.transcript_size() == 1);
        REQUIRE(wire.transcript(0).tx_start_us == deadline);
    }
}

// --- US3 AS2/FR-015: replay ------------------------------------------------------------

TEST_CASE("a retry of the buffered sequence retransmits identical bytes without "
          "invoking the handler again",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload[] = {0x01, 0x02};
    uint64_t request_end =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 5, payload, sizeof payload), 0);
    uint64_t deadline = request_end + omgp::TRUNK_T_turn_min_us;
    wire.advance_to(deadline, responder);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(handler.calls == 1);
    const auto first = wire.transcript(0);

    // A retry of the same sequence, well after the first answer.
    const uint64_t retry_start = deadline + 200 * byte_us();
    request_end = inject_request(
        wire, request_bytes(kMyAddr, kPeer, true, 5, payload, sizeof payload), retry_start);
    deadline = request_end + omgp::TRUNK_T_turn_min_us;
    wire.advance_to(deadline, responder);

    REQUIRE(handler.calls == 1); // not invoked again
    REQUIRE(wire.transcript_size() == 2);
    const auto& replay = wire.transcript(1);
    REQUIRE(replay.tx_start_us == deadline);
    REQUIRE(replay.dst == first.dst);
    REQUIRE(replay.src == first.src);
    REQUIRE(replay.response == first.response);
    REQUIRE(replay.retry == first.retry); // identical bytes: the ORIGINAL retry bit
    REQUIRE(replay.seq == first.seq);
    REQUIRE(replay.len == first.len);
    REQUIRE(std::memcmp(replay.payload, first.payload, first.len) == 0);
    REQUIRE(responder.stats().replays_served == 1);
    REQUIRE(responder.stats().transactions == 1);
}

// --- US3 AS3/FR-016: a differing sequence is new ---------------------------------------

TEST_CASE("a request with a different sequence is treated as new, replacing the buffer", "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload1[] = {0x01};
    uint64_t end1 =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 3, payload1, sizeof payload1), 0);
    wire.advance_to(end1 + omgp::TRUNK_T_turn_min_us, responder);
    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 1);

    const uint64_t start2 = end1 + omgp::TRUNK_T_turn_min_us + 200 * byte_us();

    SECTION("retry bit set, different sequence") {
        const uint8_t payload2[] = {0x02};
        uint64_t end2 = inject_request(
            wire, request_bytes(kMyAddr, kPeer, true, 4, payload2, sizeof payload2), start2);
        wire.advance_to(end2 + omgp::TRUNK_T_turn_min_us, responder);
        REQUIRE(handler.calls == 2);
        REQUIRE(wire.transcript_size() == 2);
        REQUIRE(wire.transcript(1).seq == 4);
        REQUIRE(wire.transcript(1).len == sizeof payload2);
    }
    SECTION("retry bit clear, different sequence") {
        const uint8_t payload2[] = {0x03, 0x04};
        uint64_t end2 = inject_request(
            wire, request_bytes(kMyAddr, kPeer, false, 4, payload2, sizeof payload2), start2);
        wire.advance_to(end2 + omgp::TRUNK_T_turn_min_us, responder);
        REQUIRE(handler.calls == 2);
        REQUIRE(wire.transcript_size() == 2);
        REQUIRE(wire.transcript(1).seq == 4);
    }

    REQUIRE(responder.stats().replays_served == 0);
    REQUIRE(responder.stats().transactions == 2);
}

// --- US3 AS5: a retry-flagged request before any answer is new -------------------------

TEST_CASE("a retry-flagged request before any answer has ever been given is treated as new",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload[] = {0x09};
    uint64_t end = inject_request(
        wire, request_bytes(kMyAddr, kPeer, /*retry=*/true, 7, payload, sizeof payload), 0);
    wire.advance_to(end + omgp::TRUNK_T_turn_min_us, responder);

    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 1);
    // data-model.md §5 "retry echoed": this is the only AS5/AS3 case that feeds retry ==
    // 1 into a freshly-encoded (non-replay) response, so it is the only assertion in this
    // file a stale (never-echoed) retry bit would fail (red-team @907dfbe finding #2).
    REQUIRE(wire.transcript(0).retry);
    REQUIRE(responder.stats().replays_served == 0);
    REQUIRE(responder.stats().transactions == 1);
}

// --- red-team @907dfbe finding #2: retry bit must gate the replay decision -------------

TEST_CASE("a NEW request reusing the buffered sequence with the retry bit clear "
          "re-invokes the handler instead of replaying the stale buffer",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t p1[] = {0x01};
    uint64_t end = inject_request(wire, request_bytes(kMyAddr, kPeer, false, 5, p1, sizeof p1), 0);
    wire.advance_to(end + omgp::TRUNK_T_turn_min_us, responder);
    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 1);

    // Same sequence (5) as the buffered response, but the retry bit is CLEAR: FR-016
    // requires this be treated as new, not replayed — losing the `f.retry &&` conjunct
    // in the replay decision would serve the stale seq-5 answer instead.
    const uint8_t p2[] = {0x02};
    const uint64_t retry_start = end + omgp::TRUNK_T_turn_min_us + 200 * byte_us();
    end = inject_request(wire, request_bytes(kMyAddr, kPeer, false, 5, p2, sizeof p2), retry_start);
    wire.advance_to(end + omgp::TRUNK_T_turn_min_us, responder);

    REQUIRE(handler.calls == 2);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(wire.transcript(1).seq == 5);
    REQUIRE(wire.transcript(1).len == sizeof p2);
    REQUIRE(wire.transcript(1).payload[0] == 0x02); // the NEW payload, not the stale one
    REQUIRE(responder.stats().replays_served == 0);
    REQUIRE(responder.stats().transactions == 2);
}

// --- red-team @907dfbe finding #1: src == 0xFF can never be answered -------------------

TEST_CASE("a request claiming src == 0xFF is discarded rather than scheduling an "
          "unanswerable response",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    // encode_frame only refuses dst == 0xFF, never src, so this frame is otherwise
    // perfectly legal and the Deframer delivers it (link/frame.cpp:142 checks only
    // dst). A response's dst IS the request's src, so my_addr_ would have to encode a
    // frame with dst == 0xFF -- something encode_frame always refuses.
    const uint8_t p[] = {0xAA, 0xBB};
    uint64_t end = inject_request(wire, request_bytes(kMyAddr, 0xFF, false, 5, p, sizeof p), 0);
    wire.advance_to(end + omgp::TRUNK_T_turn_min_us, responder);

    REQUIRE(handler.calls == 0);
    REQUIRE(wire.transcript_size() == 0);
    REQUIRE(responder.stats().transactions == 0);
    REQUIRE(responder.stats().discards == 1);

    // FR-015: the replay buffer must stay untouched by the unanswerable frame, so a
    // legitimate later retry of the SAME sequence (from a real station) is answered
    // fresh rather than "replayed" as zero bytes.
    const uint64_t t = end + 200 * byte_us();
    end = inject_request(wire, request_bytes(kMyAddr, kPeer, true, 5, p, sizeof p), t);
    wire.advance_to(end + omgp::TRUNK_T_turn_min_us, responder);

    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).seq == 5);
    REQUIRE(wire.transcript(0).len == sizeof p);
    REQUIRE(responder.stats().replays_served == 0);
    REQUIRE(responder.stats().transactions == 1);
}

// --- red team + review @6c2fe4b finding 2: the whole source-address class, not just 0xFF --
// f.src is wire-derived and the Deframer validates dst only (link/frame.cpp:142). Trunk §5
// confines L2 addresses to ADDR_host .. ADDR_backplane_max (kAddrCount of them) and says
// module node IDs never appear as L2 trunk addresses; and no legitimate frame carries
// src == dst. Master::begin (link/master.cpp:87, dst >= kAddrCount) and HealthTracker's
// is_node_addr (link/health.cpp) already bound this class for the addresses THEY take from
// the wire; the Responder was the third reader of one and had no bound.

TEST_CASE("a request forging src == my_addr is discarded and counted -- the node never "
          "answers itself, and its replay slot is not evicted by the forgery",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    // A legitimate transaction first, so the forged frame has a replay entry to evict.
    const uint8_t p[] = {0x55};
    uint64_t end = inject_request(wire, request_bytes(kMyAddr, kPeer, false, 4, p, sizeof p), 0);
    wire.advance_to(end + omgp::TRUNK_T_turn_max_us, responder);
    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 1);

    // dst == src == my_addr. Before the fix this was accepted: the node spent a turnaround
    // window transmitting {dst = my_addr, src = my_addr, response = 1} to itself and
    // buffer_.peer became my_addr -- the {peer, seq} eviction primitive without
    // impersonating any real station (red team @6c2fe4b: transcript=1 discards=0).
    const uint8_t q[] = {0x66};
    uint64_t t = end + 200 * byte_us();
    end = inject_request(wire, request_bytes(kMyAddr, kMyAddr, false, 9, q, sizeof q), t);
    wire.advance_to(end + omgp::TRUNK_T_turn_max_us + 1000, responder);

    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(responder.stats().transactions == 1);
    REQUIRE(responder.stats().discards == 1);

    // FR-015 for the real station: its trunk §7 retry of seq 4 is still served from the
    // buffer, because the forged frame never reached it.
    t = end + 200 * byte_us();
    end = inject_request(wire, request_bytes(kMyAddr, kPeer, true, 4, p, sizeof p), t);
    wire.advance_to(end + omgp::TRUNK_T_turn_max_us, responder);

    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(responder.stats().replays_served == 1);
    REQUIRE(wire.transcript(1).dst == kPeer);
    REQUIRE(wire.transcript(1).seq == 4);
}

TEST_CASE("a request whose src is outside trunk §5's L2 address range is discarded and "
          "counted, at the boundary and beyond",
          "[link]") {
    // kAddrCount itself (0x10, the first value that is not an L2 trunk address) pins the
    // guard's `>=` against `>`; 0xFE is the far end below the 0xFF the case above covers.
    static_assert(kAddrCount <= 0xFE, "the boundary value must itself lie outside the range");
    const uint8_t src = GENERATE(static_cast<uint8_t>(kAddrCount), static_cast<uint8_t>(0xFE));

    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t p[] = {0x77};
    const uint64_t end =
        inject_request(wire, request_bytes(kMyAddr, src, false, 2, p, sizeof p), 0);
    wire.advance_to(end + omgp::TRUNK_T_turn_max_us + 1000, responder);

    INFO("src=" << int(src));
    REQUIRE(handler.calls == 0);
    REQUIRE(wire.transcript_size() == 0);
    REQUIRE(responder.stats().transactions == 0);
    REQUIRE(responder.stats().discards == 1);
}

// --- a genuine in-flight conflict: the second request lands strictly before the ------
// --- first response is due, so transmit_if_due() cannot have flushed it first --------

TEST_CASE("a second request arriving inside the first response's turnaround window is "
          "held in the wire's receive queue, and answered after the first response",
          "[link]") {
    // Red team @e510b29 finding 1 (BLOCKING) / review @e510b29 finding 1: on_request()
    // used to DISCARD any accepted request that landed while a response was Scheduled or
    // Transmitting. data-model.md Section 5 is silent on arrival while busy, and FR-015 /
    // FR-016 make "replay" / "treat as new" unconditional MUSTs -- so the engine now
    // stops draining the wire while a response is pending and picks the bytes up, intact,
    // once the wire is free (a UART's RX FIFO does exactly this). Nothing is discarded;
    // docs/OPEN-QUESTIONS.md 2026-09-06 records the reading.
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    // The largest allowed turnaround leaves the most room for request B's own bytes to
    // arrive before request A's response is due (see the timing check below).
    Responder responder(wire, clock, handler, kMyAddr, omgp::TRUNK_T_turn_max_us);

    const uint8_t a[] = {0xA1};
    uint64_t end_a = inject_request(wire, request_bytes(kMyAddr, kPeer, false, 1, a, sizeof a), 0);
    // Drains A only: now_us == end_a, strictly before deadline_a (end_a + turnaround), so
    // transmit_if_due() cannot have fired yet -- state_ is left Scheduled.
    wire.advance_to(end_a, responder);
    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 0);
    REQUIRE(responder.stats().transactions == 1);
    const uint64_t deadline_a = end_a + omgp::TRUNK_T_turn_max_us;

    // Request B: zero-length payload keeps its own wire time short, so it fully arrives
    // well before deadline_a even at the maximum turnaround.
    const uint64_t t2 = end_a + byte_us();
    const std::vector<uint8_t> b_bytes = request_bytes(kMyAddr, 0x07, false, 2, nullptr, 0);
    uint64_t end_b = inject_request(wire, b_bytes, t2);
    REQUIRE(end_b < deadline_a); // otherwise this test would not exercise the conflict

    // Drains nothing: A's response is Scheduled and not yet due, so B's bytes stay queued
    // -- B's handler is not invoked, nothing is transmitted, nothing is discarded.
    wire.advance_to(end_b, responder);
    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 0);
    REQUIRE(responder.stats().transactions == 1);
    REQUIRE(responder.stats().discards == 0);

    // A's response lands exactly at its own deadline, B still untouched behind it.
    wire.advance_to(deadline_a, responder);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).seq == 1);
    REQUIRE(wire.transcript(0).tx_start_us == deadline_a);
    REQUIRE(handler.calls == 1);
    // A's response occupies the wire until tx_end_a; B is decoded the instant it is free,
    // and its own response is scheduled from B's request_end -- inside its own window here,
    // so it is NOT counted late (FR-014 counts a violation, not a queue).
    const uint64_t deadline_b = end_b + omgp::TRUNK_T_turn_max_us;
    wire.advance_to(deadline_b, responder);
    REQUIRE(handler.calls == 2);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(wire.transcript(1).seq == 2);
    REQUIRE(wire.transcript(1).dst == 0x07);
    REQUIRE(wire.transcript(1).tx_start_us == deadline_b);
    REQUIRE(responder.stats().transactions == 2);
    REQUIRE(responder.stats().late_responses == 0);
    REQUIRE(responder.stats().discards == 0);
}

// --- red-team @e510b29 finding 1 (BLOCKING): requests queued ahead of one late poll ----
// --- are each answered in turn -- none discarded, each counted late (FR-014/015/016) ---

TEST_CASE("two requests queued before one late poll are both answered, in order, each "
          "counted late, the second the instant the wire is free",
          "[link][timing:T_turn_max]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t a[] = {0xA1};
    const uint8_t b[] = {0xB2};
    uint64_t end1 = inject_request(wire, request_bytes(kMyAddr, kPeer, false, 1, a, sizeof a), 0);
    // Request 2 from the same master, a full T_resp + T_gap after request 1 ended (the
    // tightest spacing the Master itself permits, red team @e510b29). Neither request is
    // drained until the single advance_to() below, well after both windows have closed:
    // a late poll() (FR-014), doubled.
    const uint64_t t2 = end1 + omgp::TRUNK_T_resp_us + omgp::TRUNK_T_gap_us;
    uint64_t end2 = inject_request(wire, request_bytes(kMyAddr, kPeer, false, 2, b, sizeof b), t2);
    const uint64_t late_now = end2 + omgp::TRUNK_T_turn_max_us + 1000;

    wire.advance_to(late_now, responder);

    // Response 1 goes out at once and is counted late. Request 2's bytes are NOT drained
    // past it: the wire is occupied until response 1's final stop bit, so they wait in the
    // receive queue -- not discarded, not yet handled.
    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).seq == 1);
    REQUIRE(wire.transcript(0).tx_start_us == late_now);
    REQUIRE(responder.stats().late_responses == 1);
    REQUIRE(responder.stats().discards == 0);

    // The exact instant transmit() said the wire is free again (ByteWire: "the instant of
    // the final stop bit"): request 2 is decoded, handled, and -- its own window long
    // closed -- answered immediately, counted late. Exactly at tx_end, not a poll later
    // (deep-verify @e510b29: `now_us < transmit_until_us_` survived as `<=`).
    const std::vector<uint8_t> resp1 =
        request_bytes(kPeer, kMyAddr, false, 1, a, sizeof a); // same length as response 1
    const uint64_t tx_end1 = late_now + static_cast<uint64_t>(resp1.size()) * byte_us();
    // ... and not one microsecond before it: a poll() landing inside response 1's
    // transmission must find the wire occupied and leave request 2 in the queue (trunk §3
    // half-duplex; deep-verify @70d7660: `transmit_until_us_ = wire_.transmit(...)`
    // replaced by a constant survived -- Transmitting collapsed to "cleared by the next
    // poll()" and nothing here polled inside the transmission to notice).
    wire.advance_to(tx_end1 - 1, responder);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(handler.calls == 1);
    wire.advance_to(tx_end1, responder);
    REQUIRE(handler.calls == 2);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(wire.transcript(1).seq == 2);
    REQUIRE(wire.transcript(1).tx_start_us == tx_end1);
    REQUIRE(responder.stats().transactions == 2);
    REQUIRE(responder.stats().late_responses == 2);
    REQUIRE(responder.stats().discards == 0);
}

TEST_CASE("a retry queued in the same late-poll batch as its original is replayed, not "
          "dropped",
          "[link]") {
    // Red team @e510b29 finding 1, the FR-015 half: a trunk Section 7 retry that lands
    // behind its own original ahead of one late poll() must still be served from the
    // replay buffer -- byte for byte, handler not invoked again.
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t p[] = {0xC3};
    uint64_t end1 = inject_request(wire, request_bytes(kMyAddr, kPeer, false, 5, p, sizeof p), 0);
    uint64_t end2 = inject_request(wire, request_bytes(kMyAddr, kPeer, true, 5, p, sizeof p),
                                   end1 + omgp::TRUNK_T_resp_us);
    const uint64_t late_now = end2 + omgp::TRUNK_T_turn_max_us + 1000;

    wire.advance_to(late_now, responder);
    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(responder.stats().replays_served == 0);
    REQUIRE(responder.stats().discards == 0);

    // Well after response 1 has left the wire: the retry is drained and replayed.
    wire.advance_to(late_now + 1000, responder);
    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(responder.stats().replays_served == 1);
    REQUIRE(responder.stats().transactions == 1);
    REQUIRE(responder.stats().discards == 0);
    // data-model.md Section 5 "retransmit buffer": the exact bytes already sent, so the
    // replay is field-for-field the first response (its retry bit included: the original
    // was answered to a retry=0 request, and the buffer is not re-encoded).
    const MockWire::TxRecord& first = wire.transcript(0);
    const MockWire::TxRecord& replay = wire.transcript(1);
    REQUIRE(replay.dst == first.dst);
    REQUIRE(replay.response);
    REQUIRE(replay.retry == first.retry);
    REQUIRE(replay.seq == 5);
    REQUIRE(replay.len == 1);
    REQUIRE(replay.payload[0] == 0xC3);
}

TEST_CASE("eight well-spaced requests are all answered whatever the poll cadence, none "
          "discarded",
          "[link]") {
    // The red team's own sweep (@e510b29 finding 1): at 400 us and coarser, requests
    // were silently swallowed -- 2 of 8 answered at a 2 ms poll period. Expected by
    // FR-014: all eight, late ones counted, nothing dropped.
    const uint8_t p[] = {0x5A};
    for (uint64_t period : {50u, 400u, 1000u, 2000u}) {
        DYNAMIC_SECTION("poll period " << period << " us") {
            FakeClock clock;
            MockWire wire(clock);
            RecordingHandler handler;
            Responder responder(wire, clock, handler, kMyAddr);
            uint64_t t = 1000, last_end = 0;
            for (int i = 0; i < 8; ++i) {
                last_end = inject_request(
                    wire,
                    request_bytes(kMyAddr, kPeer, false, static_cast<uint8_t>(i), p, sizeof p), t);
                t = last_end + omgp::TRUNK_T_resp_us + omgp::TRUNK_T_gap_us;
            }
            for (uint64_t now = period; now <= last_end + 20000; now += period)
                wire.advance_to(now, responder);
            REQUIRE(handler.calls == 8);
            REQUIRE(wire.transcript_size() == 8);
            for (size_t i = 0; i < 8; ++i)
                REQUIRE(wire.transcript(i).seq == i);
            REQUIRE(responder.stats().transactions == 8);
            REQUIRE(responder.stats().discards == 0);
        }
    }
}

// --- US3 AS4/FR-017: wrong address / corrupt frame -> discard, no transmission --------

TEST_CASE("a frame for another address, and a corrupt frame, are discarded and never "
          "transmitted",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    SECTION("addressed to a different node") {
        const uint8_t payload[] = {0x01};
        uint64_t end = inject_request(wire,
                                      request_bytes(static_cast<uint8_t>(kMyAddr + 1), kPeer, false,
                                                    0, payload, sizeof payload),
                                      0);
        wire.advance_to(end + omgp::TRUNK_T_turn_max_us, responder);
        REQUIRE(handler.calls == 0);
        REQUIRE(wire.transcript_size() == 0);
        REQUIRE(responder.stats().discards == 1);
    }

    SECTION("corrupt (bad-CRC) frame") {
        const uint8_t payload[] = {0x01, 0x02};
        uint64_t end =
            inject_request(wire, crc_bad_request(kMyAddr, kPeer, 0, payload, sizeof payload), 0);
        wire.advance_to(end + omgp::TRUNK_T_turn_max_us, responder);
        REQUIRE(handler.calls == 0);
        REQUIRE(wire.transcript_size() == 0);
        REQUIRE(responder.stats().discards == 1);
    }
}

// The response-bit half of the acceptance guard (data-model.md §5 "Request acceptance";
// FR-014 "only in response to an intact frame addressed to it"): a RESPONSE frame that
// happens to carry this node's address as dst -- an echo, a misbehaving station, or
// another Responder's answer on a shared trunk -- is not a request and is never answered.
// Two Responders answering each other's responses would babble indefinitely. The suite's
// own request_bytes() hard-codes response=false, so this is the only case that exercises
// the term (red team @70d7660 finding 1: `f.response ||` deleted survived every case).
TEST_CASE("a RESPONSE-bit frame addressed to this node is never answered", "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t p[] = {0xA5};
    const FrameFields f{kMyAddr, kPeer, /*response=*/true, false, 3, static_cast<uint8_t>(sizeof p),
                        p};
    uint8_t out[kMaxWire];
    size_t written = 0;
    REQUIRE(encode_frame(f, out, sizeof out, written) == Status::Ok);
    const uint64_t end = inject_request(wire, std::vector<uint8_t>(out, out + written), 0);

    wire.advance_to(end + omgp::TRUNK_T_turn_max_us + 1000, responder);

    REQUIRE(handler.calls == 0);
    REQUIRE(wire.transcript_size() == 0);
    REQUIRE(responder.stats().transactions == 0);
    REQUIRE(responder.stats().discards == 1);
}

// A second, independent byte-level (deframer) discard must still be counted: a naive
// running total that only ever goes up by coincidence on the FIRST discard (e.g. summing
// the wrong way) can look identical to a correct one until a second discard exposes it.
TEST_CASE("a second byte-level discard is counted on top of the first, not lost", "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload1[] = {0x01, 0x02};
    uint64_t end1 =
        inject_request(wire, crc_bad_request(kMyAddr, kPeer, 0, payload1, sizeof payload1), 0);
    wire.advance_to(end1 + omgp::TRUNK_T_turn_max_us, responder);
    REQUIRE(responder.stats().discards == 1);

    const uint8_t payload2[] = {0x03, 0x04, 0x05};
    const uint64_t start2 = end1 + omgp::TRUNK_T_turn_max_us + 200 * byte_us();
    uint64_t end2 =
        inject_request(wire, crc_bad_request(kMyAddr, kPeer, 1, payload2, sizeof payload2), start2);
    wire.advance_to(end2 + omgp::TRUNK_T_turn_max_us, responder);

    REQUIRE(responder.stats().discards == 2);
    REQUIRE(handler.calls == 0);
    REQUIRE(wire.transcript_size() == 0);
}

// --- a misbehaving RequestHandler's oversized response is refused, not transmitted ----

TEST_CASE("a RequestHandler that claims more bytes than its own cap is discarded rather "
          "than transmitted",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    OversizedHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload[] = {0x01};
    uint64_t end =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 3, payload, sizeof payload), 0);
    wire.advance_to(end + omgp::TRUNK_T_turn_min_us, responder);

    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 0); // refused before encoding: nothing to transmit
    REQUIRE(responder.stats().transactions == 0);
    REQUIRE(responder.stats().discards == 1);
}

// --- idle wire: nothing transmitted -----------------------------------------------------

TEST_CASE("poll() with nothing addressed to the node transmits nothing", "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    wire.advance_to(1000, responder);
    REQUIRE(wire.transcript_size() == 0);
    REQUIRE(handler.calls == 0);
    REQUIRE(responder.stats().transactions == 0);
    REQUIRE(responder.stats().discards == 0);
}

// --- FR-014: late poll ------------------------------------------------------------------

TEST_CASE("a first poll() reached only after request_end + T_turn_max transmits at once "
          "and counts a late response",
          "[link][timing:T_turn_max]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload[] = {0x01};
    uint64_t request_end =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 0, payload, sizeof payload), 0);
    const uint64_t late_now = request_end + omgp::TRUNK_T_turn_max_us + 1000;

    // No intermediate poll(): the FIRST call after the request lands well past T_turn_max.
    wire.advance_to(late_now, responder);

    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).tx_start_us == late_now);
    REQUIRE(responder.stats().late_responses == 1);
}

// FR-014's late bound is the spec's OUTER T_turn_max, not this engine's own (default:
// T_turn_min) deadline. A poll() landing strictly between the two is late relative to the
// engine's configured turnaround yet fully inside the spec window: the response goes out
// at that instant and is NOT counted late. Every other late_responses assertion either
// polls exactly at the deadline or constructs the engine with turnaround == T_turn_max
// (deadline == late bound), so only this case separates the two operands (red team
// @70d7660 finding 2: `late_bound_us = deadline_us_` survived every case).
TEST_CASE("a response inside the window but after the engine's own deadline is not late",
          "[link][timing:T_turn_max]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr); // default turnaround == T_turn_min

    const uint8_t p[] = {0x11};
    const uint64_t end =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 4, p, sizeof p), 0);

    const uint64_t inside = end + (omgp::TRUNK_T_turn_min_us + omgp::TRUNK_T_turn_max_us) / 2;
    REQUIRE(inside > end + omgp::TRUNK_T_turn_min_us);
    REQUIRE(inside < end + omgp::TRUNK_T_turn_max_us);
    // No intermediate poll(): the first call after the request lands past the engine's own
    // deadline but inside the spec window.
    wire.advance_to(inside, responder);

    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).tx_start_us == inside);
    REQUIRE(responder.stats().late_responses == 0);
}

// --- transmitted byte count must match the actual encoded frame, not a fixed size -----

TEST_CASE("responses at very different payload lengths are each transmitted intact, in "
          "sequence",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    // A 1-byte and a maximum-length payload force very different encoded byte counts: a
    // fixed (wrong) wire length for the response buffer can only coincidentally match one
    // of the two, and a too-long first response also leaves trailing garbage that the
    // independent MockWire parser would otherwise fold into the second frame.
    const uint8_t payload1[] = {0xAB};
    uint64_t end1 =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 0, payload1, sizeof payload1), 0);
    wire.advance_to(end1 + omgp::TRUNK_T_turn_min_us, responder);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).len == sizeof payload1);
    REQUIRE(std::memcmp(wire.transcript(0).payload, payload1, sizeof payload1) == 0);

    uint8_t payload2[omgp::LIMIT_max_l3_payload];
    for (size_t i = 0; i < sizeof payload2; ++i)
        payload2[i] = static_cast<uint8_t>(i);
    const uint64_t start2 = end1 + omgp::TRUNK_T_turn_min_us + 200 * byte_us();
    uint64_t end2 = inject_request(
        wire, request_bytes(kMyAddr, kPeer, false, 1, payload2, sizeof payload2), start2);
    wire.advance_to(end2 + omgp::TRUNK_T_turn_min_us, responder);

    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(wire.transcript(1).len == sizeof payload2);
    REQUIRE(std::memcmp(wire.transcript(1).payload, payload2, sizeof payload2) == 0);
}

// --- state leaves Scheduled once sent: no repeat transmission on later poll()s --------

TEST_CASE("after transmitting, later poll() calls do not retransmit absent a new request",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload[] = {0x07};
    uint64_t request_end =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 0, payload, sizeof payload), 0);
    const uint64_t deadline = request_end + omgp::TRUNK_T_turn_min_us;

    wire.advance_to(deadline, responder);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(handler.calls == 1);

    // Nothing new arrives; deadline_us_ is unchanged and now_us only grows, so a state_
    // that failed to leave Scheduled would retransmit the same buffered response again.
    wire.advance_to(deadline + 500 * byte_us(), responder);
    wire.advance_to(deadline + 5000 * byte_us(), responder);

    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(handler.calls == 1);
}

// --- red-team @033182a finding #2: a handler return value >= 256 must be refused, not --
// --- silently truncated by the uint8_t length cast ------------------------------------

TEST_CASE("a RequestHandler returning 256 or more is refused, not truncated by the "
          "uint8_t response-length cast",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    OverlongHandler handler;
    // static_cast<uint8_t>(256) wraps to 0 -- a perfectly legal length -- unless the
    // handler's return value is checked against its own `cap` before that cast runs.
    handler.ret = 256;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload[] = {0x01, 0x02, 0x03};
    uint64_t end =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 0, payload, sizeof payload), 0);
    wire.advance_to(end + omgp::TRUNK_T_turn_min_us, responder);

    REQUIRE(handler.calls == 1);
    REQUIRE(responder.stats().discards == 1);
    REQUIRE(wire.transcript_size() == 0);
    REQUIRE(responder.stats().transactions == 0);
}

// --- red-team @033182a finding #3: a retry from a different station must not replay ---
// --- the previous requester's buffered answer ------------------------------------------

TEST_CASE("a retry-flagged request from a different station is treated as new rather "
          "than replaying the previous requester's buffered answer",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t p[] = {0x5A};
    uint64_t end1 = inject_request(wire, request_bytes(kMyAddr, kPeer, false, 5, p, sizeof p), 0);
    wire.advance_to(end1 + omgp::TRUNK_T_turn_min_us, responder);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).dst == kPeer);

    // A different station (0x07) happens to send retry=1 with the same sequence (5): the
    // ReplayBuffer is keyed on sequence alone unless the buffered requester is also
    // checked (docs/OPEN-QUESTIONS.md 2026-09-06).
    const uint8_t q[] = {0x99};
    const uint64_t start2 = end1 + omgp::TRUNK_T_turn_min_us + 200 * byte_us();
    uint64_t end2 =
        inject_request(wire, request_bytes(kMyAddr, 0x07, true, 5, q, sizeof q), start2);
    wire.advance_to(end2 + omgp::TRUNK_T_turn_min_us, responder);

    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(wire.transcript(1).dst == 0x07); // answered to the station that actually asked
    REQUIRE(handler.calls == 2);             // treated as new, not replayed
    REQUIRE(responder.stats().replays_served == 0);
}

// --- red-team @d0cc3bf finding #2: a request duplicated with the retry bit CLEAR is ----
// --- NOT covered by the replay buffer -- it re-invokes the handler a second time. ------
// Pins today's (open-question) behaviour; does not assert it is the desired outcome --
// see docs/OPEN-QUESTIONS.md 2026-09-06 ("RequestHandler's 'called at most once per NEW
// sequence' doc claim overclaims..."). `RequestHandler::handle()`'s own doc comment in
// responder.hpp is scoped to retry-bit-SET repeats precisely because of this case.

TEST_CASE("a request duplicated on the wire with the retry bit clear re-invokes the "
          "handler a second time (open question, not asserted as desired)",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t p[] = {0x5A};
    const auto req = request_bytes(kMyAddr, kPeer, /*retry=*/false, 9, p, sizeof p);
    uint64_t end1 = inject_request(wire, req, 0);
    wire.advance_to(end1 + omgp::TRUNK_T_turn_min_us, responder);
    REQUIRE(handler.calls == 1);
    REQUIRE(wire.transcript_size() == 1);

    // The exact same bytes appear again -- a wire-level duplicate, retry bit still clear --
    // shortly after, well inside the window the buffered response could have replayed from.
    const uint64_t start2 = end1 + omgp::TRUNK_T_turn_min_us + 200 * byte_us();
    uint64_t end2 = inject_request(wire, req, start2);
    wire.advance_to(end2 + omgp::TRUNK_T_turn_min_us, responder);

    CHECK(handler.calls == 2);          // NOT once: the retry-bit gate misses this
    CHECK(wire.transcript_size() == 2); // a second response goes out unsolicited
    CHECK(responder.stats().replays_served == 0);
}

TEST_CASE("requests arriving faster than poll() is called queue up behind the held-request "
          "rule: one answer per poll(), all of them late, the host's own request never "
          "reached, nothing counted (open question, not asserted as desired)",
          "[link]") {
    // Red team @17554c8 finding 1 (MEDIUM), confirmed by review @17554c8 finding 1, re-run
    // here with the suite's own helpers. "Held, not discarded" (docs/OPEN-QUESTIONS.md
    // 2026-09-06) plus "one accepted request leaves Listening and ends the drain" means a
    // poll() answers exactly ONE queued request, oldest first, with no bound on the
    // backlog's age or depth. A station at 0x07 puts one request to this node on the bus
    // every 300 us; the engine is polled every 1 ms; the host sends its own request at
    // t = 8 ms. Pre-`70d7660` (drain always, discard-and-count while busy) the host was
    // answered at the next poll and the dropped traffic was counted (discards == 48 in the
    // red team's run); at this head neither happens. FR-014 (a late poll MUST still
    // transmit) and FR-017 (MUST never transmit outside a response window) pull opposite
    // ways on a stale queued request; the ruling is the maintainer's -- these CHECKs pin
    // today's observable so the ruling's implementation starts from a failing test.
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    constexpr uint8_t kOther = 0x07;
    const uint8_t flood_payload[] = {0x5A};
    const uint8_t host_payload[] = {0xC3};
    const uint64_t poll_period = 1000, flood_period = 300, host_at = 8000;
    uint64_t next_flood = 0, host_end = 0;
    int flooded = 0;
    bool host_sent = false;
    for (uint64_t t = 0; t <= 20 * poll_period; t += poll_period) {
        // Everything the other station will have put on the bus before the next poll.
        while (next_flood < t + poll_period) {
            inject_request(wire,
                           request_bytes(kMyAddr, kOther, false,
                                         static_cast<uint8_t>(flooded & 0x0F), flood_payload,
                                         sizeof flood_payload),
                           next_flood);
            ++flooded;
            next_flood += flood_period;
        }
        if (!host_sent && t >= host_at) {
            host_end = inject_request(
                wire, request_bytes(kMyAddr, kPeer, false, 9, host_payload, sizeof host_payload),
                t);
            host_sent = true;
        }
        wire.advance_to(t, responder);
    }
    REQUIRE(host_sent);
    REQUIRE(flooded == 70);

    bool answered_host = false;
    for (size_t i = 0; i < wire.transcript_size(); ++i)
        if (wire.transcript(i).dst == kPeer)
            answered_host = true;

    // 7 answers over 21 polls, not one per poll. Two rules compose here, and both are
    // consequences of the engine refusing to transmit on a belief it cannot support:
    //   - on the FR-014 late path it drains the whole receive queue before judging the bus
    //     (red team @71caba0 / @7d31410), so on a bus this busy the last byte drained at a
    //     poll instant ENDS after that instant and the T_gap rule defers the answer -- this
    //     alone gave 17, and is Master's own documented behaviour under continuous traffic
    //     (link/master.cpp:215-235);
    //   - once the stash is full the drain stops taking bytes at all (it must: a byte it
    //     cannot hold would already have been consumed, and lost -- red team @b262d46), so
    //     last_activity_us_ stops being refreshed and says nothing further about the bus.
    //     The engine then waits out the bounded cap rather than acting on a frozen belief,
    //     which is what makes this cell 7 rather than 17.
    // A LATENCY change, not a new starvation: the backlog still grows without bound and the
    // host is still never reached, which is what this case pins. Previous pins: 20/20/0 at
    // 71caba0 (stop draining at the first decoded request), 17/17/1 at b262d46 (drain
    // everything, but a byte lost at the stash bound). The alternative -- transmitting at
    // last_activity + T_gap while blind -- restores 17 at the cost of keying down inside a
    // frame the engine has not read, which is the defect this PR was reopened to fix.
    // docs/OPEN-QUESTIONS.md 2026-09-06 (FR-014 vs FR-017) still governs the desired
    // behaviour; this is a pin of today's, not a claim about what it should be.
    CHECK(wire.transcript_size() == 7);
    CHECK(handler.calls == 7);
    CHECK_FALSE(answered_host); // 12 ms after host_end, and still queued behind 0x07's frames
    CHECK(responder.stats().late_responses == 7);
    // Nothing is discarded: no byte is consumed that cannot be kept, so the backlog is still
    // entirely invisible in stats() -- which is the open question.
    CHECK(responder.stats().discards == 0);
    CHECK(host_end + omgp::TRUNK_T_resp_us < 20 * poll_period); // the host gave up long ago
}

// --- red team @71caba0 finding 1 (BLOCKING, HIGH): listen before transmitting ------------
// --- Inside its response window the engine owns the bus by protocol (trunk §3: the host --
// --- is the only initiator and is waiting out T_resp, "a node must never transmit -------
// --- outside its response window"). Once that window has closed -- the FR-014 late-poll --
// --- path -- the guarantee is gone, so the engine must form the belief byte_wire.hpp ----
// --- requires of it ("never transmits while it believes the bus is busy") from what is ---
// --- actually on the wire, exactly as Master::fire_pending does (link/master.cpp:205). ---

TEST_CASE("a response already outside its window is not keyed down on top of another "
          "station's in-flight frame: it waits for T_gap of idle after that frame",
          "[link][timing:T_gap]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t p[] = {0x11};
    const uint64_t request_end =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 1, p, sizeof p), 1000);

    // This node polls once per superframe (T_poll = 2 ms), the cadence FR-014 exists for.
    // The host timed out after T_resp and is now polling a DIFFERENT node; that request
    // occupies the bus while this node's own answer is still due.
    const uint64_t other_start = request_end + 1000;
    const std::vector<uint8_t> other = request_bytes(0x02, kPeer, false, 2, p, sizeof p);
    const uint64_t other_end = inject_request(wire, other, other_start);
    const uint64_t mid_frame = other_start + (other_end - other_start) / 2;
    REQUIRE(mid_frame > request_end + omgp::TRUNK_T_turn_max_us); // the window has closed

    wire.advance_to(mid_frame, responder);
    // Nothing on the wire: half of another station's frame is in flight (trunk §3 is
    // half-duplex), and the engine has drained those bytes rather than staying blind to
    // them, so it knows.
    REQUIRE(wire.transcript_size() == 0);
    REQUIRE(handler.calls == 1); // the response is encoded and pending, just not sent

    // The frame ends; the engine still owes it T_gap of idle (data-model.md §4 "Gap").
    wire.advance_to(other_end, responder);
    REQUIRE(wire.transcript_size() == 0);
    wire.advance_to(other_end + omgp::TRUNK_T_gap_us - 1, responder);
    REQUIRE(wire.transcript_size() == 0);

    wire.advance_to(other_end + omgp::TRUNK_T_gap_us, responder);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).tx_start_us == other_end + omgp::TRUNK_T_gap_us);
    REQUIRE(wire.transcript(0).dst == kPeer);
    REQUIRE(wire.transcript(0).seq == 1);
    // FR-014: transmitted, not dropped, and counted as outside its window.
    REQUIRE(responder.stats().late_responses == 1);
    REQUIRE(responder.stats().transactions == 1);
    // The other station's bytes were DRAINED while the response waited -- that is how the
    // engine knew the bus was busy -- but not decoded: they are stashed, so nothing has
    // been discarded yet (red team @7d31410 finding 1: decoding during the wait is what
    // froze the belief, so the stash is what replaced it).
    REQUIRE(responder.stats().discards == 0);
    // Once the wire is free the stash is re-fed in arrival order and the frame is judged
    // exactly as it would have been read live: not this node's, so discarded and counted.
    wire.advance_to(other_end + omgp::TRUNK_T_gap_us + 100 * byte_us(), responder);
    REQUIRE(responder.stats().discards == 1);
    REQUIRE(responder.stats().transactions == 1); // and never answered
    REQUIRE(wire.transcript_size() == 1);
}

TEST_CASE("the wait for an idle bus is bounded: a station occupying the wire without pause "
          "delays a late response by at most one worst-case frame plus T_gap, never "
          "starves it",
          "[link][timing:T_gap]") {
    // Master::fire_pending's cap (link/master.cpp:277), for the same reason: past it, a
    // station still holding the wire is a trunk §3 violator, and FR-014's "MUST still
    // transmit" wins over a courtesy gap that an unbounded babbler could deny forever.
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t p[] = {0x22};
    const uint64_t request_end =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 3, p, sizeof p), 1000);

    // Non-frame noise, one byte every byte time, from before the window closes until long
    // after: the bus never goes idle for T_gap, at any poll instant.
    const uint64_t babble_start = request_end + 1;
    const uint64_t max_frame_us = static_cast<uint64_t>(kMaxWire) * byte_us();
    // Long enough to still be going at the cap (defer_origin + max_frame + T_gap), short
    // enough for MockWire's receive queue (4 * kMaxWire, contracts/mock-wire.md).
    std::vector<uint8_t> noise(300, 0x5C);
    REQUIRE(noise[0] != omgp::TRUNK_flag_byte);
    wire.inject_bytes(noise.data(), noise.size(), babble_start);
    const uint64_t babble_end = babble_start + noise.size() * byte_us();

    // The first poll is already past the window (the FR-014 late path), so it is also the
    // instant the response first found the bus busy -- the origin of the bounded wait.
    // Polling every byte time from there keeps the observation of the babble continuous.
    const uint64_t defer_origin = request_end + omgp::TRUNK_T_turn_max_us + 1;
    bool sent = false;
    for (uint64_t t = defer_origin; t <= babble_end && !sent; t += byte_us()) {
        wire.advance_to(t, responder);
        sent = wire.transcript_size() > 0;
    }
    REQUIRE(sent); // never starved, though the bus was never idle
    const uint64_t tx = wire.transcript(0).tx_start_us;
    const uint64_t cap = defer_origin + max_frame_us + omgp::TRUNK_T_gap_us;
    INFO("defer_origin=" << defer_origin << " tx=" << tx << " cap=" << cap);
    // EXACTLY the cap, not merely "no later than": the bus is never idle for T_gap here, so
    // the cap is the only thing that can release the response, and pinning the instant pins
    // the cap's own composition -- defer_origin PLUS one worst-case frame PLUS T_gap. An
    // upper bound alone is satisfied by every cap that is too SMALL (a subtracted term, a
    // constant), which is the failure that matters: too small means keying down into a frame
    // that has not finished. (Poll cadence is one byte time and max_frame + T_gap is a whole
    // number of them, so the first poll at or after the cap IS the cap.)
    REQUIRE(tx == cap);
    REQUIRE(tx < babble_end); // sent while the babble was still going, not after it stopped
    REQUIRE(responder.stats().late_responses == 1);
}

TEST_CASE("a request that completes while a late response waits for the bus is held, not "
          "lost: it is answered once the response has gone out",
          "[link]") {
    // The @e510b29 rule (a request arriving while a response is pending is held, never
    // discarded) still holds on the deferral path -- the engine now drains those bytes to
    // see the bus, so the holding moves from the wire's receive queue into the engine, one
    // decoded request deep; anything behind it stays queued as before.
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t a[] = {0xA1};
    const uint64_t end_a =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 1, a, sizeof a), 1000);

    // B is addressed to THIS node and completes while A's answer is late and waiting.
    const uint8_t b[] = {0xB2};
    const uint64_t start_b = end_a + 1000;
    const uint64_t end_b =
        inject_request(wire, request_bytes(kMyAddr, 0x07, false, 2, b, sizeof b), start_b);
    const uint64_t mid_b = start_b + (end_b - start_b) / 2;

    wire.advance_to(mid_b, responder); // A's answer deferred: B is in flight
    REQUIRE(wire.transcript_size() == 0);

    wire.advance_to(end_b + omgp::TRUNK_T_gap_us, responder);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).seq == 1); // A's answer, sent once the bus was idle
    REQUIRE(wire.transcript(0).dst == kPeer);

    // B was decoded while A's answer waited and is answered next -- not discarded.
    const uint64_t tx_end_a =
        wire.transcript(0).tx_start_us + static_cast<uint64_t>(wire.transcript(0).len) * byte_us();
    wire.advance_to(tx_end_a + omgp::TRUNK_T_gap_us + omgp::TRUNK_T_turn_max_us, responder);
    REQUIRE(handler.calls == 2);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(wire.transcript(1).seq == 2);
    REQUIRE(wire.transcript(1).dst == 0x07);
    REQUIRE(responder.stats().transactions == 2);
    REQUIRE(responder.stats().discards == 0);
}

// --- red team @71caba0 finding 3 (LOW): the engine's OWN address is wire-visible ---------

TEST_CASE("a Responder constructed with an address outside trunk §5's L2 range never "
          "originates a frame: every request is discarded and counted",
          "[link]") {
    // 71caba0 bounded the wire-derived f.src; my_addr_ becomes the src of every frame this
    // node originates (responder.cpp "src = my_addr"), and was unbounded -- the symmetric
    // half. Master::begin refuses an out-of-range dst (link/master.cpp:85-88) rather than
    // putting a non-L2 address on the trunk; the same rule here, at the only place a
    // Responder can enforce it without a Status-returning constructor.
    // 0xFF is excluded here only because no frame can carry it as a dst at all: the codec
    // refuses it (link/frame.cpp:17-18) and the Deframer discards it (`:142`), so such a
    // Responder is unreachable rather than dangerous.
    const uint8_t bad_addr = GENERATE(static_cast<uint8_t>(kAddrCount), static_cast<uint8_t>(0x40),
                                      static_cast<uint8_t>(0xFE));
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, bad_addr);

    const uint8_t p[] = {0x01};
    const uint64_t end =
        inject_request(wire, request_bytes(bad_addr, kPeer, false, 0, p, sizeof p), 1000);
    wire.advance_to(end + omgp::TRUNK_T_turn_max_us + 1000, responder);

    INFO("my_addr=" << int(bad_addr));
    REQUIRE(wire.transcript_size() == 0);
    REQUIRE(handler.calls == 0);
    REQUIRE(responder.stats().transactions == 0);
    REQUIRE(responder.stats().discards == 1);
}

// --- red team @7d31410 finding 1 (BLOCKING): a held request must not blind the engine -----
// The previous revision held one DECODED request and stopped draining there, so
// last_activity_us_ froze at that frame's last byte and every later poll transmitted at
// held_end + T_gap whatever was physically on the wire -- the bounded wait never binding,
// because the engine never saw a busy bus to wait for. Bytes are stashed undecoded now, so
// the belief stays current for everything the engine could read.

TEST_CASE("a request addressed to this node arriving during the wait does not blind it: a "
          "third station's frame behind that request is still not transmitted over",
          "[link][timing:T_gap]") {
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    // A: answered by this node, and its response is already on the late path.
    const uint8_t a[] = {0xA1};
    const uint64_t end_a =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 1, a, sizeof a), 1000);

    // B: a second station's request to THIS node -- the frame that used to be "held" and
    // freeze the belief. C: a third station's frame starting one byte time after B ends.
    const uint8_t b[] = {0xB2};
    const uint64_t start_b = end_a + 1000;
    const uint64_t end_b =
        inject_request(wire, request_bytes(kMyAddr, 0x07, false, 2, b, sizeof b), start_b);
    const uint8_t c[] = {0xC3};
    const uint64_t start_c = end_b + byte_us();
    const uint64_t end_c =
        inject_request(wire, request_bytes(0x02, kPeer, false, 3, c, sizeof c), start_c);
    REQUIRE(start_c > end_b); // C really does start after B, so B is "held" first

    // A poll landing inside C, and past end_b + T_gap -- the instant the frozen belief used
    // to authorise a transmission.
    const uint64_t mid_c = start_c + (end_c - start_c) / 2;
    REQUIRE(mid_c > end_b + omgp::TRUNK_T_gap_us);
    wire.advance_to(mid_c, responder);
    REQUIRE(wire.transcript_size() == 0); // nothing keyed down inside C

    // ...and it still goes out once C has ended and the bus has been idle for T_gap.
    wire.advance_to(end_c + omgp::TRUNK_T_gap_us, responder);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).seq == 1); // A's answer, first
    REQUIRE(wire.transcript(0).tx_start_us >= end_c + omgp::TRUNK_T_gap_us);

    // B was stashed, not lost: it is answered next, and C -- addressed elsewhere -- is
    // discarded and counted, exactly as if both had been read live.
    wire.advance_to(end_c + omgp::TRUNK_T_gap_us + 200 * byte_us(), responder);
    REQUIRE(handler.calls == 2);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(wire.transcript(1).seq == 2);
    REQUIRE(wire.transcript(1).dst == 0x07);
    // C sits behind B in the stash, so it is re-fed on a later poll() -- one accepted
    // request ends a drain, here as everywhere (the "one answer per poll" rule).
    for (int i = 0; i < 3; ++i)
        wire.advance_to(end_c + omgp::TRUNK_T_gap_us +
                            static_cast<uint64_t>(300 + 100 * i) * byte_us(),
                        responder);
    REQUIRE(responder.stats().discards == 1);
    REQUIRE(handler.calls == 2); // C is never answered
    REQUIRE(wire.transcript_size() == 2);
}

TEST_CASE("the bounded wait starts afresh for each late response: a second transaction under "
          "the same babble waits its own full cap, not one measured from the first",
          "[link][timing:T_gap]") {
    // The origin of the wait is cleared on every transmit. Left set, a later deferral would
    // inherit the FIRST one's origin, so its cap would already be in the past and the engine
    // would transmit at once -- the exact behaviour the cap exists to prevent, reappearing
    // on every transaction after the first.
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint64_t max_frame_us = static_cast<uint64_t>(kMaxWire) * byte_us();
    const uint8_t p[] = {0x33};

    uint64_t next_seq = 1;
    uint64_t tx_of_previous = 0;
    for (int round = 0; round < 2; ++round) {
        const uint64_t request_at = round == 0 ? 1000 : tx_of_previous + 4000;
        const uint64_t request_end = inject_request(
            wire,
            request_bytes(kMyAddr, kPeer, false, static_cast<uint8_t>(next_seq++), p, sizeof p),
            request_at);
        // Continuous non-frame noise from just after the request until well past the cap.
        std::vector<uint8_t> noise(250, 0x5C);
        const uint64_t babble_start = request_end + 1;
        wire.inject_bytes(noise.data(), noise.size(), babble_start);
        const uint64_t babble_end = babble_start + noise.size() * byte_us();

        const uint64_t defer_origin = request_end + omgp::TRUNK_T_turn_max_us + 1;
        const uint64_t cap = defer_origin + max_frame_us + omgp::TRUNK_T_gap_us;
        const size_t before = wire.transcript_size();
        for (uint64_t t = defer_origin; t <= babble_end && wire.transcript_size() == before;
             t += byte_us())
            wire.advance_to(t, responder);
        REQUIRE(wire.transcript_size() == before + 1);
        const uint64_t tx = wire.transcript(before).tx_start_us;
        INFO("round " << round << " defer_origin=" << defer_origin << " tx=" << tx
                      << " cap=" << cap);
        REQUIRE(tx == cap); // its OWN cap, both times
        tx_of_previous = tx;
        // Let the response finish and the stash drain before the next round's request.
        for (int i = 1; i <= 12; ++i)
            wire.advance_to(tx + static_cast<uint64_t>(i) * 30 * byte_us(), responder);
    }
    REQUIRE(responder.stats().transactions == 2);
    REQUIRE(responder.stats().late_responses == 2);
}

TEST_CASE("a request re-fed from the stash inside its own turnaround window is answered and "
          "NOT counted late: the stashed byte's own instant is what decides",
          "[link][timing:T_turn_max]") {
    // Red team @b262d46 finding 2. An earlier revision labelled the stashed start instant
    // "unobservable", on the ground that a re-fed request is ALWAYS past T_turn_max by the
    // time it is re-fed. False on the CAP path: the cap can fire with less than T_gap of
    // idle, so a request that arrived just before it is re-fed while still inside its own
    // window -- answered on time, and rightly not counted late. Replacing the stored instant
    // with anything else (a constant, a cadence-derived value) makes request_end_us wrong
    // and moves late_responses, so this case is what pins the field.
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t p[] = {0x22};
    const std::vector<uint8_t> a = request_bytes(kMyAddr, kPeer, false, 3, p, sizeof p);
    const uint64_t end_a = inject_request(wire, a, 1000);

    const uint64_t defer_origin = end_a + omgp::TRUNK_T_turn_max_us + 1;
    const uint64_t max_frame_us = static_cast<uint64_t>(kMaxWire) * byte_us();
    const uint64_t cap = defer_origin + max_frame_us + omgp::TRUNK_T_gap_us;

    // Sparse noise -- one byte every 40 us, inside T_gap -- so the bus never grants the gap,
    // the wait runs to the cap, and the stash never fills.
    const uint8_t noise = 0x5C;
    const uint8_t p2[] = {0x77};
    const std::vector<uint8_t> b = request_bytes(kMyAddr, kPeer, false, 5, p2, sizeof p2);
    const uint64_t b_start = cap - static_cast<uint64_t>(b.size()) * byte_us();
    for (uint64_t t = end_a + 60; t + 40 < b_start; t += 40)
        wire.inject_bytes(&noise, 1, t);
    wire.inject_bytes(b.data(), b.size(), b_start);
    const uint64_t end_b = b_start + static_cast<uint64_t>(b.size()) * byte_us();

    for (uint64_t t = defer_origin; t <= end_a + 6000; t += byte_us())
        wire.advance_to(t, responder);

    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(handler.calls == 2);
    REQUIRE(wire.transcript(0).seq == 3); // A, released by the cap
    REQUIRE(wire.transcript(1).seq == 5); // B, re-fed from the stash and answered
    INFO("end_b=" << end_b << " tx1=" << wire.transcript(1).tx_start_us);
    // B's answer goes out within its own turnaround window, measured from B's OWN end --
    // which is only known because the stash kept that byte's instant.
    REQUIRE(wire.transcript(1).tx_start_us <= end_b + omgp::TRUNK_T_turn_max_us);
    REQUIRE(responder.stats().late_responses == 1); // A only; B was on time
}

TEST_CASE("a request whose first byte lands exactly on the stash bound is answered, not "
          "swallowed: the drain stops BEFORE taking a byte it cannot hold",
          "[link]") {
    // Red team @b262d46 finding 1 (BLOCKING). An earlier revision took the byte off the wire
    // and only then asked the stash to hold it: at the bound the byte was consumed and
    // dropped, so a legal request straddling it lost its opening byte and the rest of its
    // frame was re-fed as headless garbage -- silently unanswered. The check now precedes
    // wire_.receive(), so a byte is never taken unless there is room for it.
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t a[] = {0xA1};
    const uint64_t end_a =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 1, a, sizeof a), 1000);

    // Exactly kMaxWire filler bytes arrive during the wait, so the stash is full to the byte
    // -- and then a perfectly legal request follows, its first byte landing on the bound.
    std::vector<uint8_t> filler(kMaxWire, 0x5C);
    REQUIRE(filler[0] != omgp::TRUNK_flag_byte);
    const uint64_t filler_start = end_a + 1;
    wire.inject_bytes(filler.data(), filler.size(), filler_start);
    const uint64_t filler_end = filler_start + filler.size() * byte_us();

    const uint8_t b[] = {0xB2};
    const std::vector<uint8_t> b_bytes = request_bytes(kMyAddr, 0x07, false, 2, b, sizeof b);
    const uint64_t end_b = inject_request(wire, b_bytes, filler_end);

    // One late poll well past everything: it fills the stash and must stop there, leaving
    // B's every byte on the wire. Then poll on until the backlog has drained.
    wire.advance_to(end_b + 5000, responder);
    for (int i = 1; i <= 40; ++i)
        wire.advance_to(end_b + 5000 + static_cast<uint64_t>(i) * 40 * byte_us(), responder);

    // Both requests answered: A first (it was already pending), then B.
    REQUIRE(handler.calls == 2);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(wire.transcript(0).seq == 1);
    REQUIRE(wire.transcript(0).dst == kPeer);
    REQUIRE(wire.transcript(1).seq == 2);
    REQUIRE(wire.transcript(1).dst == 0x07);
    REQUIRE(responder.stats().transactions == 2);
}

// --- red team @2efcb67: the drain's two exits mean opposite things -----------------------

TEST_CASE("a late response goes out T_gap after the last byte the engine saw, whether or "
          "not that byte happened to fill the stash: only an UNREAD wire is blindness",
          "[link][timing:T_gap]") {
    // The drain stops either because the stash is full (bytes left unread: the belief about
    // the bus is stale) or because the wire's queue is empty (nothing left to read: the
    // belief is current and complete). When the byte that fills the stash is also the last
    // byte on the wire both are true at once, and reading fullness as staleness cost a
    // further max_frame + T_gap of silence on a bus the engine had just watched go quiet --
    // 1420 us bought by one byte. GENERATE spans exactly that boundary.
    const size_t noise_len = GENERATE(kMaxWire - 1, kMaxWire);
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t p[] = {0x11};
    const uint64_t request_end =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 1, p, sizeof p), 1000);

    // Noise from just past the window, then nothing at all: the bus is idle from its last
    // byte onward, and the engine drains every byte of it in one late poll.
    const uint64_t noise_start = request_end + omgp::TRUNK_T_turn_max_us + 1;
    std::vector<uint8_t> noise(noise_len, 0x5C);
    REQUIRE(noise[0] != omgp::TRUNK_flag_byte);
    wire.inject_bytes(noise.data(), noise.size(), noise_start);
    const uint64_t noise_end = noise_start + noise.size() * byte_us();

    for (uint64_t t = noise_end; t <= noise_end + 4 * omgp::TRUNK_T_gap_us; t += byte_us())
        wire.advance_to(t, responder);

    INFO("noise_len=" << noise_len << " noise_end=" << noise_end);
    REQUIRE(wire.transcript_size() == 1);
    // T_gap after the last byte actually seen -- not one worst-case frame later.
    REQUIRE(wire.transcript(0).tx_start_us == noise_end + omgp::TRUNK_T_gap_us);
    REQUIRE(responder.stats().late_responses == 1);
}

TEST_CASE("a burst that fills the stash is drained and answered at wire speed, not one "
          "bounded wait per request: re-fed space is reclaimed",
          "[link]") {
    // held_.next was never reclaimed, so a stash that filled once stayed "full" for its
    // whole re-feed: every request re-fed out of it was answered blind, at its own full cap,
    // with poll() taking no bytes off the wire throughout -- 24.87 ms to clear a queue that
    // arrived in 1.42 ms (red team @2efcb67, RT2). On the ESP32-S3, whose UART RX FIFO is
    // 128 bytes (docs/OPEN-QUESTIONS.md 2026-09-06), a non-draining window that long loses
    // bytes rather than queueing them.
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t p[] = {0x11};
    const uint64_t request_end =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 1, p, sizeof p), 1000);

    // Back-to-back requests filling exactly the stash, then an idle bus.
    const uint64_t burst_start = request_end + omgp::TRUNK_T_turn_max_us + 1;
    std::vector<uint8_t> burst;
    uint8_t seq = 2;
    while (burst.size() < kMaxWire) {
        const std::vector<uint8_t> b = request_bytes(kMyAddr, kPeer, false, seq++, p, sizeof p);
        burst.insert(burst.end(), b.begin(), b.end());
    }
    burst.resize(kMaxWire);
    wire.inject_bytes(burst.data(), burst.size(), burst_start);
    const uint64_t burst_end = burst_start + burst.size() * byte_us();

    // ...and one more request arriving while that backlog is still being re-fed. This is
    // what makes the reclamation load-bearing rather than an optimisation: a new wait begins
    // with the stash part-consumed, so without reclaiming the spent prefix `len` is already
    // at the array bound and stash() would silently drop this request's bytes -- the very
    // loss the reserve slot and the pre-receive check exist to prevent.
    const uint8_t tail_payload[] = {0x99};
    const uint64_t tail_at = burst_end + 300;
    const uint64_t tail_end = inject_request(
        wire, request_bytes(kMyAddr, 0x07, false, 15, tail_payload, sizeof tail_payload), tail_at);
    REQUIRE(tail_end > burst_end);

    wire.advance_to(burst_end, responder);
    for (uint64_t t = burst_end + byte_us(); t <= burst_end + 60000; t += byte_us())
        wire.advance_to(t, responder);

    // The straggler is answered like any other request: nothing was dropped.
    bool answered_tail = false;
    for (size_t i = 0; i < wire.transcript_size(); ++i)
        if (wire.transcript(i).dst == 0x07 && wire.transcript(i).seq == 15)
            answered_tail = true;
    REQUIRE(answered_tail);

    REQUIRE(wire.transcript_size() > 1);
    // The BURST's own answers (all to kPeer); the straggler above came from 0x07 and arrived
    // after burst_end, so its answer is not part of this drain.
    uint64_t last_burst_tx = 0;
    size_t burst_answers = 0;
    for (size_t i = 0; i < wire.transcript_size(); ++i)
        if (wire.transcript(i).dst == kPeer) {
            last_burst_tx = wire.transcript(i).tx_start_us;
            ++burst_answers;
        }
    INFO("answers=" << wire.transcript_size() << " of which burst=" << burst_answers
                    << " burst_end=" << burst_end << " last=" << last_burst_tx
                    << " delta=" << (last_burst_tx - burst_end));
    REQUIRE(burst_answers > 1);
    // The backlog clears at WIRE SPEED, not one bounded wait per request. The yardstick is
    // physical: the burst is one worst-case frame of arrivals and its answers are frames of
    // the same order, so two worst-case frame times is the floor for emitting them all --
    // the observed drain sits just above one. A wait per request would be
    // burst_answers * (max_frame + T_gap), an order of magnitude more; before this round's
    // two fixes this backlog took 24.87 ms to clear 1.42 ms of arrivals.
    const uint64_t max_frame_us = static_cast<uint64_t>(kMaxWire) * byte_us();
    REQUIRE(last_burst_tx - burst_end < 2 * max_frame_us);
    REQUIRE(last_burst_tx - burst_end < burst_answers * (max_frame_us + omgp::TRUNK_T_gap_us) / 4);
}
