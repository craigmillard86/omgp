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

TEST_CASE("eight well-spaced requests are all accounted for whatever the poll cadence: "
          "answered, or discarded and counted",
          "[link]") {
    // The red team's own sweep (@e510b29 finding 1): at 400 us and coarser, requests were
    // SILENTLY swallowed -- 2 of 8 answered at a 2 ms poll period, stats() unmoved. That
    // silence is what this case was written to close, and it stays closed.
    //
    // AMENDED for the maintainer ruling of 2026-09-11 (docs/OPEN-QUESTIONS.md "Maintainer
    // rulings on the pending entries", item 5; the ruling names this case). Its earlier form
    // asserted "all eight answered, none discarded", which the hold-and-stop corner bought by
    // leaving bytes unread on the wire -- destroying them there, uncounted, under any heavier
    // load (red team @bab7378 finding 2). The engine now reads to the end of the queue and
    // discards the excess IN THE ENGINE, where FR-016's stats().discards channel shows it. So
    // the property asserted here is accounting, not throughput: at every cadence every one of
    // the eight is either answered or counted, and the answers still come out in arrival
    // order. The per-cadence answered counts are pinned too, as the honest observable of this
    // design -- they are what the ruling traded for, not a target.
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
            for (uint64_t now = period; now <= last_end + 200000; now += period)
                wire.advance_to(now, responder);
            const uint32_t answered = responder.stats().transactions;
            INFO("period=" << period << " answered=" << answered
                           << " discards=" << responder.stats().discards);
            // Nothing is lost silently: every request the station offered was read off the
            // wire and is in exactly one of the two counters.
            REQUIRE(answered + responder.stats().discards == 8);
            REQUIRE(wire.pending_injected() == 0);
            REQUIRE(handler.calls == static_cast<int>(answered));
            REQUIRE(wire.transcript_size() == answered);
            // Arrival order, with a discarded request's sequence simply absent: the hold is a
            // queue, and a discard never reorders what is still in it.
            for (size_t i = 1; i < wire.transcript_size(); ++i)
                REQUIRE(wire.transcript(i).seq > wire.transcript(i - 1).seq);
            // The measured cost, pinned per cadence: 8, 7, 5 and 4 answered at 50, 400, 1000
            // and 2000 us. The last three are exactly the "7/8, 5/8, 4/8" the ruling's own
            // entry measured for this corner (docs/OPEN-QUESTIONS.md 2026-09-07, the "never
            // stop, hold 2, discard-and-count the excess" row), so the engine built here is
            // the one the maintainer ruled on and not some neighbour of it.
            uint32_t expected_answered = 4;
            if (period == 50)
                expected_answered = 8;
            else if (period == 400)
                expected_answered = 7;
            else if (period == 1000)
                expected_answered = 5;
            REQUIRE(answered == expected_answered);
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

    uint8_t payload2[omgp::LIMIT_max_l3_message];
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

TEST_CASE("requests arriving faster than poll() is called are answered as far as the hold "
          "reaches and counted beyond it: the backlog cannot grow unread, and the host's own "
          "request is reached (open question on the tension, not asserted as desired)",
          "[link]") {
    // Red team @17554c8 finding 1 (MEDIUM), confirmed by review @17554c8 finding 1, re-run
    // here with the suite's own helpers. A station at 0x07 puts one request to this node on
    // the bus every 300 us; the engine is polled every 1 ms; the host sends its own request at
    // t = 8 ms.
    //
    // AMENDED under the ruling of 2026-09-11 (#372); the earlier form of this case existed to
    // pin a failing observable for that ruling, and this is what it turns into. Under
    // hold-and-stop the engine answered 6, counted NOTHING, left 63 requests unread on the
    // wire, and never reached the host at all -- an unbounded backlog whose only visible trace
    // was the absence of answers. Now the engine reads the queue to its end at every poll:
    // 7 answered, 59 discarded and counted, at most kHeldRequests decoded-and-waiting, and the
    // host's request IS answered, because a request can no longer be starved behind bytes the
    // engine refuses to read.
    //
    // What this case does NOT settle: FR-014 (a late poll MUST still transmit) and FR-017
    // (MUST never transmit outside a response window) still pull opposite ways on a stale
    // queued request. The 2026-09-11 ruling says so in terms and leaves it to the 2026-09-06
    // held-request-queue entry, which is still open. These CHECKs pin what the engine does.
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
    size_t offered_bytes = 0; // every byte put on the wire, for the #148 acknowledgement below
    bool host_sent = false;
    for (uint64_t t = 0; t <= 20 * poll_period; t += poll_period) {
        // Everything the other station will have put on the bus before the next poll.
        while (next_flood < t + poll_period) {
            const auto req =
                request_bytes(kMyAddr, kOther, false, static_cast<uint8_t>(flooded & 0x0F),
                              flood_payload, sizeof flood_payload);
            inject_request(wire, req, next_flood);
            offered_bytes += req.size();
            ++flooded;
            next_flood += flood_period;
        }
        if (!host_sent && t >= host_at) {
            const auto req =
                request_bytes(kMyAddr, kPeer, false, 9, host_payload, sizeof host_payload);
            host_end = inject_request(wire, req, t);
            offered_bytes += req.size();
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

    // Pin history, each value the honest observable of its design: 20 (71caba0, stop draining
    // at the first decoded request and judge from a stale last_activity_), 17 (b262d46, drain
    // everything into a byte stash), 7 (2efcb67, treat a full stash as blindness), 6 (5830fc4,
    // hold what a wait can absorb, stop rather than drop, and say the reading is partial), and
    // now this, under the ruling of 2026-09-11 (#372): read to the end of the queue every
    // time, hold two, count the rest.
    INFO("answers=" << wire.transcript_size() << " calls=" << handler.calls
                    << " late=" << responder.stats().late_responses
                    << " discards=" << responder.stats().discards);
    CHECK(wire.transcript_size() == 7);
    CHECK(handler.calls == 7);
    CHECK(responder.stats().late_responses == 7);
    // The loss under this load is large and it is COUNTED -- the whole of what the ruling
    // requires of an overloaded node, and the opposite of the `discards == 0` this case used
    // to pin while 63 requests sat destroyed-or-waiting outside the engine's books.
    CHECK(responder.stats().discards == 59);
    // ...and the host, which the backlog used to bury, is answered. Not a general guarantee:
    // it holds here because the engine no longer starves behind bytes it refuses to read.
    CHECK(answered_host);
    CHECK(host_end + omgp::TRUNK_T_resp_us < 20 * poll_period); // though long after it gave up
    REQUIRE(flooded == 70);

    // Full accounting, and the one assertion that would catch a silent regression to the old
    // corner. #148: the run ends mid-cadence, so the last few requests are still arriving --
    // 27 bytes, three whole frames, injected after the final poll's instant. Everything else
    // was READ, and every read request is answered, counted, or one of at most kHeldRequests
    // decoded and still waiting for the wire. Nothing else is possible: the difference below
    // IS the hold's occupancy, and asserting it against kHeldRequests is what makes "the
    // backlog cannot grow unread" an assertion rather than a claim.
    const size_t left = wire.take_pending_injected();
    CHECK(left == 27);
    CHECK(left < offered_bytes);
    const std::vector<uint8_t> one_frame =
        request_bytes(kMyAddr, kOther, false, 0, flood_payload, sizeof flood_payload);
    const size_t frame_bytes = one_frame.size();
    REQUIRE(left % frame_bytes == 0); // whole frames, none of them half-consumed
    const size_t offered_frames = static_cast<size_t>(flooded) + 1; // the flood, plus the host
    const size_t read_frames = offered_frames - left / frame_bytes;
    const size_t accounted = wire.transcript_size() + responder.stats().discards;
    INFO("read=" << read_frames << " accounted=" << accounted);
    REQUIRE(read_frames >= accounted);
    // The hold's depth is private to the engine (Responder::kHeldRequests, link/responder.hpp,
    // which static_asserts it to 2); restated here because that is what the bound means.
    constexpr size_t kHold = 2;
    REQUIRE(read_frames - accounted <= kHold);
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
    // The other station's frame was drained AND decoded while the response waited -- that is
    // how the engine knew the bus was busy -- and, not being this node's, discarded and
    // counted there and then, exactly as it would have been while Listening. Nothing about
    // it is held: only a request this node owes an answer to occupies a hold slot.
    REQUIRE(responder.stats().discards == 1);
    REQUIRE(responder.stats().transactions == 1); // and never answered
    wire.advance_to(other_end + omgp::TRUNK_T_gap_us + 100 * byte_us(), responder);
    REQUIRE(responder.stats().discards == 1); // and not counted twice
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
    // #148: the loop stops at the poll that transmitted (tx), and the noise deliberately runs
    // past it -- "sent while the babble was still going" is the assertion right above, so the
    // rest of the noise MUST still be queued. Acknowledged at the count the case's own timeline
    // gives: bytes at babble_start, +byte_us, ... are due at tx, the remainder is not.
    const size_t drained = static_cast<size_t>((tx - babble_start) / byte_us()) + 1;
    REQUIRE(drained < noise.size());
    REQUIRE(wire.take_pending_injected() == noise.size() - drained);
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
// because the engine never saw a busy bus to wait for. The engine decodes as it drains now
// and holds what it cannot answer, so the belief stays current for everything it reads.

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

    // B was held, not lost: it is answered next. C -- addressed elsewhere -- is decoded
    // during the wait and discarded there and then, since only a request this node owes an
    // answer to takes a hold slot.
    wire.advance_to(end_c + omgp::TRUNK_T_gap_us + 200 * byte_us(), responder);
    REQUIRE(handler.calls == 2);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(wire.transcript(1).seq == 2);
    REQUIRE(wire.transcript(1).dst == 0x07);
    // C was already discarded during the wait; these further polls only confirm it is not
    // counted twice and never answered.
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
        // Let the response finish and the held backlog clear before the next round.
        for (int i = 1; i <= 12; ++i)
            wire.advance_to(tx + static_cast<uint64_t>(i) * 30 * byte_us(), responder);
    }
    REQUIRE(responder.stats().transactions == 2);
    REQUIRE(responder.stats().late_responses == 2);
}

TEST_CASE("a request held through a wait and answered inside its own turnaround window is "
          "NOT counted late: the request's own end instant is what decides",
          "[link][timing:T_turn_max]") {
    // Red team @b262d46 finding 2. An earlier revision labelled the held request's instant
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
    // the wait runs to the cap, and only one request completes inside it.
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
    REQUIRE(wire.transcript(1).seq == 5); // B, held through the wait and then answered
    INFO("end_b=" << end_b << " tx1=" << wire.transcript(1).tx_start_us);
    // B's answer goes out within its own turnaround window, measured from B's OWN end --
    // which is only known because the hold kept B's own request_end, not the instant it
    // happened to be answered.
    REQUIRE(wire.transcript(1).tx_start_us <= end_b + omgp::TRUNK_T_turn_max_us);
    REQUIRE(responder.stats().late_responses == 1); // A only; B was on time
}

TEST_CASE("a request arriving when the hold is already full is discarded WHOLE and counted, "
          "never swallowed: one frame, one discard, and the frames behind it still decode",
          "[link]") {
    // Red team @b262d46 finding 1 (BLOCKING), restated for the design that replaced it. The
    // defect was that a BYTE was taken off the wire and only then found to be unkeepable, so
    // a legal request straddling a capacity bound lost its opening byte and the rest of its
    // frame decoded as headless garbage -- silently unanswered. The bound is expressed in
    // REQUESTS, so that defect cannot recur: a request is discarded only once the Deframer has
    // delivered it whole, and the frame behind it starts at its own opening flag.
    //
    // AMENDED for the ruling of 2026-09-11 (#372). The earlier form asserted that D was "left
    // on the wire and answered later" -- the hold-and-stop corner, which bought that answer by
    // leaving the wire unread, and lost requests uncounted at the wire under heavier load. D is
    // now consumed, discarded and COUNTED. What must still hold, and is what this case exists
    // for, is that the discard is clean: B and C come back with their own payloads, D's bytes
    // corrupt nothing, and the loss is visible in stats().
    //
    // (An earlier version filled a byte stash with non-frame noise. Noise is never held, so
    // under the current engine it exercised no bound at all -- review @6440074 finding 5. This
    // one fills the hold with real requests, which is the only way to reach it.)
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    // Distinct payloads: the handler echoes them, so each answer's payload is what proves a
    // HELD request's own bytes were carried through the hold. With one payload for all four
    // this case could not tell one request's body from another's -- and deep-verify @6440074
    // showed exactly that gap, by skipping the hold's payload copy entirely and passing the
    // whole suite.
    const uint8_t pa[] = {0xA1, 0x0A};
    const uint8_t pb[] = {0xB2, 0x0B};
    const uint8_t pc[] = {0xC3, 0x0C};
    const uint8_t pd[] = {0xD4, 0x0D};
    // A is answered late; B and C arrive back-to-back behind it, filling the hold
    // (kHeldRequests); D arrives behind them, with the hold already full.
    const uint64_t end_a =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 1, pa, sizeof pa), 1000);
    const uint64_t end_b = inject_request(
        wire, request_bytes(kMyAddr, kPeer, false, 2, pb, sizeof pb), end_a + byte_us());
    const uint64_t end_c = inject_request(
        wire, request_bytes(kMyAddr, kPeer, false, 3, pc, sizeof pc), end_b + byte_us());
    const uint64_t end_d = inject_request(
        wire, request_bytes(kMyAddr, kPeer, false, 4, pd, sizeof pd), end_c + byte_us());

    // CLAUDE.md rule 5 on the path this case exists for: the slot-full discard allocates
    // nothing. Only the engine call is wrapped -- wire.advance_to()'s own REQUIRE machinery
    // may allocate, and MockWire is host-only test code (as at :145).
    for (uint64_t t = end_a + omgp::TRUNK_T_turn_max_us + 1; t <= end_d + 40000; t += byte_us()) {
        wire.advance_to(t);
        HEAP_FREE_SCOPE({ responder.poll(t); });
    }

    // A, B and C answered in arrival order; D discarded and counted, exactly one discard for
    // exactly one frame. Full accounting: four offered, three answered, one counted.
    INFO("answers=" << wire.transcript_size() << " discards=" << responder.stats().discards);
    REQUIRE(wire.transcript_size() == 3);
    REQUIRE(handler.calls == 3);
    const uint8_t* expected[3] = {pa, pb, pc};
    for (size_t i = 0; i < 3; ++i) {
        INFO("answer " << i);
        REQUIRE(wire.transcript(i).seq == static_cast<uint8_t>(i + 1));
        // Each answer echoes ITS OWN request's payload. B and C came out of the hold, so
        // this pins that the hold carried their bytes and not merely their headers -- and
        // that D, discarded from the slot-full path, overwrote neither of them.
        REQUIRE(wire.transcript(i).len == 2);
        REQUIRE(wire.transcript(i).payload[0] == expected[i][0]);
        REQUIRE(wire.transcript(i).payload[1] == expected[i][1]);
    }
    REQUIRE(responder.stats().transactions == 3);
    REQUIRE(responder.stats().discards == 1); // D, counted -- not lost in silence
    // D's own sequence is never answered, and its bytes left nothing behind them: the wire is
    // fully read, and no fifth frame decoded out of the residue.
    for (size_t i = 0; i < wire.transcript_size(); ++i)
        REQUIRE(wire.transcript(i).seq != 4);
    REQUIRE(wire.pending_injected() == 0);
}

