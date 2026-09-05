// Trunk L2 test-support contract (spec 002 T011): pins the scheduling and safety contract
// of MockWire, the scripted ByteWire test transport, so every later link/ engine test
// (Master, Responder, health, bus-fault) can trust the mock it is driven by. Written from
// contracts/mock-wire.md and contracts/byte-wire-and-clock.md, not from
// tests/support/mock_wire.{hpp,cpp} directly (T010, #28).
#include "catch_amalgamated.hpp"
#include "fake_clock.hpp"
#include "heap_guard.hpp"
#include "link/frame.hpp"
#include "link/link_types.hpp"
#include "mock_wire.hpp"
#include "omgp_protocol.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>

using namespace omgp::link;
using omgp_test::FakeClock;
using omgp_test::Kind;
using omgp_test::MockWire;
using omgp_test::Step;
using omgp_test::xorshift32_next;

namespace {

// Encodes a minimal, valid request frame addressed to `dst`, as a real Master would hand
// to ByteWire::transmit(). One payload byte is enough: these tests assert MockWire's
// scheduling/queueing behaviour, not frame content.
std::vector<uint8_t> encode_request(uint8_t dst, uint8_t seq, uint8_t payload_byte = 0xAB) {
    const uint8_t payload[1] = {payload_byte};
    FrameFields f{dst, omgp::ADDR_host, false, false, seq, 1, payload};
    uint8_t out[kMaxWire];
    size_t written = 0;
    REQUIRE(encode_frame(f, out, sizeof out, written) == Status::Ok);
    return std::vector<uint8_t>(out, out + written);
}

// Independently builds the wire bytes MockWire's default Respond answer must produce
// (contracts/mock-wire.md "Respond": the node's RequestHandler answers by mirroring the
// request) by deframing `req` and re-encoding a response frame from its fields — not by
// calling into mock_wire.cpp, so this stays an independent check of its output.
std::vector<uint8_t> expected_respond_answer(const std::vector<uint8_t>& req) {
    Deframer d;
    FrameView view{};
    bool got = false;
    for (uint8_t b : req)
        if (d.feed(b, view))
            got = true;
    REQUIRE(got);

    FrameFields resp{view.f.src, view.f.dst, true, false, view.f.seq, view.f.len, view.f.payload};
    uint8_t out[kMaxWire];
    size_t written = 0;
    REQUIRE(encode_frame(resp, out, sizeof out, written) == Status::Ok);
    return std::vector<uint8_t>(out, out + written);
}

// The single max-payload request the RX-overflow case below retransmits unchanged: seq and
// content don't affect whether the RX queue overflows, only total byte volume does, so one
// encode suffices. kMaxWire is sized for exactly this worst case (2 + 2*(kHeaderLen +
// LIMIT_max_l3_payload + kCrcLen) == kMaxWire), so encode_frame cannot refuse it while that
// invariant holds.
std::vector<uint8_t> encode_max_payload_request() {
    uint8_t big_payload[omgp::LIMIT_max_l3_payload];
    std::fill(std::begin(big_payload), std::end(big_payload), uint8_t{0x11});
    FrameFields f{0x01, omgp::ADDR_host, false, false, 0, sizeof big_payload, big_payload};
    uint8_t out[kMaxWire];
    size_t written = 0;
    REQUIRE(encode_frame(f, out, sizeof out, written) == Status::Ok);
    return std::vector<uint8_t>(out, out + written);
}

} // namespace

TEST_CASE("MockWire::transmit reports the modelled timing at the reference bit rate",
          "[link][mock_wire][timing:bit_rate]") {
    FakeClock clock;
    MockWire wire(clock);
    REQUIRE(wire.bit_rate() == omgp::TRUNK_bit_rate);

    const std::vector<uint8_t> req = encode_request(0x01, 0);
    const uint64_t now = 1000;
    uint64_t end = 0;
    HEAP_FREE_SCOPE({ end = wire.transmit(req.data(), req.size(), now); });

    // contracts/byte-wire-and-clock.md: transmit() returns now + n * byte_time_us(rate).
    REQUIRE(end == now + req.size() * byte_time_us(omgp::TRUNK_bit_rate));
    REQUIRE(end == now + req.size() * 10); // 10us/byte at 1Mbit/s (docs/trunk-link-layer.md §9)
}

