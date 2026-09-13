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
#include "link/responder.hpp"
#include "mock_wire.hpp"
#include "omgp_protocol.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
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

// Independently builds the wire bytes MockWire must emit for a response to `req` carrying
// `payload`: the request's mirrored header (dst/src swapped, response bit set, retry clear,
// seq echoed) re-encoded by the real codec — not by calling into mock_wire.cpp, so this
// stays an independent check of its output.
std::vector<uint8_t> expected_answer_with(const std::vector<uint8_t>& req, const uint8_t* payload,
                                          uint8_t len) {
    Deframer d;
    FrameView view{};
    bool got = false;
    for (uint8_t b : req)
        if (d.feed(b, view))
            got = true;
    REQUIRE(got);

    FrameFields resp{view.f.src, view.f.dst, true, false, view.f.seq, len, payload};
    uint8_t out[kMaxWire];
    size_t written = 0;
    REQUIRE(encode_frame(resp, out, sizeof out, written) == Status::Ok);
    return std::vector<uint8_t>(out, out + written);
}

// The wire bytes of MockWire's NO-HANDLER fallback answer: the echo of the request's own
// payload. contracts/mock-wire.md:16 gives the answer to the node's `RequestHandler`; the
// echo is the ratified interim default (docs/OPEN-QUESTIONS.md 2026-09-01, ruled
// 2026-09-03) and since #147 it survives only for a `dst` with no handler registered.
std::vector<uint8_t> expected_respond_answer(const std::vector<uint8_t>& req) {
    Deframer d;
    FrameView view{};
    bool got = false;
    for (uint8_t b : req)
        if (d.feed(b, view))
            got = true;
    REQUIRE(got);
    // view.f.payload points into `d`'s own accumulator and stays valid while `d` lives.
    return expected_answer_with(req, view.f.payload, view.f.len);
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
    uint64_t first_start = 0;
    uint64_t last_start = 0;
    while (wire.receive(byte, start_us)) {
        if (drained == 0)
            first_start = start_us;
        last_start = start_us;
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
    // "at request_end + delay_us": the instant itself, pinned like the Duplicate case's.
    // The drain count alone passes for every delay SMALLER than the scripted one (receive()
    // releases a byte once start_us <= now), and test_link_master's CRC-timing cases derive
    // their expected instants from this delay (PR #137 review @2627be9, MEDIUM).
    REQUIRE(first_start == tx_end + 30);
    REQUIRE(last_start ==
            tx_end + 30 +
                static_cast<uint64_t>(real_answer.size() - 1) * byte_time_us(omgp::TRUNK_bit_rate));
}

// The CRC high byte of an encoded frame, read back off its wire tail: `... lo hi FLAG`, or
// `... lo ESC hi^xor FLAG` when hi is FLAG/ESCAPE itself (lo's own stuffing cannot put an
// ESCAPE at [n-3]: a stuffed lo reads `ESC lo^xor` there, and lo^xor is never ESCAPE).
uint8_t crc_hi_on_wire(const std::vector<uint8_t>& wire) {
    const size_t n = wire.size();
    REQUIRE(n >= 4);
    REQUIRE(wire[n - 1] == omgp::TRUNK_flag_byte);
    return wire[n - 3] == omgp::TRUNK_escape_byte
               ? static_cast<uint8_t>(wire[n - 2] ^ omgp::TRUNK_escape_xor)
               : wire[n - 2];
}

TEST_CASE("Kind::CrcError at each stuffing-boundary CRC high byte (FLAG, ESCAPE, and the two "
          "whose XOR 0xFF would land on them) still answers CRC-invalid at the real "
          "response's wire length",
          "[link][mock_wire]") {
    // contracts/mock-wire.md's amended CrcError row (docs/OPEN-QUESTIONS.md 2026-09-05):
    // XOR 0xFF except where that crosses the FLAG/ESCAPE stuffing boundary, where a
    // different wrong byte of the same stuffed length is chosen. The generic case above
    // never produces those four values, so the boundary branches went unexercised (PR #137
    // red-team @1057568, LOW: neutering the FLAG/ESCAPE branch left the suite green). Each
    // of the four is driven here through a request whose mirrored answer carries it —
    // found by search, and REQUIREd to exist so the case cannot pass vacuously. A literal
    // `crc_hi ^ 0xFF` fails the length assertion at all four (T028's note); a branch that
    // returns the real byte unchanged fails REQUIRE_FALSE(delivered).
    const uint8_t boundary[4] = {omgp::TRUNK_flag_byte, omgp::TRUNK_escape_byte,
                                 static_cast<uint8_t>(omgp::TRUNK_flag_byte ^ 0xFF),
                                 static_cast<uint8_t>(omgp::TRUNK_escape_byte ^ 0xFF)};
    for (uint8_t want : boundary) {
        CAPTURE(int(want));
        bool found = false;
        std::vector<uint8_t> req;
        for (unsigned seq = 0; seq < 16 && !found; ++seq)
            for (unsigned pb = 0; pb < 256 && !found; ++pb) {
                req = encode_request(0x01, static_cast<uint8_t>(seq), static_cast<uint8_t>(pb));
                found = crc_hi_on_wire(expected_respond_answer(req)) == want;
            }
        REQUIRE(found);
        const std::vector<uint8_t> real_answer = expected_respond_answer(req);
        REQUIRE(crc_hi_on_wire(real_answer) == want);

        FakeClock clock;
        const Step crc_step[] = {{0x01, Kind::CrcError, 30}};
        MockWire wire(clock);
        wire.set_script(0x01, crc_step, 1);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
        wire.advance_to(tx_end + 30 +
                        static_cast<uint64_t>(real_answer.size()) *
                            byte_time_us(omgp::TRUNK_bit_rate));

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
        REQUIRE_FALSE(delivered);
        REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadCrc)] == 1);
        REQUIRE(drained == real_answer.size());
        REQUIRE(wire.take_fault() == nullptr);
    }
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