TEST_CASE("a trunk §7 retry completing when the hold is already full is discarded and counted, "
          "NOT replayed: what FR-015 loses to the 2026-09-11 ruling, pinned",
          "[link]") {
    // Review @3dfe0e3 (BLOCKING): the discard branch hold_or_discard() gained under the ruling
    // of 2026-09-11 (#372) tests acceptable() only -- dst, the response bit, src and trunk §5's
    // range -- never f.retry. So a retry is discarded on exactly the same terms as a new
    // request, and FR-015's unconditional "MUST retransmit the buffered frame unchanged" is not
    // met for one that completes past kHeldRequests inside a single late wait. That consequence
    // is real, it is a divergence from a MUST that item 5 of that ruling did not amend (FR-015
    // does carry a 2026-09-11 marker, but it is item 7's #373 request-byte matching, which
    // does not reach a retry discarded before the replay test runs), and it is recorded in
    // docs/OPEN-QUESTIONS.md 2026-09-14 ("item 5 of the 2026-09-11 rulings also discards a
    // trunk §7 retry ...", Ruling: PENDING -- human). This case exists so the divergence is an
    // ASSERTION rather than a construction argument: whichever way it is ruled, the behaviour
    // cannot move in silence. It is NOT an endorsement -- corner (b) of that entry would make
    // it fail, which is the point.
    //
    // The two sections differ only in how many requests sit between the retry and the hold, so
    // what loses the retry is demonstrably the hold bound and nothing about retries themselves.
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t pa[] = {0xA1, 0x0A};
    const uint8_t pb[] = {0xB2, 0x0B};
    const uint8_t pc[] = {0xC3, 0x0C};
    // A is answered, and its answer is on the late path from defer_origin on. Fillers arrive
    // behind it during that wait; R -- a trunk §7 retry of A, the frame FR-015 says must be
    // served from buffer_ byte for byte -- arrives last.
    const uint64_t end_a =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 1, pa, sizeof pa), 1000);
    uint64_t end_last = inject_request(wire, request_bytes(kMyAddr, kPeer, false, 2, pb, sizeof pb),
                                       end_a + byte_us());
    const bool hold_full = GENERATE(true, false);
    if (hold_full)
        end_last = inject_request(wire, request_bytes(kMyAddr, kPeer, false, 3, pc, sizeof pc),
                                  end_last + byte_us());
    const uint64_t end_r = inject_request(
        wire, request_bytes(kMyAddr, kPeer, true, 1, pa, sizeof pa), end_last + byte_us());

    for (uint64_t t = end_a + omgp::TRUNK_T_turn_max_us + 1; t <= end_r + 40000; t += byte_us())
        wire.advance_to(t, responder);

    INFO("hold_full=" << hold_full << " answers=" << wire.transcript_size()
                      << " discards=" << responder.stats().discards
                      << " replays=" << responder.stats().replays_served);
    // Common to both: every offered frame was read off the wire, and each is in exactly one
    // counter. The ruling's accounting property holds for a retry as for anything else.
    REQUIRE(wire.pending_injected() == 0);
    const uint32_t accounted = responder.stats().transactions + responder.stats().replays_served +
                               responder.stats().discards;
    REQUIRE(accounted == (hold_full ? 4u : 3u));

    if (hold_full) {
        // A, B and C answered; R read, discarded and COUNTED. The host gets no answer to its
        // retry at all -- neither the buffered frame (FR-015) nor a fresh one (FR-016) -- so it
        // sees a second T_resp timeout and, per trunk §7, a consecutive failure against this
        // node. That is the cost the entry puts to the maintainer.
        REQUIRE(wire.transcript_size() == 3);
        REQUIRE(responder.stats().transactions == 3);
        REQUIRE(responder.stats().replays_served == 0);
        REQUIRE(responder.stats().discards == 1);
        // Exactly one answer carries seq 1 -- A's own. R produced no second one.
        int with_seq_1 = 0;
        for (size_t i = 0; i < wire.transcript_size(); ++i)
            if (wire.transcript(i).seq == 1)
                ++with_seq_1;
        REQUIRE(with_seq_1 == 1);
    } else {
        // One filler short of the bound, R fits in the hold and IS answered: three answers (A,
        // B, R), two of them seq 1. It is served as new rather than replayed (buffer_ holds B's
        // response by the time R is popped, so seq 1 != buffer_.seq -- FR-016's "treated as
        // new", which this engine has always done: red team @033182a finding 3). The contrast
        // is the evidence that the hold bound, not the retry bit, is what loses it above.
        REQUIRE(wire.transcript_size() == 3);
        REQUIRE(responder.stats().discards == 0);
        // "Served as new rather than replayed" is asserted, not just stated: every counter
        // above holds equally for an engine that replayed (transactions 2 + replays_served 1),
        // so without this line the sentence is a claim the case does not make (review
        // @948cdf0 finding 3).
        REQUIRE(responder.stats().replays_served == 0);
        REQUIRE(responder.stats().transactions == 3);
        int with_seq_1 = 0;
        for (size_t i = 0; i < wire.transcript_size(); ++i)
            if (wire.transcript(i).seq == 1)
                ++with_seq_1;
        REQUIRE(with_seq_1 == 2);
    }
}