TEST_CASE("MockWire::transmit reports the modelled timing at the fallback bit rate",
          "[link][mock_wire][timing:bit_rate_fallback]") {
    FakeClock clock;
    MockWire wire(clock);
    wire.set_bit_rate(omgp::TRUNK_bit_rate_fallback);
    REQUIRE(wire.bit_rate() == omgp::TRUNK_bit_rate_fallback);

    const std::vector<uint8_t> req = encode_request(0x01, 0);
    const uint64_t now = 2000;
    uint64_t end = 0;
    HEAP_FREE_SCOPE({ end = wire.transmit(req.data(), req.size(), now); });

    REQUIRE(end == now + req.size() * byte_time_us(omgp::TRUNK_bit_rate_fallback));
    REQUIRE(end == now + req.size() * 86); // 86us/byte at 115.2kbit/s (integer model)
}

TEST_CASE("MockWire::receive releases queued RX bytes strictly in ascending start-instant "
          "order, and never before their start instant",
          "[link][mock_wire]") {
    FakeClock clock;

    // Node 0x01 gets a long ("late response") turnaround; node 0x02 keeps the default
    // (TRUNK_T_turn_min_us) short one. Node 1 is transmitted to first, but because its
    // delay is much longer, its response bytes start *later* than node 2's, which is
    // transmitted to second — exercising real reordering, not just FIFO/insertion order.
    // Positional init (node, kind, delay_us; count/seed take their defaults): Step is a
    // C++17 aggregate and this project builds C++17, so `.node = ...` designated init (a
    // C++20 feature; PR #112 review finding) is avoided here.
    const Step slow[] = {{0x01, Kind::Respond, 500}};
    // Declared after `slow` (not before): mock_wire.hpp documents that `steps` must outlive
    // the MockWire pointing at it. With the opposite order, ~MockWire() would run against
    // an already-destroyed `slow` (PR #112 review finding — benign today only because the
    // destructor never reads scripts_, but not something to rely on).
    MockWire wire(clock);
    wire.set_script(0x01, slow, 1);

    const std::vector<uint8_t> req1 = encode_request(0x01, 1);
    uint64_t tx_end1 = 0;
    HEAP_FREE_SCOPE({ tx_end1 = wire.transmit(req1.data(), req1.size(), 0); });

    const std::vector<uint8_t> req2 = encode_request(0x02, 2);
    uint64_t tx_end2 = 0;
    HEAP_FREE_SCOPE({ tx_end2 = wire.transmit(req2.data(), req2.size(), tx_end1); });

    const std::vector<uint8_t> resp1 = expected_respond_answer(req1);
    const std::vector<uint8_t> resp2 = expected_respond_answer(req2);

    const uint64_t t0_1 = tx_end1 + 500;
    const uint64_t t0_2 = tx_end2 + omgp::TRUNK_T_turn_min_us;
    REQUIRE(t0_2 < t0_1); // otherwise this test isn't exercising reordering at all
    const uint64_t byte_us = byte_time_us(omgp::TRUNK_bit_rate);

    uint8_t byte;
    uint64_t start_us;
    bool got = false;

    // Nothing is due yet: neither response's first byte has reached its start instant.
    wire.advance_to(tx_end2);
    HEAP_FREE_SCOPE({ got = wire.receive(byte, start_us); });
    REQUIRE_FALSE(got);

    // One microsecond before node 2's first byte's start instant: still not due. Pins the
    // exclusive boundary — receive() gates on `start_us > now`, not `>=` — the same way the
    // drains below already pin the inclusive one (PR #112 review finding: previously
    // unchecked, so a byte released one microsecond early would have survived undetected).
    wire.advance_to(t0_2 - 1);
    HEAP_FREE_SCOPE({ got = wire.receive(byte, start_us); });
    REQUIRE_FALSE(got);

    // Advance to exactly node 2's last byte's start instant: all of node 2's response is
    // due, none of node 1's (t0_1 is still well in the future - checked above).
    wire.advance_to(t0_2 + (resp2.size() - 1) * byte_us);
    std::vector<uint8_t> drained;
    uint64_t last_start = 0;
    bool have_last = false;
    size_t idx = 0;
    for (;;) {
        HEAP_FREE_SCOPE({ got = wire.receive(byte, start_us); });
        if (!got)
            break;
        if (have_last)
            REQUIRE(start_us > last_start); // strictly ascending start-instant order
        // Pins the exact schedule, not just its ordering and outer bounds
        // (contracts/mock-wire.md §Scheduling: byte i fires at t0 + i * byte_time_us(rate);
        // PR #112 review finding — a schedule at any other spacing than byte_us previously
        // survived undetected as long as it stayed ordered).
        REQUIRE(start_us == t0_2 + idx * byte_us);
        last_start = start_us;
        have_last = true;
        drained.push_back(byte);
        ++idx;
    }
    REQUIRE(drained == resp2); // node 2's bytes only
    // node 1's bytes stay queued (in the future)
    HEAP_FREE_SCOPE({ got = wire.receive(byte, start_us); });
    REQUIRE_FALSE(got);

    // Advance past node 1's response too; it drains next, still in ascending order.
    wire.advance_to(t0_1 + (resp1.size() - 1) * byte_us);
    drained.clear();
    have_last = false;
    idx = 0;
    for (;;) {
        HEAP_FREE_SCOPE({ got = wire.receive(byte, start_us); });
        if (!got)
            break;
        if (have_last)
            REQUIRE(start_us > last_start);
        REQUIRE(start_us == t0_1 + idx * byte_us);
        last_start = start_us;
        have_last = true;
        drained.push_back(byte);
        ++idx;
    }
    REQUIRE(drained == resp1);
}