// --- Undelivered injected RX bytes (#148) --------------------------------------------------
// The direct test of MockWire::inject_bytes() tasks.md records as outstanding under T028, and
// the proof of the accounting ~MockWire() checks. A cell that injects a fault — a duplicate, a
// babble burst, a corrupted response — and then lets the wire die before the engine consumes
// it asserts nothing about that fault while passing (the SC-004 duplicate cells found by the
// review of #145). contracts/mock-wire.md "Capacity" already forbids a SILENT dropped byte;
// these two cases extend that from dropped to undelivered.

TEST_CASE("MockWire counts the injected RX bytes receive() has not released, and reaches zero "
          "once they are drained",
          "[link][mock_wire]") {
    FakeClock clock;
    MockWire wire(clock);
    const uint64_t bt = byte_time_us(omgp::TRUNK_bit_rate);

    // A wire nothing was injected into owes nothing.
    size_t pending = 1;
    HEAP_FREE_SCOPE({ pending = wire.pending_injected(); });
    REQUIRE(pending == 0);

    // Scheduled bytes (Respond/CrcError/Duplicate) are deliberately NOT tracked — the guard is
    // scoped to raw injection (#148): a late scripted response legitimately outlives many
    // cases. Driven on its own MockWire, inside this scope, so that its destructor runs here
    // with a whole undrained response still queued and stays silent about it.
    {
        FakeClock sched_clock;
        MockWire scheduled(sched_clock);
        const std::vector<uint8_t> req = encode_request(0x01, 0);
        uint64_t tx_end = 0;
        HEAP_FREE_SCOPE({ tx_end = scheduled.transmit(req.data(), req.size(), 0); });
        REQUIRE(tx_end == req.size() * bt); // the Respond answer really was scheduled, undrained
        REQUIRE(scheduled.pending_injected() == 0);
    }

    // n injected bytes, one byte_time_us() apart (mock_wire.hpp): all outstanding at once.
    const uint8_t noise[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    constexpr size_t n = sizeof noise;
    const uint64_t start = 1000;
    HEAP_FREE_SCOPE({ wire.inject_bytes(noise, n, start); });
    REQUIRE(wire.pending_injected() == n);

    // Advance only far enough for the first k to reach their start instants, and drain those.
    constexpr size_t k = 4;
    static_assert(k < n, "the case must leave some injected bytes undelivered at this point");
    wire.advance_to(start + (k - 1) * bt);
    uint8_t byte = 0;
    uint64_t start_us = 0;
    size_t drained = 0;
    bool got = false;
    for (;;) {
        HEAP_FREE_SCOPE({ got = wire.receive(byte, start_us); });
        if (!got)
            break;
        ++drained;
    }
    REQUIRE(drained == k); // otherwise the residue below is not the one this case claims
    REQUIRE(wire.pending_injected() == n - k);

    // Past the last byte's start instant the rest is released, and the accounting empties.
    wire.advance_to(start + (n - 1) * bt);
    for (;;) {
        HEAP_FREE_SCOPE({ got = wire.receive(byte, start_us); });
        if (!got)
            break;
        ++drained;
    }
    REQUIRE(drained == n);
    REQUIRE(wire.pending_injected() == 0);
    // Nothing left for ~MockWire() to complain about: this case ends silent by consuming the
    // bytes it planted, the first of the two routes open to a case (#148).
}

TEST_CASE("MockWire::take_pending_injected() acknowledges undelivered injected bytes and clears "
          "the accounting the destructor checks",
          "[link][mock_wire]") {
    FakeClock clock;
    MockWire wire(clock);
    const uint8_t burst[3] = {0xDE, 0xAD, 0xBE};
    constexpr size_t n = sizeof burst;
    const uint64_t start = 500;
    wire.inject_bytes(burst, n, start);

    // The clock is never advanced: this case deliberately leaves the whole burst undelivered —
    // exactly the residue ~MockWire() fails on — and acknowledges it explicitly instead, the
    // second of the two routes open to a case (#148).
    REQUIRE(wire.pending_injected() == n);
    size_t acked = 0;
    HEAP_FREE_SCOPE({ acked = wire.take_pending_injected(); });
    REQUIRE(acked == n);
    // take_fault()'s idiom: the count is consumed, not merely observed, so a second reader
    // (including the destructor) sees nothing outstanding.
    REQUIRE(wire.take_pending_injected() == 0);
    REQUIRE(wire.pending_injected() == 0);

    // Acknowledging the residue clears the ACCOUNTING, not the wire: the bytes are still
    // queued and still released on their own start instants, and the count stays at 0 rather
    // than wrapping below it (size_t: an unguarded decrement would read as a huge number here).
    wire.advance_to(start + (n - 1) * byte_time_us(omgp::TRUNK_bit_rate));
    uint8_t byte = 0;
    uint64_t start_us = 0;
    size_t drained = 0;
    bool got = false;
    for (;;) {
        HEAP_FREE_SCOPE({ got = wire.receive(byte, start_us); });
        if (!got)
            break;
        ++drained;
    }
    REQUIRE(drained == n);
    REQUIRE(wire.pending_injected() == 0);
}

TEST_CASE("encode_crc_corrupted refuses a payload longer than the protocol allows", "[mock]") {
    // Red team @ef1ec22 [LOW]: the guard added with this function's export was pinned by
    // nothing, so deleting it would pass the suite -- and what it prevents is a stack
    // overflow, not a wrong answer. `unstuffed[kMaxUnstuffed]` is 70 bytes; f.len = 200 with
    // a roomy `cap` clears the capacity check (2 + 2*(4 + 200 + 2) = 414) and then writes 206.
    static_assert(kMaxUnstuffed < 200, "the case below must exceed the internal buffer");
    const std::vector<uint8_t> payload(200, 0xAB);
    const omgp::link::FrameFields f{0x01,
                                    omgp::ADDR_host,
                                    /*response=*/true,
                                    /*retry=*/false,
                                    /*seq=*/3,
                                    static_cast<uint8_t>(payload.size()),
                                    payload.data()};
    uint8_t out[512];
    REQUIRE(omgp_test::encode_crc_corrupted(f, out, sizeof out) == 0);

    // ...and still encodes the longest payload the protocol DOES allow, so the guard is a
    // bound and not a blanket refusal.
    const std::vector<uint8_t> ok(omgp::LIMIT_max_l3_payload, 0xCD);
    const omgp::link::FrameFields g{0x01,
                                    omgp::ADDR_host,
                                    /*response=*/true,
                                    /*retry=*/false,
                                    /*seq=*/3,
                                    static_cast<uint8_t>(ok.size()),
                                    ok.data()};
    REQUIRE(omgp_test::encode_crc_corrupted(g, out, sizeof out) > 0);
}

// --- Kind::Respond answered by the node's RequestHandler (#147) ----------------------------
// contracts/mock-wire.md:16 has always said the node's `RequestHandler` answers `Kind::
// Respond`; `schedule_respond()` fabricated an echo instead, an interim default ratified on
// 2026-09-03 only because `omgp::link::RequestHandler` did not exist at T010. These cases pin
// the handler seat MockWire now offers: a registered handler's answer is what reaches the
// wire (for Respond, CrcError and Duplicate alike), the echo survives only where no handler
// is registered, and the handler is never invoked for traffic it does not own.

namespace {

// A test-local RequestHandler: answers with a fixed payload the test chooses, counts its
// invocations, and records what the mock passed it. Allocation-free (fixed arrays, no
// std::vector) so the handler-answer path can be driven inside HEAP_FREE_SCOPE.
//
// Records rather than REQUIREs: handle() runs on MockWire::transmit()'s call stack, which in
// the engine tests is the engine's own — and link/CMakeLists.txt builds omgp_link with
// -fno-exceptions, so a REQUIRE thrown from this frame would unwind through it (the same
// hazard mock_wire.hpp's fault_ deferral exists to avoid; test_link_loop.cpp's CountingHandler
// takes the same care). Every check below runs on the test's own stack afterwards.
struct ScriptedHandler : RequestHandler {
    uint8_t answer[omgp::LIMIT_max_l3_payload] = {};
    size_t answer_len = 0;
    // Return a value that is NOT what was written — the only way to drive the overlong-answer
    // refusal without writing past `cap` (which would be this handler's own bug, not
    // MockWire's).
    bool force_return = false;
    size_t forced_return = 0;

    unsigned invocations = 0;
    uint8_t seen_request[omgp::LIMIT_max_l3_payload] = {};
    size_t seen_len = 0;
    size_t seen_cap = 0;
    bool seen_null_request = false;

    size_t handle(const uint8_t* req, size_t len, uint8_t* resp, size_t cap) override {
        ++invocations;
        seen_null_request = (req == nullptr);
        seen_len = len;
        seen_cap = cap;
        for (size_t i = 0; i < len && i < sizeof seen_request; ++i)
            seen_request[i] = req[i];
        const size_t n = answer_len < cap ? answer_len : cap;
        for (size_t i = 0; i < n; ++i)
            resp[i] = answer[i];
        return force_return ? forced_return : n;
    }

    void set_answer(std::initializer_list<uint8_t> bytes) {
        answer_len = 0;
        for (uint8_t b : bytes)
            answer[answer_len++] = b;
    }
    const uint8_t* answer_bytes() const {
        return answer;
    }
    uint8_t answer_size() const {
        return static_cast<uint8_t>(answer_len);
    }
};

// Every byte receive() will release at the wire's current clock, in order, with the start
// instant of the first and last of them.
std::vector<uint8_t> drain_all(MockWire& wire, uint64_t& first_start, uint64_t& last_start) {
    std::vector<uint8_t> out;
    uint8_t byte = 0;
    uint64_t start_us = 0;
    while (wire.receive(byte, start_us)) {
        if (out.empty())
            first_start = start_us;
        last_start = start_us;
        out.push_back(byte);
    }
    return out;
}

std::vector<uint8_t> drain_all(MockWire& wire) {
    uint64_t first = 0, last = 0;
    return drain_all(wire, first, last);
}

// The request every handler case below answers: one payload byte, 0xAB. Every handler answer
// used here is chosen to be something the echo CANNOT produce (a different length AND
// different content), so an unchanged schedule_respond() fails these cases rather than
// passing on a coincidence.
constexpr uint8_t kRequestByte = 0xAB;

} // namespace

TEST_CASE("Kind::Respond answers from the node's registered RequestHandler, at "
          "request_end + delay_us",
          "[link][mock_wire]") {
    FakeClock clock;
    static const Step respond_step[] = {{0x01, Kind::Respond, 30}};
    MockWire wire(clock);
    ScriptedHandler handler;
    // Four bytes, none of them the request's single 0xAB: the echo answer is one byte long
    // and carries 0xAB, so no echo can produce this frame (the "green log proves nothing"
    // trap named in #147's evidence section).
    handler.set_answer({0x11, 0x22, 0x33, 0x44});
    HEAP_FREE_SCOPE({ wire.set_handler(0x01, handler); });
    wire.set_script(0x01, respond_step, 1);

    const std::vector<uint8_t> req = encode_request(0x01, 7, kRequestByte);
    uint64_t tx_end = 0;
    // The whole handler-answer path — invocation, framing, RX enqueue — allocates nothing
    // (contracts/mock-wire.md preamble: MockWire stays allocation-free so F4 can seed its
    // virtual wire from it).
    HEAP_FREE_SCOPE({ tx_end = wire.transmit(req.data(), req.size(), 0); });

    // contracts/mock-wire.md:16 + contracts/link-cpp.md "Responder engine": handle(req, len,
    // resp, cap) is called once, with the REQUEST's payload and the protocol's payload bound
    // as `cap`.
    REQUIRE(handler.invocations == 1);
    REQUIRE_FALSE(handler.seen_null_request);
    REQUIRE(handler.seen_len == 1);
    REQUIRE(handler.seen_request[0] == kRequestByte);
    REQUIRE(handler.seen_cap == omgp::LIMIT_max_l3_payload);

    const std::vector<uint8_t> expected =
        expected_answer_with(req, handler.answer_bytes(), handler.answer_size());
    const uint64_t bt = byte_time_us(omgp::TRUNK_bit_rate);
    wire.advance_to(tx_end + 30 + static_cast<uint64_t>(expected.size()) * bt);

    uint64_t first_start = 0, last_start = 0;
    const std::vector<uint8_t> drained = drain_all(wire, first_start, last_start);
    REQUIRE(drained == expected);
    // "first byte at request_end + delay_us" (the Respond row), unchanged by whose answer it is.
    REQUIRE(first_start == tx_end + 30);
    REQUIRE(last_start == tx_end + 30 + static_cast<uint64_t>(expected.size() - 1) * bt);

    // ...and the frame the engine under test would actually decode: mirrored header, the
    // handler's length and the handler's bytes.
    Deframer d;
    FrameView view{};
    bool delivered = false;
    for (uint8_t b : drained)
        if (d.feed(b, view))
            delivered = true;
    REQUIRE(delivered);
    REQUIRE(view.f.dst == omgp::ADDR_host);
    REQUIRE(view.f.src == 0x01);
    REQUIRE(view.f.response);
    REQUIRE_FALSE(view.f.retry);
    REQUIRE(view.f.seq == 7);
    REQUIRE(view.f.len == handler.answer_size());
    REQUIRE(std::equal(view.f.payload, view.f.payload + view.f.len, handler.answer_bytes()));
}

TEST_CASE("Kind::CrcError corrupts the registered RequestHandler's answer, invoking it once",
          "[link][mock_wire]") {
    FakeClock clock;
    static const Step crc_step[] = {{0x01, Kind::CrcError, 30}};
    MockWire wire(clock);
    ScriptedHandler handler;
    handler.set_answer({0x5A, 0x5B, 0x5C});
    wire.set_handler(0x01, handler);
    wire.set_script(0x01, crc_step, 1);

    const std::vector<uint8_t> req = encode_request(0x01, 3, kRequestByte);
    uint64_t tx_end = 0;
    HEAP_FREE_SCOPE({ tx_end = wire.transmit(req.data(), req.size(), 0); });
    REQUIRE(handler.invocations == 1);

    // contracts/mock-wire.md's CrcError row builds from "the real response" — which is now the
    // handler's, not the echo's. Same fields, same wire length, one wrong CRC high byte.
    const std::vector<uint8_t> clean =
        expected_answer_with(req, handler.answer_bytes(), handler.answer_size());
    const uint64_t bt = byte_time_us(omgp::TRUNK_bit_rate);
    wire.advance_to(tx_end + 30 + static_cast<uint64_t>(clean.size()) * bt);

    uint64_t first_start = 0, last_start = 0;
    const std::vector<uint8_t> drained = drain_all(wire, first_start, last_start);
    REQUIRE(drained.size() == clean.size());
    REQUIRE(drained != clean);
    REQUIRE(first_start == tx_end + 30);

    Deframer d;
    FrameView view{};
    bool delivered = false;
    for (uint8_t b : drained)
        if (d.feed(b, view))
            delivered = true;
    REQUIRE_FALSE(delivered);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadCrc)] == 1);

    // The payload really is the handler's: every byte except the corrupted CRC high one
    // matches the clean handler answer (the echo answer is a different LENGTH, so it cannot
    // satisfy the size check above either).
    size_t differing = 0;
    for (size_t i = 0; i < clean.size(); ++i)
        if (drained[i] != clean[i])
            ++differing;
    REQUIRE(differing == 1);
}