// --- red team @2efcb67 / @7a80ec3: the late drain now has ONE exit ------------------------
// Those rounds turned on the drain having two exits that meant opposite things: the wire's
// queue empty (a COMPLETE reading of the bus -- T_gap of idle after the last byte is a real
// gap) or the hold full (a PARTIAL one -- bytes left unread, so the wait fell back to the
// bounded cap). Under the ruling of 2026-09-11 (#372) the second exit is gone: poll() drains
// to the end of the queue on every path and counts what it cannot hold, so on the late path
// the reading is always complete and T_gap is always measured from a byte the engine actually
// read. Every revision that instead inferred completeness from a buffer's occupancy had a
// capacity boundary and reopened the same collision at it, one byte further along each time --
// the cases below pin the surviving rule at exactly those bounds (review @948cdf0 finding 4:
// this banner described the deleted exit).

TEST_CASE("a late response goes out T_gap after the last byte the engine saw when the wait "
          "absorbed no request, however much traffic passed",
          "[link][timing:T_gap]") {
    // Noise spans every capacity bound an earlier revision had (kMaxWire, kMaxWire + 1 and
    // twice kMaxWire): none of them changes the answer now, because none of them exists.
    // What decides is that nothing was HELD, so the drain reached the end of the queue.
    const size_t noise_len =
        GENERATE(size_t{40}, kMaxWire - 1, kMaxWire, kMaxWire + 1, size_t{2} * kMaxWire);
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t p[] = {0x11};
    const uint64_t request_end =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 1, p, sizeof p), 1000);

    const uint64_t noise_start = request_end + omgp::TRUNK_T_turn_max_us + 1;
    std::vector<uint8_t> noise(noise_len, 0x5C);
    REQUIRE(noise[0] != omgp::TRUNK_flag_byte);
    wire.inject_bytes(noise.data(), noise.size(), noise_start);
    const uint64_t noise_end = noise_start + noise.size() * byte_us();

    for (uint64_t t = noise_end; t <= noise_end + 4 * omgp::TRUNK_T_gap_us; t += byte_us())
        wire.advance_to(t, responder);

    INFO("noise_len=" << noise_len << " noise_end=" << noise_end);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).tx_start_us == noise_end + omgp::TRUNK_T_gap_us);
    REQUIRE(responder.stats().late_responses == 1);
    // Nothing of the noise is held: only a request this node owes an answer to takes a hold
    // slot, so the drain always reached the end of the queue here.
    REQUIRE(responder.stats().transactions == 1);
}