TEST_CASE("MockWire's xorshift32 PRNG is byte-for-byte reproducible for a given Step::seed",
          "[link][mock_wire]") {
    // Garbage/Babble (T030) aren't implemented yet, so this exercises the PRNG mock_wire.hpp
    // exposes directly (contracts/mock-wire.md: "All randomness from Step::seed through an
    // xorshift32 ... the same script reproduces byte-for-byte") rather than through a script.
    const Step step{.node = 0xFF, .kind = Kind::Garbage, .seed = 0xC0FFEEu};

    auto byte_stream = [](uint32_t seed, size_t n) {
        std::vector<uint8_t> bytes;
        uint32_t state = seed;
        for (size_t i = 0; i < n; ++i)
            bytes.push_back(static_cast<uint8_t>(xorshift32_next(state) & 0xFF));
        return bytes;
    };

    const std::vector<uint8_t> run1 = byte_stream(step.seed, 32);
    const std::vector<uint8_t> run2 = byte_stream(step.seed, 32);
    REQUIRE(run1 == run2);

    // Not vacuously true: a degenerate (constant) stream would also be "reproducible".
    REQUIRE(std::adjacent_find(run1.begin(), run1.end(), std::not_equal_to<>()) != run1.end());

    // seed == 0 is Step::seed's default (every step that doesn't set one explicitly) and
    // xorshift32_next()'s one fixed point; mock_wire.cpp repairs it (substituting
    // 0xFFFFFFFF before advancing) rather than letting it emit a constant all-zero stream
    // forever. Cover that repaired path directly, not just the explicit 0xC0FFEE seed
    // above — it's the one every default-seed Garbage/Babble script (T030) will take.
    const std::vector<uint8_t> zero_run1 = byte_stream(0, 32);
    const std::vector<uint8_t> zero_run2 = byte_stream(0, 32);
    REQUIRE(zero_run1 == zero_run2);
    REQUIRE(std::adjacent_find(zero_run1.begin(), zero_run1.end(), std::not_equal_to<>()) !=
            zero_run1.end());
}

TEST_CASE("MockWire's RX queue capacity overflow is a hard test failure, never a silent drop",
          "[link][mock_wire]") {
    // contracts/mock-wire.md "Capacity": the RX queue is a fixed 4*kMaxWire bytes; enqueueing
    // past it must fail loudly, never truncate or drop silently. This test drives enough
    // Respond-scheduled answers through the queue - with nothing ever draining it via
    // receive() - to exceed that capacity, then asserts the *specific* RX-capacity fault via
    // MockWire::take_fault() with an ordinary REQUIRE.
    //
    // PR #112 review, finding 1: the previous version used Catch2's [!shouldfail] tag, which
    // inverts pass/fail at whole-case granularity — any failure anywhere in the case
    // (including an unrelated encode_frame regression, of which this case used to make ~12)
    // reported the case as passing, without ever reaching MockWire::enqueue()'s capacity
    // guard. take_fault() lets this case assert the *specific* fault message with a normal
    // REQUIRE instead, so it can only pass for the reason it claims to.
    FakeClock clock;
    MockWire wire(clock);
    constexpr size_t kRxCapacity = 4 * kMaxWire;

    const std::vector<uint8_t> req = encode_max_payload_request();

    uint64_t now = 0;
    // Each Respond answer enqueues roughly req.size() RX bytes; send enough more to exceed
    // the fixed-size queue regardless of exact stuffed length (never drained via receive()).
    const size_t iterations = kRxCapacity / req.size() + 5;
    for (size_t i = 0; i < iterations; ++i)
        HEAP_FREE_SCOPE({ now = wire.transmit(req.data(), req.size(), now); });

    const char* fault = wire.take_fault();
    REQUIRE(fault != nullptr);
    REQUIRE(std::strcmp(fault, "MockWire: RX queue capacity exceeded (4 * kMaxWire)") == 0);
}