TEST_CASE("Kind::Duplicate repeats the registered RequestHandler's answer, invoking the "
          "handler once per REQUEST rather than once per emitted copy",
          "[link][mock_wire]") {
    FakeClock clock;
    static const Step dup_step[] = {{0x01, Kind::Duplicate, 40}};
    MockWire wire(clock);
    ScriptedHandler handler;
    handler.set_answer({0xC0, 0xC1, 0xC2, 0xC3, 0xC4});
    wire.set_handler(0x01, handler);
    wire.set_script(0x01, dup_step, 1);

    const std::vector<uint8_t> req = encode_request(0x01, 5, kRequestByte);
    uint64_t tx_end = 0;
    HEAP_FREE_SCOPE({ tx_end = wire.transmit(req.data(), req.size(), 0); });

    // Two copies reach the wire; the handler was consulted ONCE. A per-copy invocation would
    // make this 2 — and would give a stateful handler (a real Responder's replay buffer, the
    // eventual consumer of this seat) a second, spurious transaction to account for.
    REQUIRE(handler.invocations == 1);

    const std::vector<uint8_t> answer =
        expected_answer_with(req, handler.answer_bytes(), handler.answer_size());
    const uint64_t bt = byte_time_us(omgp::TRUNK_bit_rate);
    const uint64_t first_end =
        tx_end + omgp::TRUNK_T_turn_min_us + static_cast<uint64_t>(answer.size()) * bt;
    wire.advance_to(first_end + 40 + static_cast<uint64_t>(answer.size()) * bt);

    std::vector<uint8_t> drained;
    uint64_t second_copy_start = 0;
    uint8_t byte = 0;
    uint64_t start_us = 0;
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
    REQUIRE(second_copy_start == first_end + 40);
}

