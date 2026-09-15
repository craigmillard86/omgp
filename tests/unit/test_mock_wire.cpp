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
#include <utility>
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

TEST_CASE("Step::count holds Kind::Rate's bit-rate values at full width",
          "[link][mock_wire][T030]") {
    // contracts/mock-wire.md: Kind::Rate is "the node now hears only at `count` interpreted as
    // bit rate (1 000 000 or 115 200)". Ruling 2026-09-03 (docs/OPEN-QUESTIONS.md 2026-09-02
    // "Step::count (uint16_t) cannot represent Kind::Rate's bit-rate values"; issue #48): widen
    // count to uint32_t atomically across the contract, data-model §10, research R-07 and
    // mock_wire.hpp. Red at uint16_t: the assignment narrows 115 200 to 49 664 and 1 000 000
    // to 16 960, so neither REQUIRE below can hold — the field cannot express either rate.
    Step step{.node = 0x01, .kind = Kind::Rate};
    step.count = static_cast<decltype(Step::count)>(omgp::TRUNK_bit_rate_fallback);
    REQUIRE(step.count == omgp::TRUNK_bit_rate_fallback);
    step.count = static_cast<decltype(Step::count)>(omgp::TRUNK_bit_rate);
    REQUIRE(step.count == omgp::TRUNK_bit_rate);
    // The widths agree by construction once ruled: the field is at least as wide as the
    // protocol's rate symbols, so no rate the YAML can name is unauthorable as a Rate step.
    REQUIRE(sizeof(Step::count) >= sizeof(omgp::TRUNK_bit_rate));
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

// --- contracts/mock-wire.md Step table: Kind::Garbage / Kind::Babble / Kind::Rate (T028) ---
// The three rows tasks.md leaves to this task; `CrcError`/`Duplicate` landed above with T029
// (PR #137). Everything here is asserted at the harness level only — which bytes reach the RX
// queue, when they start, and what the REAL Deframer makes of them. The engine-level
// consequences (timeout, SUSPECT/OFFLINE, BUS_FAULT, the SC-005 §7-mode mapping) belong to
// test_link_master.cpp / test_link_loop.cpp / test_link_busfault.cpp, not here.

namespace {

// How many frames a fresh REAL Deframer delivers from `bytes`. contracts/mock-wire.md's
// Garbage row ("never containing a valid frame — checked at generation") and trunk §4 are both
// statements about that parser, so these cases ask it rather than applying a byte-pattern rule
// of their own.
size_t frames_in(const std::vector<uint8_t>& bytes) {
    Deframer d;
    FrameView view{};
    size_t n = 0;
    for (uint8_t b : bytes)
        if (d.feed(b, view))
            ++n;
    return n;
}

// drain_all(), plus EVERY released byte's start instant: its two-out-param overload reports
// only the first and last, which cannot express an interleaved two-source stream.
std::vector<uint8_t> drain_with_starts(MockWire& wire, std::vector<uint64_t>& starts) {
    std::vector<uint8_t> out;
    uint8_t byte = 0;
    uint64_t start_us = 0;
    while (wire.receive(byte, start_us)) {
        out.push_back(byte);
        starts.push_back(start_us);
    }
    return out;
}

// contracts/mock-wire.md "Scheduling": byte i of a burst starting at t0 fires at
// t0 + i * byte_time_us(rate). Checked for every byte, not just the first and last — a burst at
// any other cadence than the wire's byte time would otherwise survive.
void require_byte_cadence(const std::vector<uint64_t>& starts, uint64_t t0, uint64_t bt) {
    for (size_t i = 0; i < starts.size(); ++i) {
        CAPTURE(i);
        REQUIRE(starts[i] == t0 + static_cast<uint64_t>(i) * bt);
    }
}

// A burst that is PRNG output rather than a constant run: "contains no valid frame" is
// satisfied by a stream of identical bytes too, so every case checking the former checks this.
void require_not_constant(const std::vector<uint8_t>& bytes) {
    REQUIRE_FALSE(bytes.empty());
    REQUIRE(std::adjacent_find(bytes.begin(), bytes.end(), std::not_equal_to<>()) != bytes.end());
}

} // namespace

TEST_CASE("Kind::Garbage emits count PRNG bytes at request_end + delay_us that contain no valid "
          "frame, and then nothing",
          "[link][mock_wire]") {
    // contracts/mock-wire.md's Garbage row: "`count` PRNG bytes (never containing a valid frame
    // — checked at generation) starting at `request_end + delay_us`, then nothing".
    constexpr uint32_t kCount = 64; // non-trivial: longer than a whole minimal frame's wire form
    constexpr uint32_t kDelay = 30;
    FakeClock clock;
    static const Step garbage_step[] = {{0x01, Kind::Garbage, kDelay, kCount, 0xC0FFEEu}};
    MockWire wire(clock);
    wire.set_script(0x01, garbage_step, 1);

    const std::vector<uint8_t> req = encode_request(0x01, 0);
    uint64_t tx_end = 0;
    // Allocation-free, like every other scheduling path (contracts/mock-wire.md preamble).
    HEAP_FREE_SCOPE({ tx_end = wire.transmit(req.data(), req.size(), 0); });

    const uint64_t bt = byte_time_us(omgp::TRUNK_bit_rate);
    wire.advance_to(tx_end + kDelay + static_cast<uint64_t>(kCount - 1) * bt);

    std::vector<uint64_t> starts;
    const std::vector<uint8_t> drained = drain_with_starts(wire, starts);
    REQUIRE(drained.size() == kCount);
    require_byte_cadence(starts, tx_end + kDelay, bt);
    REQUIRE(frames_in(drained) == 0);
    require_not_constant(drained);

    // "then nothing": no answer frame trails the burst, however far time is advanced — this is
    // what separates Garbage from "a corrupted response" (Kind::CrcError).
    wire.advance_to(tx_end + kDelay + 10000 * bt);
    REQUIRE(drain_all(wire).empty());
    // The request itself was still decoded and transcribed: Garbage governs the ANSWER only.
    // transcript_size() also REQUIREs the mock recorded no fault (mock_wire.cpp), so this case
    // cannot pass while the kind is merely deferring a "not implemented" fault.
    REQUIRE(wire.transcript_size() == 1);
    REQUIRE(wire.transcript(0).dst == 0x01);
}

TEST_CASE("Kind::Garbage and Kind::Babble bursts are byte-for-byte reproducible for a given "
          "Step::seed, and differ for a different one",
          "[link][mock_wire]") {
    // contracts/mock-wire.md "Scheduling": "All randomness from `Step::seed` through an
    // xorshift32 in the mock; the same script reproduces byte-for-byte." Asserted by running the
    // same script twice rather than by predicting the stream from xorshift32_next(): predicting
    // it would pin one extraction rule (the low byte of each output), which the contract does
    // not state, and would go red on an equally-conforming change.
    constexpr uint32_t kCount = 48;
    auto burst = [](Kind kind, uint32_t seed) {
        FakeClock clock;
        const Step steps[] = {{0x01, kind, 30, kCount, seed}};
        // Declared after `steps` so ~MockWire() runs while the script it points at is alive
        // (mock_wire.hpp: "`steps` must outlive this MockWire").
        MockWire wire(clock);
        wire.set_script(0x01, steps, 1);
        const std::vector<uint8_t> req = encode_request(0x01, 0);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
        wire.advance_to(tx_end + 30 +
                        static_cast<uint64_t>(kCount) * byte_time_us(omgp::TRUNK_bit_rate));
        return drain_all(wire);
    };

    for (Kind kind : {Kind::Garbage, Kind::Babble}) {
        CAPTURE(static_cast<int>(kind));
        const std::vector<uint8_t> a = burst(kind, 0xABCDEFu);
        const std::vector<uint8_t> b = burst(kind, 0xABCDEFu);
        const std::vector<uint8_t> other = burst(kind, 0x00123456u);
        REQUIRE(a.size() == kCount);
        REQUIRE(a == b);
        require_not_constant(a);
        // Not vacuous: a generator ignoring the seed entirely would also satisfy a == b.
        REQUIRE(a != other);
        REQUIRE(frames_in(a) == 0);
        REQUIRE(frames_in(other) == 0);
    }
}

TEST_CASE("Kind::Babble emits its burst on a request addressed to a DIFFERENT node — outside any "
          "response window of its own — and is consumed once, not latched",
          "[link][mock_wire]") {
    // contracts/mock-wire.md's Babble row: "`count` PRNG bytes at `request_end + delay_us`
    // **regardless of addressee** (also emitted when a different node is polled, i.e. outside
    // any window)". docs/trunk-link-layer.md §3 makes transmitting outside one's own response
    // window the violation this Kind exists to stage; a step that only fired when its own node
    // was polled could not stage it at all.
    constexpr uint32_t kCount = 24;
    constexpr uint32_t kDelay = 50;
    FakeClock clock;
    static const Step babbler[] = {{0x03, Kind::Babble, kDelay, kCount, 0x0B0BB1Eu}};
    // Two steps, so the second poll below is silent for a reason of its own rather than falling
    // through to the exhausted-script default Respond.
    static const Step polled[] = {{0x01, Kind::Silence, 0}, {0x01, Kind::Silence, 0}};
    MockWire wire(clock);
    wire.set_script(0x03, babbler, 1);
    wire.set_script(0x01, polled, 2);

    // The host polls node 0x01. Node 0x03 is never addressed, here or below.
    const std::vector<uint8_t> req = encode_request(0x01, 0);
    uint64_t tx_end = 0;
    HEAP_FREE_SCOPE({ tx_end = wire.transmit(req.data(), req.size(), 0); });

    const uint64_t bt = byte_time_us(omgp::TRUNK_bit_rate);
    wire.advance_to(tx_end + kDelay + static_cast<uint64_t>(kCount - 1) * bt);

    std::vector<uint64_t> starts;
    const std::vector<uint8_t> drained = drain_with_starts(wire, starts);
    // Node 0x01 said nothing (Kind::Silence), so every byte here is node 0x03's babble.
    REQUIRE(drained.size() == kCount);
    require_byte_cadence(starts, tx_end + kDelay, bt);
    REQUIRE(frames_in(drained) == 0);
    require_not_constant(drained);

    // Consumed, not latched: node 0x03's one-step script is now exhausted, so a second poll of
    // node 0x01 puts nothing at all on the wire.
    const std::vector<uint8_t> req2 = encode_request(0x01, 1);
    const uint64_t tx_end2 = wire.transmit(req2.data(), req2.size(), clock.now_us());
    wire.advance_to(tx_end2 + kDelay + 10000 * bt);
    REQUIRE(drain_all(wire).empty());
}

TEST_CASE("Kind::Babble fires for the addressed node too, and its burst coexists with another "
          "node's scheduled answer in start-instant order",
          "[link][mock_wire]") {
    const uint64_t bt = byte_time_us(omgp::TRUNK_bit_rate);

    SECTION("the babbling node is itself the addressee") {
        constexpr uint32_t kCount = 16;
        FakeClock clock;
        static const Step babbler[] = {{0x03, Kind::Babble, 45, kCount, 0x5EED01u}};
        MockWire wire(clock);
        wire.set_script(0x03, babbler, 1);

        const std::vector<uint8_t> req = encode_request(0x03, 2);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
        wire.advance_to(tx_end + 45 + static_cast<uint64_t>(kCount - 1) * bt);

        std::vector<uint64_t> starts;
        const std::vector<uint8_t> drained = drain_with_starts(wire, starts);
        REQUIRE(drained.size() == kCount);
        require_byte_cadence(starts, tx_end + 45, bt);
        // Never an answer as well: a Babble step replaces the node's response, it does not
        // precede one.
        REQUIRE(frames_in(drained) == 0);
    }

    SECTION("a third node babbles while the addressed node answers normally") {
        constexpr uint32_t kCount = 16;
        // Far enough after the answer that the two bursts do not overlap: this case is about
        // both reaching the wire in start-instant order, not about tie-breaking equal instants.
        constexpr uint32_t kBabbleDelay = 500;
        FakeClock clock;
        static const Step answerer[] = {{0x01, Kind::Respond, 30}};
        static const Step babbler[] = {{0x03, Kind::Babble, kBabbleDelay, kCount, 0x5EED02u}};
        MockWire wire(clock);
        wire.set_script(0x01, answerer, 1);
        wire.set_script(0x03, babbler, 1);

        const std::vector<uint8_t> req = encode_request(0x01, 4);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
        const std::vector<uint8_t> answer = expected_respond_answer(req);
        REQUIRE(30 + answer.size() * bt < kBabbleDelay); // the two bursts really are disjoint

        wire.advance_to(tx_end + kBabbleDelay + static_cast<uint64_t>(kCount - 1) * bt);
        std::vector<uint64_t> starts;
        const std::vector<uint8_t> drained = drain_with_starts(wire, starts);

        REQUIRE(drained.size() == answer.size() + kCount);
        const std::vector<uint8_t> got_answer(drained.begin(),
                                              drained.begin() + static_cast<long>(answer.size()));
        const std::vector<uint8_t> got_babble(drained.begin() + static_cast<long>(answer.size()),
                                              drained.end());
        REQUIRE(got_answer == answer); // node 0x01's own answer is untouched by the babble
        REQUIRE(frames_in(got_babble) == 0);
        REQUIRE(starts.front() == tx_end + 30);
        REQUIRE(starts[answer.size()] == tx_end + kBabbleDelay);
        for (size_t i = 1; i < starts.size(); ++i)
            REQUIRE(starts[i] > starts[i - 1]); // strictly ascending across BOTH sources
    }

    SECTION("two nodes babbling at once both reach the wire, interleaved rather than collided") {
        // Pins that the addressee-independent sweep covers every scripted node, not just the
        // first one it finds: with only one burst emitted the size below reads 20, not 40.
        //
        // Both bursts are scheduled at the SAME instants, one byte from each per instant. This
        // mock therefore models two simultaneous transmitters as a deterministic INTERLEAVE of
        // two INTACT bursts — NOT as the electrical collision docs/trunk-link-layer.md §3 makes
        // two nodes transmitting at once on a half-duplex pair, from which neither transmission
        // would survive. That model is asserted here rather than left implicit behind a byte
        // count (PR #610 red-team round 1, finding 3), so no later case can read a collision
        // into it; it is recorded as an open modelling question in docs/OPEN-QUESTIONS.md
        // 2026-09-15 "Two `Kind::Babble` bursts scheduled at one instant are interleaved, not
        // collided" (ruling PENDING).
        constexpr uint32_t kCount = 20;
        constexpr uint32_t kDelay = 60;

        // The bytes one babbler puts on the wire with nothing sharing its instants — the
        // comparison that makes "intact" mean something.
        auto burst_alone = [&](uint8_t node, uint32_t seed) {
            FakeClock clock;
            const Step babbler[] = {{node, Kind::Babble, kDelay, kCount, seed}};
            const Step silent[] = {{0x01, Kind::Silence, 0}};
            // Declared after the scripts so ~MockWire() runs while they are still alive.
            MockWire wire(clock);
            wire.set_script(node, babbler, 1);
            wire.set_script(0x01, silent, 1);
            const std::vector<uint8_t> req = encode_request(0x01, 6);
            const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
            wire.advance_to(tx_end + kDelay + static_cast<uint64_t>(kCount) * bt);
            return drain_all(wire);
        };
        const std::vector<uint8_t> alone_a = burst_alone(0x03, 0x5EED03u);
        const std::vector<uint8_t> alone_b = burst_alone(0x05, 0x5EED04u);
        REQUIRE(alone_a.size() == kCount);
        REQUIRE(alone_b.size() == kCount);
        REQUIRE(alone_a != alone_b); // distinct seeds: the two halves below are distinguishable

        struct Run {
            std::vector<uint8_t> bytes;
            std::vector<uint64_t> starts;
            uint64_t tx_end;
        };
        auto both_at_once = [&] {
            FakeClock clock;
            const Step babbler_a[] = {{0x03, Kind::Babble, kDelay, kCount, 0x5EED03u}};
            const Step babbler_b[] = {{0x05, Kind::Babble, kDelay, kCount, 0x5EED04u}};
            const Step polled[] = {{0x01, Kind::Silence, 0}};
            MockWire wire(clock);
            wire.set_script(0x03, babbler_a, 1);
            wire.set_script(0x05, babbler_b, 1);
            wire.set_script(0x01, polled, 1);

            const std::vector<uint8_t> req = encode_request(0x01, 6);
            Run run{};
            run.tx_end = wire.transmit(req.data(), req.size(), 0);
            wire.advance_to(run.tx_end + kDelay + static_cast<uint64_t>(kCount) * bt);
            run.bytes = drain_with_starts(wire, run.starts);
            return run;
        };

        const Run run = both_at_once();
        REQUIRE(run.bytes.size() == 2 * kCount);
        REQUIRE(frames_in(run.bytes) == 0);
        // Each instant of the burst carries exactly two bytes, in ascending pairs.
        for (size_t i = 0; i < run.starts.size(); ++i) {
            CAPTURE(i);
            REQUIRE(run.starts[i] == run.tx_end + kDelay + static_cast<uint64_t>(i / 2) * bt);
        }
        // Intact: de-interleaving the two slots at each instant recovers exactly what each
        // babbler puts on the wire alone. WHICH of the two takes the first slot is the RX
        // queue's insertion order for equal instants, which contracts/byte-wire-and-clock.md
        // does not state ("earliest start instant first" is silent on ties) — so it is not
        // pinned here, only that both bursts are wholly present.
        std::vector<uint8_t> first_slots, second_slots;
        for (size_t i = 0; i < run.bytes.size(); ++i)
            (i % 2 == 0 ? first_slots : second_slots).push_back(run.bytes[i]);
        REQUIRE(((first_slots == alone_a && second_slots == alone_b) ||
                 (first_slots == alone_b && second_slots == alone_a)));
        // ...and the tie order is deterministic, not whatever the queue happens to do: the same
        // script replays the same bytes at the same instants (the "Scheduling" section's rule,
        // which equal instants must not quietly escape).
        const Run again = both_at_once();
        REQUIRE(again.bytes == run.bytes);
        REQUIRE(again.starts == run.starts);
    }
}

TEST_CASE("Kind::Rate makes the node deaf to every other bit rate: silence when seed == 0, "
          "garbage when seed != 0, and a normal answer at the rate it hears",
          "[link][mock_wire][timing:bit_rate][timing:bit_rate_fallback]") {
    // contracts/mock-wire.md's Rate row: "the node now 'hears' only at `count` interpreted as
    // bit rate (1 000 000 or 115 200); requests at another rate behave as `Silence` (or
    // `Garbage` if `seed != 0`)" — research.md R-07 says the same ("a probe at another rate
    // yields silence (or garbage, when the step says so)"). This is the primitive trunk §7's
    // wrong-rate probe and its simultaneous-failure BUS_FAULT rule are eventually built on
    // (T042); nothing here asserts either — this case is the mock's own contract.
    const uint64_t ref_bt = byte_time_us(omgp::TRUNK_bit_rate);
    const uint64_t slow_bt = byte_time_us(omgp::TRUNK_bit_rate_fallback);

    SECTION("seed == 0: a request at the wrong rate is silence, and consumes no later step") {
        FakeClock clock;
        // Hears at the FALLBACK rate; the wire below starts at the reference rate.
        static const Step steps[] = {{0x01, Kind::Rate, 30, omgp::TRUNK_bit_rate_fallback, 0},
                                     {0x01, Kind::Respond, 30}};
        MockWire wire(clock);
        wire.set_script(0x01, steps, 2);
        REQUIRE(wire.bit_rate() == omgp::TRUNK_bit_rate);

        // The request carrying the Rate step is itself subject to it ("the node NOW hears only
        // at `count`"): unheard, so nothing is transmitted.
        const std::vector<uint8_t> req = encode_request(0x01, 0);
        uint64_t now = 0;
        HEAP_FREE_SCOPE({ now = wire.transmit(req.data(), req.size(), now); });
        wire.advance_to(now + 10000 * ref_bt);
        REQUIRE(drain_all(wire).empty());

        // Still deaf on the next request, the wire not having moved.
        const std::vector<uint8_t> req2 = encode_request(0x01, 1);
        now = wire.transmit(req2.data(), req2.size(), clock.now_us());
        wire.advance_to(now + 10000 * ref_bt);
        REQUIRE(drain_all(wire).empty());

        // Move the wire to the rate the node hears at: it answers again — and answers from the
        // `Respond` step with delay 30, NOT the exhausted-script default at TRUNK_T_turn_min_us.
        // That is what proves the two unheard requests consumed no script step: had they, this
        // answer would start at + TRUNK_T_turn_min_us instead.
        static_assert(omgp::TRUNK_T_turn_min_us != 30,
                      "the delay below must distinguish the scripted step from the default");
        wire.set_bit_rate(omgp::TRUNK_bit_rate_fallback);
        const std::vector<uint8_t> req3 = encode_request(0x01, 2);
        const uint64_t tx_end = wire.transmit(req3.data(), req3.size(), clock.now_us());
        const std::vector<uint8_t> answer = expected_respond_answer(req3);
        wire.advance_to(tx_end + 30 + static_cast<uint64_t>(answer.size()) * slow_bt);

        std::vector<uint64_t> starts;
        const std::vector<uint8_t> drained = drain_with_starts(wire, starts);
        REQUIRE(drained == answer);
        require_byte_cadence(starts, tx_end + 30, slow_bt);
    }

    SECTION("seed != 0: a request at the wrong rate is garbage, reproducibly") {
        auto wrong_rate_burst = [&] {
            FakeClock clock;
            const Step steps[] = {
                {0x01, Kind::Rate, 40, omgp::TRUNK_bit_rate_fallback, 0xDEADBEEFu}};
            MockWire wire(clock);
            wire.set_script(0x01, steps, 1);
            const std::vector<uint8_t> req = encode_request(0x01, 0);
            const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
            wire.advance_to(tx_end + 40 + 10000 * ref_bt);
            std::vector<uint64_t> starts;
            const std::vector<uint8_t> drained = drain_with_starts(wire, starts);
            REQUIRE_FALSE(starts.empty());
            // "at request_end + delay_us", at the wire's own byte cadence. The burst LENGTH is
            // deliberately not asserted: the Rate row spends `count` on the bit rate and `seed`
            // on selecting this branch, so it names no length, and pinning the mock's chosen one
            // here would be inventing contract rather than checking it.
            require_byte_cadence(starts, tx_end + 40, ref_bt);
            return drained;
        };

        const std::vector<uint8_t> a = wrong_rate_burst();
        const std::vector<uint8_t> b = wrong_rate_burst();
        REQUIRE_FALSE(a.empty());   // garbage, not the seed == 0 branch's silence
        REQUIRE(frames_in(a) == 0); // "no valid frame", the Garbage row's own property
        require_not_constant(a);
        REQUIRE(a == b); // deterministic for a fixed script (the Scheduling section's rule)
    }

    SECTION("a request at the rate the node hears is answered normally") {
        FakeClock clock;
        // count == the wire's own rate: the step changes what the node hears to what it already
        // hears, so nothing about the exchange changes.
        static const Step steps[] = {{0x01, Kind::Rate, 30, omgp::TRUNK_bit_rate, 0}};
        MockWire wire(clock);
        wire.set_script(0x01, steps, 1);

        const std::vector<uint8_t> req = encode_request(0x01, 3);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
        const std::vector<uint8_t> answer = expected_respond_answer(req);
        wire.advance_to(tx_end + 30 + static_cast<uint64_t>(answer.size()) * ref_bt);

        std::vector<uint64_t> starts;
        const std::vector<uint8_t> drained = drain_with_starts(wire, starts);
        REQUIRE(drained == answer);
        require_byte_cadence(starts, tx_end + 30, ref_bt);
    }

    SECTION("the deafness is per node, not wire-wide") {
        FakeClock clock;
        static const Step deaf[] = {{0x01, Kind::Rate, 30, omgp::TRUNK_bit_rate_fallback, 0}};
        MockWire wire(clock);
        wire.set_script(0x01, deaf, 1);

        const std::vector<uint8_t> to_deaf = encode_request(0x01, 0);
        const uint64_t now = wire.transmit(to_deaf.data(), to_deaf.size(), 0);
        wire.advance_to(now + 10000 * ref_bt);
        REQUIRE(drain_all(wire).empty());

        // Node 0x02 never saw a Rate step and still answers at the wire's rate.
        const std::vector<uint8_t> to_hearing = encode_request(0x02, 1);
        const uint64_t tx_end = wire.transmit(to_hearing.data(), to_hearing.size(), clock.now_us());
        const std::vector<uint8_t> answer = expected_respond_answer(to_hearing);
        wire.advance_to(tx_end + omgp::TRUNK_T_turn_min_us +
                        static_cast<uint64_t>(answer.size()) * ref_bt);
        REQUIRE(drain_all(wire) == answer);
    }
}

TEST_CASE("Kind::Rate's deafness gates Kind::Babble the SAME way whoever is addressed: a deaf "
          "node babbles for no request, and its Babble step survives unconsumed",
          "[link][mock_wire][timing:bit_rate][timing:bit_rate_fallback]") {
    // PR #610 red-team round 1, finding 1: fire_foreign_babble() consulted no node's hearing, so
    // a node deafened by a Kind::Rate step DID babble (and DID advance its script) on a request
    // it provably could not hear, while staying silent when that same unheard request was
    // addressed to itself — making Kind::Babble addressee-DEPENDENT for exactly the nodes
    // carrying both Kinds, which is the one property contracts/mock-wire.md's Babble row states.
    // Either answer is defensible; one of each is not. The two readings and the reason for this
    // one are in docs/OPEN-QUESTIONS.md 2026-09-15 "A node deafened by `Kind::Rate`: does its
    // pending `Kind::Babble` step still fire?" (ruling PENDING).
    constexpr uint32_t kCount = 24;
    constexpr uint32_t kDelay = 50;
    const uint64_t ref_bt = byte_time_us(omgp::TRUNK_bit_rate);
    const uint64_t slow_bt = byte_time_us(omgp::TRUNK_bit_rate_fallback);

    FakeClock clock;
    // seed == 0 on the Rate step, so its own wrong-rate branch is Silence: every byte drained
    // below is the Babble burst or nothing at all.
    static const Step deaf_babbler[] = {
        {0x03, Kind::Rate, 30, omgp::TRUNK_bit_rate_fallback, 0},
        {0x03, Kind::Babble, kDelay, kCount, 0x0B0BB1Eu},
    };
    // One Silence step per poll of node 0x01 below, so none of them falls through to the
    // exhausted-script default Respond and contributes bytes of its own.
    static const Step polled[] = {
        {0x01, Kind::Silence, 0}, {0x01, Kind::Silence, 0}, {0x01, Kind::Silence, 0}};
    MockWire wire(clock);
    wire.set_script(0x03, deaf_babbler, 2);
    wire.set_script(0x01, polled, 3);
    REQUIRE(wire.bit_rate() == omgp::TRUNK_bit_rate);

    // Arm the deafness: node 0x03 now hears only the fallback rate, the wire runs at the
    // reference rate, and node 0x03's Babble step is next at the head of its script.
    const std::vector<uint8_t> arm = encode_request(0x03, 0);
    uint64_t now = wire.transmit(arm.data(), arm.size(), 0);
    wire.advance_to(now + 10000 * ref_bt);
    REQUIRE(drain_all(wire).empty());

    // (a) an unheard request addressed to a DIFFERENT node: no burst.
    const std::vector<uint8_t> foreign = encode_request(0x01, 1);
    now = wire.transmit(foreign.data(), foreign.size(), clock.now_us());
    wire.advance_to(now + kDelay + 10000 * ref_bt);
    REQUIRE(drain_all(wire).empty());

    // (b) the same unheard request addressed to the babbler ITSELF: no burst either. (a) and
    // (b) together are the symmetry the Babble row requires.
    const std::vector<uint8_t> own = encode_request(0x03, 2);
    now = wire.transmit(own.data(), own.size(), clock.now_us());
    wire.advance_to(now + kDelay + 10000 * ref_bt);
    REQUIRE(drain_all(wire).empty());

    // Unheard, not consumed: move the wire to the rate node 0x03 hears and the Babble step is
    // still there — and still addressee-independent, firing on a poll of node 0x01.
    wire.set_bit_rate(omgp::TRUNK_bit_rate_fallback);
    const std::vector<uint8_t> foreign2 = encode_request(0x01, 3);
    const uint64_t tx_end = wire.transmit(foreign2.data(), foreign2.size(), clock.now_us());
    wire.advance_to(tx_end + kDelay + static_cast<uint64_t>(kCount - 1) * slow_bt);

    std::vector<uint64_t> starts;
    const std::vector<uint8_t> drained = drain_with_starts(wire, starts);
    REQUIRE(drained.size() == kCount);
    require_byte_cadence(starts, tx_end + kDelay, slow_bt);
    REQUIRE(frames_in(drained) == 0);
    require_not_constant(drained);

    // ...and consumed exactly once when it does fire, like any other Babble step.
    const std::vector<uint8_t> foreign3 = encode_request(0x01, 4);
    now = wire.transmit(foreign3.data(), foreign3.size(), clock.now_us());
    wire.advance_to(now + kDelay + 10000 * slow_bt);
    REQUIRE(drain_all(wire).empty());
}

TEST_CASE("Kind::Rate remembers the ARMING step's seed and delay per node, not in one shared slot",
          "[link][mock_wire][timing:bit_rate][timing:bit_rate_fallback]") {
    // PR #610 red-team round 1, finding 2: node_rate_seed_/node_rate_delay_ (mock_wire.hpp) are
    // per-node arrays, but no case read either for more than one node, so an implementation
    // keeping both in ONE shared slot passed the whole suite. They are read only when a LATER
    // wrong-rate request arrives — the arming step is long consumed by then (docs/
    // OPEN-QUESTIONS.md 2026-09-15 "`Kind::Rate`'s wrong-rate `Garbage` branch names no burst
    // LENGTH") — so arming two nodes with DIFFERENT seeds and delays and then RE-polling the
    // first is what discriminates: with a shared slot the third burst below takes node 0x02's
    // delay (a cadence failure) and node 0x02's seed (a byte failure).
    constexpr uint32_t kDelayA = 40;
    constexpr uint32_t kDelayB = 90;
    static_assert(kDelayA != kDelayB, "the two delays must be distinguishable");
    const uint64_t ref_bt = byte_time_us(omgp::TRUNK_bit_rate);

    FakeClock clock;
    static const Step arm_a[] = {
        {0x01, Kind::Rate, kDelayA, omgp::TRUNK_bit_rate_fallback, 0xAAAAAAAAu}};
    static const Step arm_b[] = {
        {0x02, Kind::Rate, kDelayB, omgp::TRUNK_bit_rate_fallback, 0xBBBBBBBBu}};
    MockWire wire(clock);
    wire.set_script(0x01, arm_a, 1);
    wire.set_script(0x02, arm_b, 1);

    // Polls `dst` at the wire's (unchanged, reference) rate, which neither node hears, and
    // returns the wrong-rate burst — having required it to start at `delay` past the request,
    // at the wire's byte cadence. The LENGTH is deliberately not asserted here either, for the
    // reason the Kind::Rate case above gives.
    auto wrong_rate_poll = [&](uint8_t dst, uint8_t seq, uint32_t delay) {
        const std::vector<uint8_t> req = encode_request(dst, seq);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), clock.now_us());
        wire.advance_to(tx_end + delay + 10000 * ref_bt);
        std::vector<uint64_t> starts;
        std::vector<uint8_t> burst = drain_with_starts(wire, starts);
        REQUIRE_FALSE(starts.empty());
        require_byte_cadence(starts, tx_end + delay, ref_bt);
        return burst;
    };

    const std::vector<uint8_t> burst_a = wrong_rate_poll(0x01, 0, kDelayA);
    const std::vector<uint8_t> burst_b = wrong_rate_poll(0x02, 1, kDelayB);
    require_not_constant(burst_a);
    REQUIRE(frames_in(burst_a) == 0);
    REQUIRE(frames_in(burst_b) == 0);
    REQUIRE(burst_a != burst_b); // different arming seeds, different bytes

    // Node 0x01 again, its arming step long consumed and node 0x02 armed since: its OWN delay
    // (the cadence check inside the lambda) and its OWN seed, not the most recently armed
    // node's.
    const std::vector<uint8_t> burst_a_again = wrong_rate_poll(0x01, 2, kDelayA);
    REQUIRE(burst_a_again == burst_a);
}

