// Trunk L2 responder engine (spec 002 US3, T033). trunk §3 (media access) and §7 (retry
// rule); contracts/link-cpp.md "Responder engine"; data-model.md §5 "Responder". Written
// ahead of link/responder.{hpp,cpp} (T035): this file compiles only once T035 lands
// (tasks.md: "T035 ... make T033 and T034 pass" runs the other way) — the same intended
// TDD red state as tests/unit/test_link_loop.cpp (T034), recorded in the PR body per
// CLAUDE.md rule 8. Every assertion is driven through the scripted MockWire + FakeClock
// harness with simulated time advanced explicitly (CLAUDE.md rule 3): nothing here sleeps
// or reads a wall clock.
#include "catch_amalgamated.hpp"
#include "fake_clock.hpp"
#include "heap_guard.hpp"
#include "link/crc16.hpp"
#include "link/frame.hpp"
#include "link/link_types.hpp"
#include "link/responder.hpp"
#include "mock_wire.hpp"
#include "omgp_protocol.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace omgp::link;
using omgp_test::encode_crc_corrupted;
using omgp_test::FakeClock;
using omgp_test::MockWire;

namespace {

// The Responder under test always answers as this address; requests arrive "from" the
// host address, mirroring a real trunk request (contracts/link-cpp.md "Master engine").
constexpr uint8_t kMyAddr = 0x01;
constexpr uint8_t kHostAddr = omgp::ADDR_host;

// Independently encodes the wire bytes a frame with these fields would produce — used only
// to build request bytes and to cross-check transcript contents, never to verify
// Responder's own encoder indirectly through itself (mirrors test_link_master.cpp).
std::vector<uint8_t> encode_expected(uint8_t f_dst, uint8_t f_src, bool response, bool retry,
                                     uint8_t seq, const uint8_t* payload, size_t len) {
    FrameFields f{f_dst, f_src, response, retry, seq, static_cast<uint8_t>(len), payload};
    uint8_t out[kMaxWire];
    size_t written = 0;
    REQUIRE(encode_frame(f, out, sizeof out, written) == Status::Ok);
    return std::vector<uint8_t>(out, out + written);
}

uint64_t byte_us() {
    return byte_time_us(omgp::TRUNK_bit_rate);
}

// contracts/link-cpp.md "Responder engine": counts invocations so "handler invoked once
// per new seq" is directly assertable, and echoes the request payload so the response
// content is trivially predictable (mirrors test_link_loop.cpp's CountingHandler).
struct CountingHandler : RequestHandler {
    unsigned invocations = 0;
    size_t handle(const uint8_t* req, size_t len, uint8_t* resp, size_t cap) override {
        ++invocations;
        REQUIRE(len <= cap);
        if (len > 0)
            std::memcpy(resp, req, len);
        return len;
    }
};

} // namespace

// --- timing: default turnaround, and the constructor's clamp ------------------------------

TEST_CASE("a request is answered with its first byte exactly at request_end + T_turn_min by "
          "default",
          "[timing:T_turn_min]") {
    FakeClock clock;
    MockWire wire(clock);
    CountingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload[] = {0xAA};
    const auto req = encode_expected(kMyAddr, kHostAddr, false, false, 0, payload, sizeof payload);
    const uint64_t req_start = 1000;
    wire.inject_bytes(req.data(), req.size(), req_start);
    const uint64_t req_end = req_start + static_cast<uint64_t>(req.size()) * byte_us();

    wire.advance_to(req_end, responder); // drains the request; handler invoked once
    REQUIRE(handler.invocations == 1);
    REQUIRE(wire.transcript_size() == 0); // not yet due

    const uint64_t resp_start = req_end + omgp::TRUNK_T_turn_min_us;
    wire.advance_to(resp_start, responder);
    REQUIRE(wire.transcript_size() == 1);
    const auto& resp = wire.transcript(0);
    REQUIRE(resp.tx_start_us == resp_start);
    REQUIRE(resp.dst == kHostAddr);
    REQUIRE(resp.src == kMyAddr);
    REQUIRE(resp.response);
    REQUIRE_FALSE(resp.retry);
    REQUIRE(resp.seq == 0);
    REQUIRE(resp.len == sizeof payload);
    REQUIRE(std::memcmp(resp.payload, payload, sizeof payload) == 0);
    REQUIRE(responder.stats().transactions == 1);
    REQUIRE(responder.stats().late_responses == 0);
}