TEST_CASE("With no handler registered for the addressed node, Respond, CrcError and Duplicate "
          "still emit the ratified interim echo, byte for byte",
          "[link][mock_wire]") {
    // The fallback half of #147: registering a handler for one node must not change what any
    // OTHER node answers, and a rig that registers none keeps exactly today's bytes — which is
    // what lets test_link_master.cpp's timing assertions (derived from the echo answer's wire
    // length) stay untouched. A handler IS registered here, on node 0x02, so the case also
    // pins that the lookup is per-node and not "any handler answers for anyone".
    FakeClock clock;
    static const Step steps[] = {{0x01, Kind::Respond, 30}};
    MockWire wire(clock);
    ScriptedHandler other_node;
    other_node.set_answer({0xEE, 0xEF});
    wire.set_handler(0x02, other_node);

    const uint64_t bt = byte_time_us(omgp::TRUNK_bit_rate);

    SECTION("Respond") {
        wire.set_script(0x01, steps, 1);
        const std::vector<uint8_t> req = encode_request(0x01, 1, kRequestByte);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
        const std::vector<uint8_t> echo = expected_respond_answer(req);
        wire.advance_to(tx_end + 30 + static_cast<uint64_t>(echo.size()) * bt);
        REQUIRE(drain_all(wire) == echo);
        REQUIRE(other_node.invocations == 0);
    }

    SECTION("CrcError") {
        static const Step crc_step[] = {{0x01, Kind::CrcError, 30}};
        wire.set_script(0x01, crc_step, 1);
        const std::vector<uint8_t> req = encode_request(0x01, 1, kRequestByte);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
        const std::vector<uint8_t> echo = expected_respond_answer(req);
        wire.advance_to(tx_end + 30 + static_cast<uint64_t>(echo.size()) * bt);
        const std::vector<uint8_t> drained = drain_all(wire);
        REQUIRE(drained.size() == echo.size());
        size_t differing = 0;
        for (size_t i = 0; i < echo.size(); ++i)
            if (drained[i] != echo[i])
                ++differing;
        REQUIRE(differing == 1); // the corrupted CRC high byte, and nothing else
        REQUIRE(other_node.invocations == 0);
    }

    SECTION("Duplicate") {
        static const Step dup_step[] = {{0x01, Kind::Duplicate, 40}};
        wire.set_script(0x01, dup_step, 1);
        const std::vector<uint8_t> req = encode_request(0x01, 1, kRequestByte);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
        const std::vector<uint8_t> echo = expected_respond_answer(req);
        const uint64_t first_end =
            tx_end + omgp::TRUNK_T_turn_min_us + static_cast<uint64_t>(echo.size()) * bt;
        wire.advance_to(first_end + 40 + static_cast<uint64_t>(echo.size()) * bt);
        const std::vector<uint8_t> drained = drain_all(wire);
        REQUIRE(drained.size() == 2 * echo.size());
        REQUIRE(std::equal(drained.begin(), drained.begin() + static_cast<long>(echo.size()),
                           echo.begin()));
        REQUIRE(std::equal(drained.begin() + static_cast<long>(echo.size()), drained.end(),
                           echo.begin()));
        REQUIRE(other_node.invocations == 0);
    }
}