// --- contracts/mock-wire.md Step table: Kind::CrcError / Kind::Duplicate, at the harness -
// level directly (PR #137 review, MEDIUM: implemented and used throughout
// tests/unit/test_link_master.cpp, T029's own slice of T030, but never asserted here) -----

TEST_CASE("Kind::CrcError answers with a CRC-invalid frame of the same wire length as the "
          "real response would have been, at request_end + delay_us",
          "[link][mock_wire]") {
    FakeClock clock;
    const Step crc_step[] = {{0x01, Kind::CrcError, 30}};
    MockWire wire(clock);
    wire.set_script(0x01, crc_step, 1);

    const std::vector<uint8_t> req = encode_request(0x01, 0);
    uint64_t tx_end = 0;
    HEAP_FREE_SCOPE({ tx_end = wire.transmit(req.data(), req.size(), 0); });

    const std::vector<uint8_t> real_answer = expected_respond_answer(req);
    const uint64_t answer_end =
        tx_end + 30 +
        static_cast<uint64_t>(real_answer.size()) * byte_time_us(omgp::TRUNK_bit_rate);
    wire.advance_to(answer_end);

    Deframer d;
    FrameView view{};
    bool delivered = false;
    size_t drained = 0;
    uint8_t byte;
    uint64_t start_us;
    while (wire.receive(byte, start_us)) {
        if (d.feed(byte, view))
            delivered = true;
        ++drained;
    }
    // contracts/mock-wire.md: "the real response with its last CRC byte XOR 0xFF" — never
    // an intact frame, but (docs/OPEN-QUESTIONS.md 2026-09-05: the byte itself is not a
    // literal XOR 0xFF at four values) always the same wire length as the real one.
    REQUIRE_FALSE(delivered);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadCrc)] == 1);
    REQUIRE(drained == real_answer.size());
}

TEST_CASE("Kind::Duplicate answers with the real response, then the identical bytes again "
          "delay_us after the first copy's own end",
          "[link][mock_wire]") {
    FakeClock clock;
    const Step dup_step[] = {{0x01, Kind::Duplicate, 40}};
    MockWire wire(clock);
    wire.set_script(0x01, dup_step, 1);

    const std::vector<uint8_t> req = encode_request(0x01, 0);
    uint64_t tx_end = 0;
    HEAP_FREE_SCOPE({ tx_end = wire.transmit(req.data(), req.size(), 0); });

    const std::vector<uint8_t> answer = expected_respond_answer(req);
    const uint64_t bt = byte_time_us(omgp::TRUNK_bit_rate);
    // "the real response" (contracts/mock-wire.md's Respond row): first byte at
    // request_end + TRUNK_T_turn_min_us, the default delay — Duplicate's own delay_us
    // governs only the SECOND copy, below.
    const uint64_t first_end =
        tx_end + omgp::TRUNK_T_turn_min_us + static_cast<uint64_t>(answer.size()) * bt;
    const uint64_t second_end = first_end + 40 + static_cast<uint64_t>(answer.size()) * bt;
    wire.advance_to(second_end);

    std::vector<uint8_t> drained;
    uint64_t second_copy_start = 0;
    uint8_t byte;
    uint64_t start_us;
    while (wire.receive(byte, start_us)) {
        if (drained.size() == answer.size())
            second_copy_start = start_us;
        drained.push_back(byte);
    }
    REQUIRE(drained.size() == 2 * answer.size());
    REQUIRE(std::equal(drained.begin(), drained.begin() + static_cast<long>(answer.size()),
                       answer.begin()));
    REQUIRE(std::equal(drained.begin() + static_cast<long>(answer.size()), drained.end(),
                       answer.begin()));
    // contracts/mock-wire.md: the second copy starts delay_us after the FIRST copy's OWN
    // end, not after the request's own end.
    REQUIRE(second_copy_start == first_end + 40);
}