TEST_CASE("a constructor turnaround_us above T_turn_max is clamped to T_turn_max",
          "[timing:T_turn_max]") {
    FakeClock clock;
    MockWire wire(clock);
    CountingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr, omgp::TRUNK_T_turn_max_us + 1000);

    const uint8_t payload[] = {0xBB};
    const auto req = encode_expected(kMyAddr, kHostAddr, false, false, 0, payload, sizeof payload);
    const uint64_t req_start = 0;
    wire.inject_bytes(req.data(), req.size(), req_start);
    const uint64_t req_end = req_start + static_cast<uint64_t>(req.size()) * byte_us();
    wire.advance_to(req_end, responder);

    const uint64_t just_before = req_end + omgp::TRUNK_T_turn_max_us - 1;
    wire.advance_to(just_before, responder);
    REQUIRE(wire.transcript_size() == 0); // clamped deadline not yet reached

    const uint64_t at_max = req_end + omgp::TRUNK_T_turn_max_us;
    wire.advance_to(at_max, responder);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).tx_start_us == at_max);
    REQUIRE(responder.stats().late_responses == 0); // on time, at the clamped deadline itself
}

TEST_CASE("a constructor turnaround_us below T_turn_min is clamped to T_turn_min",
          "[timing:T_turn_min]") {
    FakeClock clock;
    MockWire wire(clock);
    CountingHandler handler;
    REQUIRE(omgp::TRUNK_T_turn_min_us > 0); // else 0 below is not actually below the minimum
    Responder responder(wire, clock, handler, kMyAddr, /*turnaround_us=*/0);

    const uint8_t payload[] = {0xBC};
    const auto req = encode_expected(kMyAddr, kHostAddr, false, false, 0, payload, sizeof payload);
    const uint64_t req_start = 0;
    wire.inject_bytes(req.data(), req.size(), req_start);
    const uint64_t req_end = req_start + static_cast<uint64_t>(req.size()) * byte_us();
    wire.advance_to(req_end, responder);

    REQUIRE(wire.transcript_size() == 0); // never fires before the clamped minimum
    const uint64_t clamped_due = req_end + omgp::TRUNK_T_turn_min_us;
    wire.advance_to(clamped_due, responder);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).tx_start_us == clamped_due);
}

// --- retry / replay semantics (data-model.md §5) -------------------------------------------

TEST_CASE("a retry of the already-answered sequence is replayed verbatim without invoking the "
          "handler again",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    CountingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload[] = {0xCC};
    const auto req = encode_expected(kMyAddr, kHostAddr, false, false, 3, payload, sizeof payload);
    uint64_t t = 0;
    wire.inject_bytes(req.data(), req.size(), t);
    t += static_cast<uint64_t>(req.size()) * byte_us();
    wire.advance_to(t, responder);
    t += omgp::TRUNK_T_turn_min_us;
    wire.advance_to(t, responder);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(handler.invocations == 1);
    const auto first = wire.transcript(0);

    // Master's own retry (contracts/link-cpp.md "Master engine"): the SAME seq, retry set.
    const auto retry_req =
        encode_expected(kMyAddr, kHostAddr, false, true, 3, payload, sizeof payload);
    t += omgp::TRUNK_T_gap_us;
    wire.inject_bytes(retry_req.data(), retry_req.size(), t);
    t += static_cast<uint64_t>(retry_req.size()) * byte_us();
    wire.advance_to(t, responder);
    REQUIRE(handler.invocations == 1); // not invoked again

    t += omgp::TRUNK_T_turn_min_us;
    wire.advance_to(t, responder);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(responder.stats().replays_served == 1);
    REQUIRE(responder.stats().transactions == 2); // both requests handled

    const auto second = wire.transcript(1);
    REQUIRE(second.dst == first.dst);
    REQUIRE(second.src == first.src);
    REQUIRE(second.response == first.response);
    REQUIRE(second.retry == first.retry); // the buffered bytes are replayed unmodified
    REQUIRE(second.seq == first.seq);
    REQUIRE(second.len == first.len);
    REQUIRE(std::memcmp(second.payload, first.payload, first.len) == 0);
}