TEST_CASE("A RequestHandler answer longer than LIMIT_max_l3_payload enqueues no frame and "
          "records a fault, rather than a truncated frame or a silent drop",
          "[link][mock_wire]") {
    FakeClock clock;
    MockWire wire(clock);
    ScriptedHandler handler;
    handler.set_answer({0x01, 0x02});
    // Claims more than it wrote: MockWire must judge the RETURNED length, since that is what it
    // would put in the frame's len field and copy out of the buffer.
    handler.force_return = true;
    handler.forced_return = omgp::LIMIT_max_l3_payload + 1;
    wire.set_handler(0x01, handler);

    const std::vector<uint8_t> req = encode_request(0x01, 2, kRequestByte);
    const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
    REQUIRE(handler.invocations == 1);

    // Named, not merely counted: a test driving MockWire into a specific fault asserts WHICH
    // one fired (take_fault()'s documented use), and taking it clears it for the advance_to()
    // and transcript_size() below, both of which REQUIRE fault_ == nullptr.
    const char* fault = wire.take_fault();
    REQUIRE(fault != nullptr);
    REQUIRE(std::string(fault).find("longer than") != std::string::npos);

    // Nothing on the wire at all — not a truncated frame, not a clamped one.
    wire.advance_to(tx_end + omgp::TRUNK_T_turn_min_us + 100 * byte_time_us(omgp::TRUNK_bit_rate));
    REQUIRE(drain_all(wire).empty());
    // The request itself was still transcribed: the refusal is of the ANSWER, not of the
    // request MockWire decoded.
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).dst == 0x01);
}