TEST_CASE("a burst of back-to-back requests is fully accounted: the few the node can answer "
          "inside one wait are answered, the rest are discarded and counted",
          "[link]") {
    // AMENDED for the ruling of 2026-09-11 (#372); this is the case where the trade is at its
    // starkest, so it states both sides. Before: the engine held what a wait could absorb and
    // STOPPED reading, so a 142-byte burst was answered in full, over ~21.93 ms of caps -- and
    // the bytes it did not read sat in the wire's queue, which on any real UART (or on
    // MockWire past 568 bytes) destroys them uncounted once it fills. After: the engine reads
    // the burst to the end, answers what its hold can carry, and COUNTS the rest.
    //
    // A burst this dense cannot be answered in full by a node that may hold two requests at a
    // time and owes each answer a gap: that is a property of the offered load, not of this
    // choice. What the choice decides is whether the shortfall is visible. It is:
    // transactions + discards accounts for every frame in the burst.
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t p[] = {0x11};
    inject_request(wire, request_bytes(kMyAddr, kPeer, false, 1, p, sizeof p), 1000);

    // Back-to-back requests, one worst-case frame of them, then an idle bus.
    const uint64_t burst_start = 1000 + omgp::TRUNK_T_turn_max_us + 200;
    std::vector<uint8_t> burst;
    uint8_t seq = 2;
    int frames = 0;
    while (burst.size() < kMaxWire) {
        const std::vector<uint8_t> b = request_bytes(kMyAddr, kPeer, false, seq++, p, sizeof p);
        if (burst.size() + b.size() > kMaxWire)
            break;
        burst.insert(burst.end(), b.begin(), b.end());
        ++frames;
    }
    REQUIRE(frames > 4);
    wire.inject_bytes(burst.data(), burst.size(), burst_start);
    const uint64_t burst_end = burst_start + burst.size() * byte_us();

    for (uint64_t t = burst_end; t <= burst_end + 200000; t += byte_us())
        wire.advance_to(t, responder);

    // A bound on the backlog's latency, restored after review @6440074 finding 3: the two
    // earlier bounds were dropped in the same commit that made the engine slower, leaving
    // nothing to fail if it slowed further on exactly its known weak axis. The bound is per
    // ANSWER, so it keeps its meaning now that the number of answers is smaller: what it
    // catches is a doubling of the per-answer cost.
    const uint64_t cap_us = static_cast<uint64_t>(kMaxWire) * byte_us() + omgp::TRUNK_T_gap_us;
    const uint64_t answers = static_cast<uint64_t>(wire.transcript_size());
    const uint64_t last_tx = wire.transcript(wire.transcript_size() - 1).tx_start_us;
    INFO("drain=" << (last_tx - burst_end) << " bound=" << (answers + 2) * cap_us);
    REQUIRE(last_tx - burst_end < (answers + 2) * cap_us);

    INFO("frames=" << frames << " answers=" << wire.transcript_size()
                   << " discards=" << responder.stats().discards);
    // Full accounting over the burst plus the first request: each is a transaction or a
    // counted discard, and the wire is left empty -- nothing waiting, nothing destroyed
    // outside the engine's own books.
    REQUIRE(responder.stats().transactions + responder.stats().discards ==
            static_cast<uint32_t>(frames) + 1);
    REQUIRE(wire.pending_injected() == 0);
    REQUIRE(handler.calls == static_cast<int>(responder.stats().transactions));
    REQUIRE(wire.transcript_size() == responder.stats().transactions);
    // The measured shortfall, pinned: 15 frames behind the first request, 3 answered, 13
    // counted. Pinning it is how a silent change in either direction gets noticed -- an
    // engine that answered more without accounting for the rest would pass a bare
    // transactions + discards check.
    REQUIRE(frames == 15);
    REQUIRE(responder.stats().transactions == 3);
    REQUIRE(responder.stats().discards == 13);
    // The answers that do go out are in arrival order -- the hold is a queue, not a slot that
    // overwrites -- starting with the first request. `seq` is 4 bits (link/frame.cpp).
    REQUIRE(wire.transcript(0).seq == 1);
    for (size_t i = 1; i < wire.transcript_size(); ++i)
        REQUIRE(wire.transcript(i).seq > wire.transcript(i - 1).seq);
}