TEST_CASE("a retry-flagged request with a different sequence than the buffered one is treated "
          "as new: the handler is invoked and no replay is served",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    CountingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload1[] = {0x01};
    const auto req1 =
        encode_expected(kMyAddr, kHostAddr, false, false, 5, payload1, sizeof payload1);
    uint64_t t = 0;
    wire.inject_bytes(req1.data(), req1.size(), t);
    t += static_cast<uint64_t>(req1.size()) * byte_us();
    wire.advance_to(t, responder);
    t += omgp::TRUNK_T_turn_min_us;
    wire.advance_to(t, responder);
    REQUIRE(handler.invocations == 1);

    const uint8_t payload2[] = {0x02};
    // retry bit set, but a DIFFERENT seq than the one just buffered (6 vs 5).
    const auto req2 =
        encode_expected(kMyAddr, kHostAddr, false, true, 6, payload2, sizeof payload2);
    t += omgp::TRUNK_T_gap_us;
    wire.inject_bytes(req2.data(), req2.size(), t);
    t += static_cast<uint64_t>(req2.size()) * byte_us();
    wire.advance_to(t, responder);
    REQUIRE(handler.invocations == 2); // treated as a new request, not a replay

    t += omgp::TRUNK_T_turn_min_us;
    wire.advance_to(t, responder);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(responder.stats().replays_served == 0);
    REQUIRE(wire.transcript(1).seq == 6);
    REQUIRE(wire.transcript(1).retry); // echoed from this (new) request
}

TEST_CASE("a retry-flagged request with no prior answer is treated as new", "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    CountingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload[] = {0x09};
    const auto req =
        encode_expected(kMyAddr, kHostAddr, false, /*retry=*/true, 2, payload, sizeof payload);
    uint64_t t = 0;
    wire.inject_bytes(req.data(), req.size(), t);
    t += static_cast<uint64_t>(req.size()) * byte_us();
    wire.advance_to(t, responder);
    REQUIRE(handler.invocations == 1);
    REQUIRE(responder.stats().replays_served == 0);

    t += omgp::TRUNK_T_turn_min_us;
    wire.advance_to(t, responder);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).retry); // echoed from the request
}

// --- discards: not addressed here, or not a valid frame at all -----------------------------

TEST_CASE("a request addressed to another node is discarded: nothing transmitted, handler not "
          "invoked, discards counted",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    CountingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload[] = {0x77};
    const auto req =
        encode_expected(/*dst=*/0x02, kHostAddr, false, false, 0, payload, sizeof payload);
    uint64_t t = 0;
    wire.inject_bytes(req.data(), req.size(), t);
    t += static_cast<uint64_t>(req.size()) * byte_us();
    wire.advance_to(t, responder);
    REQUIRE(handler.invocations == 0);
    REQUIRE(responder.stats().discards == 1);
    REQUIRE(responder.stats().transactions == 0);

    // Never transmits outside a window: no bytes when nothing was addressed to it, however
    // far the clock is advanced afterward.
    wire.advance_to(t + omgp::TRUNK_T_turn_max_us + 1000, responder);
    REQUIRE(wire.transcript_size() == 0);
}

