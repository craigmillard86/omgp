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