// --- #372, maintainer ruling 2026-09-11 (docs/OPEN-QUESTIONS.md "Maintainer rulings on the -
// --- pending entries", item 5; spec FR-014 and data-model.md §5 both carry the marker): ----
// --- the engine NEVER stops reading during a late wait. A completed request beyond ---------
// --- kHeldRequests is discarded and COUNTED, so the engine's reading of the bus stays ------
// --- complete and a late response keys down only on a bus it has read. The two cases below -
// --- are the ruling's own evidence: the collision the hold-and-stop corner reproduced, and -
// --- the conformant-load accounting it lost. -----------------------------------------------

TEST_CASE("the hold filling during a late wait does not blind the engine: a third station's "
          "frame arriving where the cap used to fire is never transmitted over",
          "[link][timing:T_gap]") {
    // Red team @6440074 finding 1, as re-measured in docs/OPEN-QUESTIONS.md 2026-09-07 ("the
    // Responder's late path CAN transmit into a frame it has not read"): with kHeldRequests
    // requests decoded during one late wait, poll() stopped before wire_.receive() and read
    // nothing for the rest of the deferral -- up to max_frame + T_gap -- then fired at the cap
    // against a bus it had not looked at since. Measured there: a third station's frame
    // occupying [2631, 2721) transmitted over at tx = 2661. This case rebuilds that timeline
    // from the suite's own helpers and asserts the ruled property instead.
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    // A: answered by this node; its answer is on the late path from defer_origin on.
    const uint8_t pa[] = {0xA1};
    const uint64_t end_a =
        inject_request(wire, request_bytes(kMyAddr, kPeer, false, 1, pa, sizeof pa), 1000);
    // B and C: two more requests to this node, back to back, filling the hold during the wait.
    const uint8_t pb[] = {0xB2};
    const uint8_t pc[] = {0xC3};
    const uint64_t end_b = inject_request(
        wire, request_bytes(kMyAddr, 0x07, false, 2, pb, sizeof pb), end_a + byte_us());
    const uint64_t end_c = inject_request(
        wire, request_bytes(kMyAddr, 0x07, false, 3, pc, sizeof pc), end_b + byte_us());

    // D: a THIRD station's frame, addressed elsewhere, placed so that the cap the blind engine
    // fell back to lands strictly inside it. Nothing about D violates trunk §3: the host timed
    // this node out after T_resp long ago and may legitimately open a new transaction.
    const uint64_t defer_origin = end_a + omgp::TRUNK_T_turn_max_us + 1;
    const uint64_t max_frame_us = static_cast<uint64_t>(kMaxWire) * byte_us();
    const uint64_t cap = defer_origin + max_frame_us + omgp::TRUNK_T_gap_us;
    const uint8_t pd[] = {0xD4};
    const std::vector<uint8_t> d = request_bytes(0x02, kPeer, false, 4, pd, sizeof pd);
    const uint64_t start_d = cap - 3 * byte_us();
    const uint64_t end_d = inject_request(wire, d, start_d);
    REQUIRE(start_d < cap);
    REQUIRE(cap < end_d); // the old cap fires strictly inside D -- that is the collision

    for (uint64_t t = defer_origin; t <= end_d + 4 * omgp::TRUNK_T_gap_us; t += byte_us())
        wire.advance_to(t, responder);

    // The ruled property, asserted over every frame this node put on the wire: none of them
    // overlaps D. An instant-only check would pass a transmission that STARTED before D and
    // was still keying down when D began, so the whole occupancy is compared.
    REQUIRE(wire.transcript_size() == 3);
    for (size_t i = 0; i < wire.transcript_size(); ++i) {
        const MockWire::TxRecord& tx = wire.transcript(i);
        // The response's own wire length, by the same encoding the engine used.
        const size_t n = request_bytes(tx.dst, tx.src, tx.retry, tx.seq, tx.payload, tx.len).size();
        const uint64_t tx_end = tx.tx_start_us + static_cast<uint64_t>(n) * byte_us();
        INFO("answer " << i << " [" << tx.tx_start_us << ", " << tx_end << ") vs D [" << start_d
                       << ", " << end_d << ")");
        REQUIRE((tx_end <= start_d || tx.tx_start_us >= end_d));
    }

    // A goes out T_gap after the last byte the engine read -- C's final byte -- which is
    // 1.3 ms earlier than the cap the blind engine waited for, and long before D.
    REQUIRE(wire.transcript(0).seq == 1);
    REQUIRE(wire.transcript(0).tx_start_us >= end_c + omgp::TRUNK_T_gap_us);
    REQUIRE(wire.transcript(0).tx_start_us < end_c + omgp::TRUNK_T_gap_us + byte_us());
    REQUIRE(wire.transcript(0).tx_start_us < start_d);
    // B and C were held through the wait and answered in arrival order; D was decoded, found
    // to be another node's, and discarded and counted there and then.
    REQUIRE(wire.transcript(1).seq == 2);
    REQUIRE(wire.transcript(2).seq == 3);
    REQUIRE(handler.calls == 3);
    REQUIRE(responder.stats().transactions == 3);
    REQUIRE(responder.stats().discards == 1);
    REQUIRE(responder.stats().late_responses == 3);
    REQUIRE(wire.pending_injected() == 0); // every injected byte was read, not left behind
}

