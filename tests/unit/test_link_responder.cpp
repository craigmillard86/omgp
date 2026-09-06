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

    size_t handle(const uint8_t* req, size_t len, uint8_t* resp, size_t cap) override {
        ++calls;
        REQUIRE(len <= cap);
        if (len > 0)
            std::memcpy(resp, req, len);
        return len;
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
    REQUIRE(wire.transcript_size() == 0);
    REQUIRE(handler.calls == 1);

    wire.advance_to(deadline, responder);
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
    REQUIRE(responder.stats().replays_served == 0);
    REQUIRE(responder.stats().transactions == 1);
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