TEST_CASE("a request whose response bit is already set (not a request at all) is discarded",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    CountingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload[] = {0x78};
    const auto stray =
        encode_expected(kMyAddr, kHostAddr, /*response=*/true, false, 0, payload, sizeof payload);
    uint64_t t = 0;
    wire.inject_bytes(stray.data(), stray.size(), t);
    t += static_cast<uint64_t>(stray.size()) * byte_us();
    wire.advance_to(t, responder);
    REQUIRE(handler.invocations == 0);
    REQUIRE(responder.stats().discards == 1);

    wire.advance_to(t + omgp::TRUNK_T_turn_max_us + 1000, responder);
    REQUIRE(wire.transcript_size() == 0);
}

TEST_CASE("a CRC-corrupt frame is discarded: nothing transmitted, handler not invoked, "
          "discards counted",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    CountingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload[] = {0x88};
    FrameFields f{kMyAddr, kHostAddr, false, false, 0, sizeof payload, payload};
    uint8_t buf[kMaxWire];
    const size_t written = encode_crc_corrupted(f, buf, sizeof buf);
    REQUIRE(written > 0);
    uint64_t t = 0;
    wire.inject_bytes(buf, written, t);
    t += static_cast<uint64_t>(written) * byte_us();
    wire.advance_to(t, responder);
    REQUIRE(handler.invocations == 0);
    REQUIRE(responder.stats().discards == 1);

    wire.advance_to(t + omgp::TRUNK_T_turn_max_us + 1000, responder);
    REQUIRE(wire.transcript_size() == 0);
}

// --- FR-014: late poll ----------------------------------------------------------------------

TEST_CASE("the first poll() after request_end + T_turn_max transmits immediately and counts a "
          "late response",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    CountingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload[] = {0x66};
    const auto req = encode_expected(kMyAddr, kHostAddr, false, false, 4, payload, sizeof payload);
    const uint64_t req_start = 0;
    wire.inject_bytes(req.data(), req.size(), req_start);
    const uint64_t req_end = req_start + static_cast<uint64_t>(req.size()) * byte_us();
    wire.advance_to(req_end, responder); // drains the request; deadline scheduled at +T_turn_min

    // No poll() call until well past request_end + T_turn_max: models a scheduler that
    // missed its usual cadence (spec FR-014).
    const uint64_t late_poll_at = req_end + omgp::TRUNK_T_turn_max_us + 5000;
    wire.advance_to(late_poll_at, responder);
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).tx_start_us == late_poll_at); // transmitted AT the late instant
    REQUIRE(responder.stats().late_responses == 1);
}

// --- CLAUDE.md rule 5 / research R-10: handling + responding allocates nothing -------------

TEST_CASE("handling a request and transmitting its response allocates nothing", "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    CountingHandler handler;
    Responder responder(wire, clock, handler, kMyAddr);

    const uint8_t payload[] = {0x05};
    const auto req = encode_expected(kMyAddr, kHostAddr, false, false, 0, payload, sizeof payload);
    const uint64_t req_start = 0;
    wire.inject_bytes(req.data(), req.size(), req_start);
    const uint64_t req_end = req_start + static_cast<uint64_t>(req.size()) * byte_us();

    // advance_to(t) (clock-only) runs INFO/REQUIRE (Catch2 macros that allocate) and must
    // stay outside the guarded region below (test_link_master.cpp's own convention) — only
    // poll() itself, a bare engine call with no Catch2 macro inside, is measured.
    wire.advance_to(req_end);
    HEAP_FREE_SCOPE({ responder.poll(req_end); }); // drains + invokes the handler
    REQUIRE(handler.invocations == 1);

    const uint64_t resp_start = req_end + omgp::TRUNK_T_turn_min_us;
    wire.advance_to(resp_start);
    HEAP_FREE_SCOPE({ responder.poll(resp_start); }); // transmits the response
    REQUIRE(wire.transcript_size() == 1);
}