TEST_CASE("A Garbage, Babble or Rate step the mock cannot honour is refused by name, never "
          "quietly turned into silence",
          "[link][mock_wire]") {
    // contracts/mock-wire.md "Capacity" forbids a silent drop; the same reasoning covers a step
    // the mock cannot carry out at all. Each of these would otherwise put nothing on the wire —
    // indistinguishable from Kind::Silence — and every case scripted on it would pass while
    // asserting nothing about the kind it named. take_fault()'s documented use: assert WHICH
    // fault fired, with an ordinary REQUIRE.
    const uint64_t bt = byte_time_us(omgp::TRUNK_bit_rate);

    SECTION("Kind::Garbage with count == 0") {
        FakeClock clock;
        static const Step steps[] = {{0x01, Kind::Garbage, 30, 0, 0x1234u}};
        MockWire wire(clock);
        wire.set_script(0x01, steps, 1);
        const std::vector<uint8_t> req = encode_request(0x01, 0);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
        const char* fault = wire.take_fault();
        REQUIRE(fault != nullptr);
        REQUIRE(std::string(fault).find("count == 0") != std::string::npos);
        wire.advance_to(tx_end + 10000 * bt);
        REQUIRE(drain_all(wire).empty());
    }

    SECTION("Kind::Babble longer than the RX queue can hold") {
        FakeClock clock;
        // One byte past 4 * kMaxWire, the queue's fixed capacity (mock_wire.hpp): every byte
        // beyond it would be dropped, so the burst is refused whole instead.
        static const Step steps[] = {{0x03, Kind::Babble, 30, 4 * kMaxWire + 1, 0x1234u}};
        MockWire wire(clock);
        wire.set_script(0x03, steps, 1);
        const std::vector<uint8_t> req = encode_request(0x03, 0);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
        const char* fault = wire.take_fault();
        REQUIRE(fault != nullptr);
        REQUIRE(std::string(fault).find("RX queue capacity") != std::string::npos);
        wire.advance_to(tx_end + 10000 * bt);
        REQUIRE(drain_all(wire).empty());
    }

    SECTION("Kind::Rate with count == 0") {
        FakeClock clock;
        // 0 is the mock's "no Rate step seen" sentinel and is no bit rate a node could hear
        // at, so honouring it would silently DISARM the node rather than deafen it.
        static const Step steps[] = {{0x01, Kind::Rate, 30, 0, 0}};
        MockWire wire(clock);
        wire.set_script(0x01, steps, 1);
        const std::vector<uint8_t> req = encode_request(0x01, 0);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
        const char* fault = wire.take_fault();
        REQUIRE(fault != nullptr);
        REQUIRE(std::string(fault).find("count is a bit rate") != std::string::npos);
        wire.advance_to(tx_end + 10000 * bt);
        REQUIRE(drain_all(wire).empty());
    }

    SECTION("Kind::Babble in the 0xFF wildcard script") {
        FakeClock clock;
        // contracts/mock-wire.md:21 makes Babble fire "regardless of addressee" and :25 makes a
        // 0xFF step "apply to every node" — but the two together name no burst count for a
        // single request, and fire_foreign_babble() deliberately scans per-node scripts only
        // (mock_wire.cpp), so a wildcard Babble could otherwise only reach the ADDRESSED node
        // and would be Kind::Garbage wearing another Kind's name. Refused by name rather than
        // honoured as that narrower thing: docs/OPEN-QUESTIONS.md 2026-09-15 "A `Kind::Babble`
        // step in the 0xFF wildcard script".
        static const Step steps[] = {{0xFF, Kind::Babble, 50, 24, 0x0B0BB1Eu}};
        MockWire wire(clock);
        wire.set_script(0xFF, steps, 1);
        const std::vector<uint8_t> req = encode_request(0x01, 0);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
        const char* fault = wire.take_fault();
        REQUIRE(fault != nullptr);
        REQUIRE(std::string(fault).find("wildcard") != std::string::npos);
        wire.advance_to(tx_end + 10000 * bt);
        REQUIRE(drain_all(wire).empty());

        // Refused, not silently skipped past: the step was drawn (the wildcard cursor moved),
        // so the next poll of the same node falls through to the exhausted-script default
        // Respond rather than raising the same fault a second time.
        const std::vector<uint8_t> req2 = encode_request(0x01, 1);
        const uint64_t tx_end2 = wire.transmit(req2.data(), req2.size(), clock.now_us());
        REQUIRE(wire.take_fault() == nullptr);
        wire.advance_to(tx_end2 + omgp::TRUNK_T_turn_min_us + 10000 * bt);
        REQUIRE(frames_in(drain_all(wire)) == 1);
    }
}