TEST_CASE("A RequestHandler answering zero bytes produces a valid zero-payload response frame, "
          "not silence",
          "[link][mock_wire]") {
    FakeClock clock;
    MockWire wire(clock);
    ScriptedHandler handler;
    handler.set_answer({}); // answer_len == 0
    wire.set_handler(0x01, handler);

    const std::vector<uint8_t> req = encode_request(0x01, 9, kRequestByte);
    uint64_t tx_end = 0;
    HEAP_FREE_SCOPE({ tx_end = wire.transmit(req.data(), req.size(), 0); });
    REQUIRE(handler.invocations == 1);

    const std::vector<uint8_t> expected = expected_answer_with(req, handler.answer_bytes(), 0);
    const uint64_t bt = byte_time_us(omgp::TRUNK_bit_rate);
    wire.advance_to(tx_end + omgp::TRUNK_T_turn_min_us +
                    static_cast<uint64_t>(expected.size()) * bt);

    uint64_t first_start = 0, last_start = 0;
    const std::vector<uint8_t> drained = drain_all(wire, first_start, last_start);
    // Silence stays reachable only through Kind::Silence: a zero-length ANSWER is a frame.
    REQUIRE_FALSE(drained.empty());
    REQUIRE(drained == expected);
    REQUIRE(first_start == tx_end + omgp::TRUNK_T_turn_min_us);

    Deframer d;
    FrameView view{};
    bool delivered = false;
    for (uint8_t b : drained)
        if (d.feed(b, view))
            delivered = true;
    REQUIRE(delivered);
    REQUIRE(view.f.response);
    REQUIRE(view.f.len == 0);
    REQUIRE(view.f.seq == 9);
}