TEST_CASE("a conformant offered load is fully accounted: every request is answered or counted "
          "as discarded, and the wire's receive queue never overflows",
          "[link]") {
    // Red team @bab7378 finding 2, the finding the ruling turns on. One station offers this
    // node a request every 300 us -- ~37% bus occupancy, nothing in it violating trunk §3 or
    // §9 -- against a 1 ms poll(). Under hold-and-stop the engine answered 7 of 74, MockWire's
    // own 568-byte receive queue overflowed at t = 23 ms, and stats().discards stayed 0: the
    // loss happened at the wire, outside the engine, and nothing counted it. The ruling's
    // property is accounting, not throughput -- an overloaded node may still fail to answer,
    // but every request it consumed is either answered or counted.
    FakeClock clock;
    MockWire wire(clock);
    RecordingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    constexpr uint8_t kOther = 0x07;
    const uint8_t p[] = {0x5A, 0x5B, 0x5C};
    const uint64_t offer_period = 300, poll_period = 1000;
    const int offered = 74;
    const size_t frame_len = request_bytes(kMyAddr, kOther, false, 0, p, sizeof p).size();
    // ~37%: the occupancy the finding was measured at, recomputed from the codec rather than
    // asserted as a comment (a stuffing change would otherwise move it silently).
    INFO("frame_len=" << frame_len);
    REQUIRE(frame_len * byte_us() * 100 / offer_period >= 35);
    REQUIRE(frame_len * byte_us() < offer_period); // requests never overlap on the wire

    uint64_t next_offer = 1000;
    int injected = 0;
    // Polls run on past the last offer so nothing is left in flight: the accounting below is
    // exact rather than bounded.
    const uint64_t last_offer = next_offer + static_cast<uint64_t>(offered - 1) * offer_period;
    for (uint64_t t = 0; t <= last_offer + 40000; t += poll_period) {
        while (injected < offered && next_offer < t + poll_period) {
            const uint8_t seq = static_cast<uint8_t>(injected & 0x0F);
            inject_request(wire, request_bytes(kMyAddr, kOther, false, seq, p, sizeof p),
                           next_offer);
            ++injected;
            next_offer += offer_period;
        }
        wire.advance_to(t, responder);
    }
    REQUIRE(injected == offered);

    // Full accounting: every offered request was read off the wire, and each is either a
    // transaction (answered) or a counted discard. Nothing is lost silently, and nothing is
    // still sitting unread in the wire's queue.
    INFO("transactions=" << responder.stats().transactions
                         << " discards=" << responder.stats().discards);
    REQUIRE(wire.pending_injected() == 0);
    REQUIRE(responder.stats().transactions + responder.stats().discards ==
            static_cast<uint32_t>(offered));
    REQUIRE(wire.transcript_size() == responder.stats().transactions);
    REQUIRE(handler.calls == static_cast<int>(responder.stats().transactions));
    // The loss under this load is real and this case does not pretend otherwise -- it is
    // VISIBLE, which is the whole of what the ruling requires of an overloaded node.
    REQUIRE(responder.stats().discards > 0);
    REQUIRE(responder.stats().transactions > 0);
    // No harness fault (MockWire REQUIREs on RX-queue overflow inside advance_to(), so a
    // regression here fails as a harness fault rather than as an assertion; this is the
    // explicit check that none was raised and swallowed).
    REQUIRE(wire.take_fault() == nullptr);
}