// --- MockWire::inject_bytes(), directly (tasks.md T028: "plus a direct test of
// `MockWire::inject_bytes()`", PR #137 red-team @050f397, NOT EXAMINED) ----------------------
// The helper is exercised heavily through test_link_master.cpp and test_link_loop.cpp, but its
// own contract — raw bytes from ANOTHER station: same RX queue, same cadence, no transcript
// entry, no scheduled answer — was never asserted as a unit. The two #148 cases above cover the
// pending/acknowledge accounting only, not what reaches the wire.

TEST_CASE("MockWire::inject_bytes queues raw bytes at the requested instants without "
          "transcribing them or answering them",
          "[link][mock_wire]") {
    const uint64_t bt = byte_time_us(omgp::TRUNK_bit_rate);

    SECTION("even a perfectly valid request frame is wire content, not a request to answer") {
        FakeClock clock;
        MockWire wire(clock);
        // The very bytes a Master would transmit to node 0x01 — but arriving from the bus, not
        // handed to transmit(). mock_wire.hpp: "these bytes model another station's
        // transmission, not a request addressed to a node".
        const std::vector<uint8_t> frame = encode_request(0x01, 5);
        const uint64_t start = 1000;
        HEAP_FREE_SCOPE({ wire.inject_bytes(frame.data(), frame.size(), start); });

        // Not decoded into the transcript, and no script step consumed: the mock's parser_ never
        // sees these bytes. (transcript_size() also REQUIREs no fault was recorded.)
        REQUIRE(wire.transcript_size() == 0);

        wire.advance_to(start + static_cast<uint64_t>(frame.size() - 1) * bt);
        std::vector<uint64_t> starts;
        const std::vector<uint8_t> drained = drain_with_starts(wire, starts);
        REQUIRE(drained == frame); // byte for byte, in order
        require_byte_cadence(starts, start, bt);

        // No answer was scheduled for it, however far time runs on.
        wire.advance_to(start + 10000 * bt);
        REQUIRE(drain_all(wire).empty());
        REQUIRE(wire.pending_injected() == 0);
    }

    SECTION("an opened frame that stops mid-flight leaves the Deframer accumulating") {
        // inject_bytes()'s motivating case (mock_wire.hpp): schedule_respond/crc_error/duplicate
        // all emit COMPLETE frames, so none can leave a receiver mid-accumulation. A transmitter
        // that stalls mid-frame is a trunk §7 failure class, and only raw injection reaches it.
        FakeClock clock;
        MockWire wire(clock);
        const uint8_t truncated[4] = {omgp::TRUNK_flag_byte, 0x00, omgp::ADDR_host, 0x10};
        const uint64_t start = 200;
        wire.inject_bytes(truncated, sizeof truncated, start);
        wire.advance_to(start + 3 * bt);

        Deframer d;
        FrameView view{};
        uint8_t byte = 0;
        uint64_t start_us = 0;
        size_t delivered = 0, drained = 0;
        while (wire.receive(byte, start_us)) {
            ++drained;
            if (d.feed(byte, view))
                ++delivered;
        }
        REQUIRE(drained == sizeof truncated);
        REQUIRE(delivered == 0);
        REQUIRE(d.in_frame()); // still accumulating, which is the whole point of the case
        REQUIRE(wire.pending_injected() == 0);
    }

    SECTION("injected bytes interleave with a scheduled answer in start-instant order") {
        FakeClock clock;
        static const Step answerer[] = {{0x01, Kind::Respond, 30}};
        MockWire wire(clock);
        wire.set_script(0x01, answerer, 1);

        const std::vector<uint8_t> req = encode_request(0x01, 8);
        const uint64_t tx_end = wire.transmit(req.data(), req.size(), 0);
        const std::vector<uint8_t> answer = expected_respond_answer(req);
        REQUIRE(answer.size() >= 3);

        // Two bytes landing strictly BETWEEN the answer's own byte instants, so the merged order
        // is unambiguous (no equal start instants to tie-break).
        const uint8_t noise[2] = {0x5A, 0xA5};
        const uint64_t noise_start = tx_end + 30 + bt / 2;
        wire.inject_bytes(noise, sizeof noise, noise_start);

        wire.advance_to(tx_end + 30 + static_cast<uint64_t>(answer.size()) * bt);
        std::vector<uint64_t> starts;
        const std::vector<uint8_t> drained = drain_with_starts(wire, starts);

        std::vector<std::pair<uint64_t, uint8_t>> expected;
        for (size_t i = 0; i < answer.size(); ++i)
            expected.emplace_back(tx_end + 30 + static_cast<uint64_t>(i) * bt, answer[i]);
        for (size_t i = 0; i < sizeof noise; ++i)
            expected.emplace_back(noise_start + static_cast<uint64_t>(i) * bt, noise[i]);
        std::stable_sort(expected.begin(), expected.end(),
                         [](const std::pair<uint64_t, uint8_t>& a,
                            const std::pair<uint64_t, uint8_t>& b) { return a.first < b.first; });

        std::vector<std::pair<uint64_t, uint8_t>> got;
        for (size_t i = 0; i < drained.size(); ++i)
            got.emplace_back(starts[i], drained[i]);
        REQUIRE(got == expected);
        REQUIRE(wire.pending_injected() == 0);
    }
}