TEST_CASE("A registered RequestHandler is not invoked for traffic it does not own",
          "[link][mock_wire]") {
    FakeClock clock;
    MockWire wire(clock);
    ScriptedHandler handler;
    handler.set_answer({0x77, 0x78, 0x79});
    // Registered on BOTH the host address and node 0x01, so the handler is reachable at every
    // address any section below actually names as a dst — except 0x10, which set_handler()
    // REQUIREs against (mock_wire.cpp: node < kAddrCount), and where the counter is therefore
    // NOT the evidence; see that section. Each section names the mechanism that holds its own
    // counter at 0: they are three different ones, not one guard in transmit().
    wire.set_handler(omgp::ADDR_host, handler);
    wire.set_handler(0x01, handler);

    SECTION("a RESPONSE frame transmitted by the engine under test") {
        // What a real Responder puts on the wire: dst == ADDR_host, response bit set. MockWire
        // must not treat it as a request to answer (mock_wire.cpp's `if (view.f.response)
        // continue;`), so handlers_[ADDR_host] stays untouched. Held by that early return:
        // a handler IS registered at ADDR_host, so delete the `continue` and this counter
        // reads 1 — the 0 is evidence for this guard specifically.
        const uint8_t payload[2] = {0x01, 0x02};
        FrameFields resp{omgp::ADDR_host, 0x01, true, false, 4, 2, payload};
        uint8_t out[kMaxWire];
        size_t written = 0;
        REQUIRE(encode_frame(resp, out, sizeof out, written) == Status::Ok);
        wire.transmit(out, written, 0);
        REQUIRE(handler.invocations == 0);
    }

    SECTION("a request addressed to a different node") {
        // transmit() does NOT return early here: 0x02 is a valid node address, so the request
        // reaches schedule_respond() and build_response(), which finds handlers_[0x02] ==
        // nullptr and emits the no-handler echo. What holds this counter at 0 is the per-node
        // lookup (mock_wire.cpp: handlers_[request.dst]) — make that lookup answer with any
        // registered handler rather than this node's and the counter reads 1.
        const std::vector<uint8_t> req = encode_request(0x02, 1, kRequestByte);
        wire.transmit(req.data(), req.size(), 0);
        REQUIRE(handler.invocations == 0);
    }

    SECTION("a request addressed outside trunk §5's node range") {
        // docs/trunk-link-layer.md §5: only 0x00..0x0F are node addresses. encode_frame refuses
        // only 0xFF, so 0x10 is encodable — and MockWire answers it with silence plus a fault,
        // never by reaching past the end of handlers_[kAddrCount].
        //
        // The counter is NOT the evidence here, and is kept only as the bound on
        // handlers_[0x10] never being read: set_handler() REQUIREs node < kAddrCount, so no
        // handler is or can be registered at 0x10, and the counter reads 0 with transmit()'s
        // dst >= kAddrCount guard deleted exactly as it does with it. What that guard holds is
        // asserted below instead — delete it and the request resolves to the default Respond,
        // which records no fault and enqueues a no-handler echo, so the fault REQUIRE fails
        // (first, ending the section) and the drained-empty one would too.
        const std::vector<uint8_t> req = encode_request(0x10, 1, kRequestByte);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
        REQUIRE(handler.invocations == 0);
        // Taken before advance_to(), which REQUIREs fault_ == nullptr (mock_wire.hpp).
        const char* fault = wire.take_fault();
        REQUIRE(fault != nullptr);
        REQUIRE(std::string(fault).find("kAddrCount") != std::string::npos);
        wire.advance_to(tx_end + omgp::TRUNK_T_turn_min_us +
                        100 * byte_time_us(omgp::TRUNK_bit_rate));
        REQUIRE(drain_all(wire).empty());
    }

    SECTION("a Kind::Silence step") {
        // Held by the Silence arm of transmit()'s switch calling no schedule_*() at all: a
        // handler IS registered at 0x01, so route this Kind to schedule_respond() and the
        // counter reads 1 — the 0 is evidence for that arm specifically.
        static const Step silence_step[] = {{0x01, Kind::Silence, 0}};
        wire.set_script(0x01, silence_step, 1);
        const std::vector<uint8_t> req = encode_request(0x01, 1, kRequestByte);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
        REQUIRE(handler.invocations == 0);
        wire.advance_to(tx_end + 10000);
        REQUIRE(drain_all(wire).empty());
    }
}
