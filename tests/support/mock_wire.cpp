// Test-support: MockWire skeleton (spec 002 T010) — Kind::Respond/Silence scheduling
// and the RX queue/transcript/PRNG machinery every later link/ engine test builds on.
// contracts/mock-wire.md. Kind::CrcError/Duplicate are implemented below (T029's own
// scope: test_link_master.cpp needs exactly these two, neither of which reads
// Step::count); Garbage/Babble/Rate — and the Step::count widening ruling on issue #48 —
// remain T030.
#include "mock_wire.hpp"

#include "catch_amalgamated.hpp"
#include "link/crc16.hpp"
#include "link/frame.hpp"

#include <cstring>

namespace omgp_test {

namespace {
// Kind::CrcError (contracts/mock-wire.md): "the real response with its last CRC byte
// XOR 0xFF". Duplicates encode_frame's unstuffed-build-then-stuff steps (link/frame.cpp)
// rather than post-processing its output, so corrupting the CRC can never accidentally
// land on a byte that needs (or stops needing) stuffing — which would silently change
// the wire length relative to a real response's and break every timing assertion built
// on that length (tests/unit/test_link_master.cpp computes expected instants from the
// UNCORRUPTED response's own encode_frame length). encode_frame's own refusals
// (PayloadTooLong/ReservedAddress) do not apply here: `request` is a frame this same
// MockWire just decoded, so `f.dst == request.src` is already a real trunk address.
size_t encode_crc_corrupted(const omgp::link::FrameFields& f, uint8_t* out, size_t cap) {
    using namespace omgp::link;
    // Same worst-case bound as encode_frame (link/frame.cpp), checked up front: this runs
    // on the engine-under-test's call stack (transmit() -> schedule_crc_error()), so a
    // capacity refusal must be a return value here, not a REQUIRE (see fault_'s
    // declaration in mock_wire.hpp for why this file never throws off that stack).
    const size_t needed = 2 + 2 * (kHeaderLen + static_cast<size_t>(f.len) + kCrcLen);
    if (cap < needed)
        return 0;
    uint8_t unstuffed[kMaxUnstuffed];
    size_t n = 0;
    unstuffed[n++] = f.dst;
    unstuffed[n++] = f.src;
    unstuffed[n++] = static_cast<uint8_t>((f.response ? 0x01 : 0x00) | (f.retry ? 0x02 : 0x00) |
                                          static_cast<uint8_t>((f.seq & 0x0F) << 4));
    unstuffed[n++] = f.len;
    for (uint8_t i = 0; i < f.len; ++i)
        unstuffed[n++] = f.payload[i];
    const uint16_t c = omgp::crc16_ccitt_false(unstuffed, n);
    unstuffed[n++] = static_cast<uint8_t>(c & 0xFF);
    unstuffed[n++] = static_cast<uint8_t>(((c >> 8) & 0xFF) ^ 0xFF); // the corruption

    size_t w = 0;
    out[w++] = omgp::TRUNK_flag_byte;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = unstuffed[i];
        if (b == omgp::TRUNK_flag_byte) {
            out[w++] = omgp::TRUNK_escape_byte;
            out[w++] = static_cast<uint8_t>(omgp::TRUNK_flag_byte ^ omgp::TRUNK_escape_xor);
        } else if (b == omgp::TRUNK_escape_byte) {
            out[w++] = omgp::TRUNK_escape_byte;
            out[w++] = static_cast<uint8_t>(omgp::TRUNK_escape_byte ^ omgp::TRUNK_escape_xor);
        } else {
            out[w++] = b;
        }
    }
    out[w++] = omgp::TRUNK_flag_byte;
    return w;
}

// The response FrameFields a conforming node (or MockWire's Respond/CrcError/Duplicate
// echo) sends back to `request` — dst/src swapped, response bit set, seq echoed. Shared
// by all three Kinds: they differ only in what bytes reach the wire and when, not in
// whom the response is addressed to or which payload it carries.
omgp::link::FrameFields response_fields(const omgp::link::FrameFields& request) {
    omgp::link::FrameFields r{};
    r.dst = request.src;
    r.src = request.dst;
    r.response = true;
    r.retry = false;
    r.seq = request.seq;
    r.len = request.len;
    r.payload = request.payload;
    return r;
}
} // namespace

uint32_t xorshift32_next(uint32_t& state) {
    // xorshift32 has a fixed point at state == 0 (every subsequent call returns 0). A
    // Step left with the default seed == 0 (every step authored so far) would otherwise
    // yield a constant all-zero stream once Garbage/Babble (T030) start consuming this —
    // deterministic, but degenerate, and seed != 0 is also given a separate meaning for
    // Kind::Rate (contracts/mock-wire.md). Re-seed to a fixed nonzero constant instead.
    // 0xFFFFFFFF, not a "known good" PRNG seed constant (unlike e.g. the golden ratio),
    // so a script author picking their own nonzero seed is unlikely to land on the exact
    // value this repair uses and get an accidental collision with the unset (0) case —
    // see mock_wire.hpp.
    if (state == 0)
        state = 0xFFFFFFFFu;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

MockWire::MockWire(FakeClock& clock) : clock_(clock) {}

MockWire::~MockWire() {
    // Non-throwing (CHECK, not REQUIRE): a destructor is the last chance to catch a fault
    // recorded by a transmit()/receive() that ran after the test's final advance_to() —
    // transcript_size()/transcript() only see faults raised before they're called, and
    // nothing else drains fault_ once the test stops calling advance_to(). Throwing here
    // would risk std::terminate if already unwinding, so this reports rather than aborts.
    INFO((fault_ != nullptr ? fault_ : ""));
    CHECK(fault_ == nullptr);
}

void MockWire::set_script(uint8_t node, const Step* steps, size_t count) {
    // Step::node documents which node a step was authored for; require it to agree with
    // the array's registered node so a mismatch (e.g. a Step{.node = 5, ...} registered
    // via set_script(3, ...)) fails loudly here instead of the step being silently
    // unreachable with no compiler or runtime diagnostic.
    for (size_t i = 0; i < count; ++i)
        REQUIRE(steps[i].node == node);
    if (node == 0xFF) {
        wildcard_script_ = steps;
        wildcard_len_ = count;
        for (size_t& pos : wildcard_pos_)
            pos = 0;
        return;
    }
    REQUIRE(node < omgp::link::kAddrCount);
    scripts_[node] = steps;
    script_len_[node] = count;
    script_pos_[node] = 0;
}

const Step* MockWire::next_step(uint8_t node) {
    if (node < omgp::link::kAddrCount && scripts_[node] != nullptr &&
        script_pos_[node] < script_len_[node]) {
        return &scripts_[node][script_pos_[node]++];
    }
    // Own script exhausted or never set: draw from the shared 0xFF fallback script next,
    // each node at its own cursor; only once that is also exhausted (or unset) does the
    // default Respond apply. contracts/mock-wire.md's "an exhausted script behaves as
    // Respond with the default delay" is read here as the combined own+wildcard sequence
    // for a node, not "own-script exhaustion skips straight past the wildcard".
    if (node < omgp::link::kAddrCount && wildcard_script_ != nullptr &&
        wildcard_pos_[node] < wildcard_len_) {
        return &wildcard_script_[wildcard_pos_[node]++];
    }
    return nullptr; // exhausted/unset: default Respond, TRUNK_T_turn_min_us delay
}

void MockWire::enqueue(uint8_t byte, uint64_t start_us) {
    // contracts/mock-wire.md "Capacity": never a SILENT drop — but this runs on the
    // engine-under-test's call stack (transmit() -> schedule_respond()), so the failure
    // is recorded and REQUIRE'd later, at the test's own advance_to() call, rather than
    // thrown here (see fault_'s declaration in mock_wire.hpp).
    if (rx_count_ >= kRxCapacity) {
        if (fault_ == nullptr)
            fault_ = "MockWire: RX queue capacity exceeded (4 * kMaxWire)";
        return;
    }
    // Insertion-sort by start_us (stable: only strictly-later entries shift right) so
    // receive() can always take index 0 in start-instant order, per byte-wire-and-clock.md,
    // even when a later transmit() schedules an earlier-firing response (e.g. a short
    // default delay for one node queued after a long "late response" delay for another).
    size_t pos = rx_count_;
    while (pos > 0 && rx_queue_[pos - 1].start_us > start_us) {
        rx_queue_[pos] = rx_queue_[pos - 1];
        --pos;
    }
    rx_queue_[pos] = QueuedByte{byte, start_us};
    ++rx_count_;
}

void MockWire::record_transcript(const omgp::link::FrameFields& f, uint64_t tx_start_us) {
    // Same deferred-fault reasoning as enqueue(): this runs on transmit()'s (engine) call
    // stack, so a capacity overrun is recorded, not thrown, here.
    if (transcript_count_ >= kTranscriptCapacity) {
        if (fault_ == nullptr)
            fault_ = "MockWire: transcript capacity exceeded (kTranscriptCapacity)";
        return;
    }
    TxRecord& rec = transcript_[transcript_count_++];
    rec.dst = f.dst;
    rec.src = f.src;
    rec.response = f.response;
    rec.retry = f.retry;
    rec.seq = f.seq;
    rec.len = f.len;
    if (f.len > 0)
        std::memcpy(rec.payload, f.payload, f.len);
    rec.tx_start_us = tx_start_us;
}

void MockWire::enqueue_frame(const uint8_t* buf, size_t written, uint64_t t0) {
    for (size_t i = 0; i < written; ++i)
        enqueue(buf[i], t0 + static_cast<uint64_t>(i) * omgp::link::byte_time_us(bit_rate_));
}

void MockWire::schedule_respond(const omgp::link::FrameFields& request, uint64_t tx_end,
                                uint32_t delay_us) {
    using omgp::link::encode_frame;
    using omgp::link::kMaxWire;
    using omgp::link::Status;

    // Default node behaviour: echo the request payload back (contracts/mock-wire.md
    // "Respond": "the node's RequestHandler ... answers" — a real per-node handler is
    // wired in by a later task; this skeleton's own answer is a valid, deterministic
    // response so Respond is directly usable — retry/timeout scripts drive Silence
    // instead where a real answer must not appear).
    const auto response = response_fields(request);
    uint8_t buf[kMaxWire];
    size_t written = 0;
    // response.dst == request.src. A well-formed request from a real Master/Responder
    // never carries src == 0xFF (trunk addresses are 0x00..0x0F, kAddrCount), so
    // encode_frame is not expected to refuse this response. This still runs on
    // transmit()'s (engine) call stack though, so a refusal is recorded via fault_
    // rather than REQUIRE'd here — see fault_'s declaration in mock_wire.hpp — turning a
    // script/engine bug that violates the assumption into a loud failure (at the next
    // advance_to()) instead of a silent, Silence-indistinguishable no-response
    // (contracts/mock-wire.md "Capacity": dropped bytes are never silent here either).
    if (encode_frame(response, buf, sizeof buf, written) != Status::Ok) {
        if (fault_ == nullptr)
            fault_ = "MockWire: encode_frame refused a Respond answer (response.dst == 0xFF?)";
        return;
    }
    enqueue_frame(buf, written, tx_end + delay_us);
}

void MockWire::schedule_crc_error(const omgp::link::FrameFields& request, uint64_t tx_end,
                                  uint32_t delay_us) {
    using omgp::link::kMaxWire;

    const auto response = response_fields(request);
    uint8_t buf[kMaxWire];
    const size_t written = encode_crc_corrupted(response, buf, sizeof buf);
    if (written == 0) {
        if (fault_ == nullptr)
            fault_ = "MockWire: encode_crc_corrupted refused a CrcError answer (buffer too small?)";
        return;
    }
    enqueue_frame(buf, written, tx_end + delay_us);
}

void MockWire::schedule_duplicate(const omgp::link::FrameFields& request, uint64_t tx_end,
                                  uint32_t delay_us) {
    using omgp::link::encode_frame;
    using omgp::link::kMaxWire;
    using omgp::link::Status;

    const auto response = response_fields(request);
    uint8_t buf[kMaxWire];
    size_t written = 0;
    if (encode_frame(response, buf, sizeof buf, written) != Status::Ok) {
        if (fault_ == nullptr)
            fault_ = "MockWire: encode_frame refused a Duplicate answer (response.dst == 0xFF?)";
        return;
    }
    // contracts/mock-wire.md: "the real response, then the same bytes again delay_us
    // after the first ends" — the first copy is the ordinary prompt answer (like Respond,
    // at the default turnaround: this row never restates "first byte at request_end +
    // delay_us" the way Respond/CrcError's rows do), and delay_us instead names the GAP
    // before the repeated copy — the one this Kind exists to plant as a late duplicate.
    const uint64_t first_t0 = tx_end + omgp::TRUNK_T_turn_min_us;
    enqueue_frame(buf, written, first_t0);
    const uint64_t first_end =
        first_t0 + static_cast<uint64_t>(written) * omgp::link::byte_time_us(bit_rate_);
    enqueue_frame(buf, written, first_end + delay_us);
}

uint64_t MockWire::transmit(const uint8_t* bytes, size_t n, uint64_t now_us) {
    using omgp::link::byte_time_us;
    using omgp::link::FrameView;

    const uint64_t tx_end = now_us + static_cast<uint64_t>(n) * byte_time_us(bit_rate_);

    FrameView view{};
    // Act on each decoded frame immediately, inside the feed loop: view.f.payload points
    // into parser_'s own accumulator and is valid only until the next feed() call
    // (link/frame.hpp), so deferring to after the loop would read a payload already
    // overwritten (or, for a call carrying several concatenated frames, would silently
    // drop every frame but the last — no transcript entry, no scheduled response).
    for (size_t i = 0; i < n; ++i) {
        const uint64_t byte_us = now_us + static_cast<uint64_t>(i) * byte_time_us(bit_rate_);
        const bool is_flag = (bytes[i] == omgp::TRUNK_flag_byte);
        // The instant of whichever FLAG most recently opened the accumulation now in
        // progress — persisted in open_flag_us_ across transmit() calls (mock_wire.hpp),
        // so a frame whose opening FLAG arrived in an earlier call still gets that call's
        // instant as its tx_start_us, not this call's.
        const uint64_t frame_tx_start = open_flag_us_;

        const bool delivered = parser_.feed(bytes[i], view);

        // on_flag() (link/frame.cpp) unconditionally resets accumulation on every FLAG
        // byte, whichever branch it takes, so this FLAG now opens the next frame either
        // way — update after feed() so frame_tx_start above still reads the *previous*
        // opening FLAG's instant for a frame delivered on this very byte.
        if (is_flag)
            open_flag_us_ = byte_us;

        if (!delivered)
            continue;

        const uint64_t frame_tx_end = byte_us + byte_time_us(bit_rate_);

        record_transcript(view.f, frame_tx_start);

        // contracts/mock-wire.md scopes every Kind to "the next request addressed to
        // node"; a response frame transmitted by the engine under test (Responder, from
        // T031) must not itself consume a script step or be echoed an answer.
        if (view.f.response)
            continue;

        // docs/trunk-link-layer.md §5: only 0x00..0x0F (kAddrCount) are node addresses.
        // encode_frame/Deframer reject only dst == 0xFF, so a request mis-addressed to
        // 0x10..0xFE would otherwise still resolve via next_step()'s default-Respond
        // fallback (next_step only checks node < kAddrCount to *look up* a script, not
        // whether the address is real) and get answered as if that node existed — hiding
        // exactly the mis-addressing bug the mock exists to expose. Silence is the
        // faithful wire behaviour (no such node to answer); also surfaced via fault_ so a
        // test can't pass by relying on it.
        if (view.f.dst >= omgp::link::kAddrCount) {
            if (fault_ == nullptr)
                fault_ = "MockWire: request addressed to dst >= kAddrCount (not a node)";
            continue;
        }

        const Step* step = next_step(view.f.dst);
        const Kind kind = step != nullptr ? step->kind : Kind::Respond;
        const uint32_t delay_us = step != nullptr ? step->delay_us : omgp::TRUNK_T_turn_min_us;

        switch (kind) {
        case Kind::Respond:
            schedule_respond(view.f, frame_tx_end, delay_us);
            break;
        case Kind::Silence:
            break;
        case Kind::CrcError:
            schedule_crc_error(view.f, frame_tx_end, delay_us);
            break;
        case Kind::Duplicate:
            schedule_duplicate(view.f, frame_tx_end, delay_us);
            break;
        case Kind::Garbage:
        case Kind::Babble:
        case Kind::Rate:
            // T030 (tasks.md): not implemented by this skeleton. Recorded via fault_
            // (not REQUIRE'd here — this runs on the engine-under-test's call stack, see
            // fault_'s declaration) rather than silently behaving as Silence: a T028/T029
            // script that schedules one of these would otherwise go green while asserting
            // nothing about the behaviour it names.
            if (fault_ == nullptr)
                fault_ = "MockWire: Kind::Garbage/Babble/Rate not implemented until T030";
            break;
        }
    }

    return tx_end;
}

bool MockWire::receive(uint8_t& byte, uint64_t& start_us) {
    if (rx_count_ == 0)
        return false;
    // rx_queue_ is kept sorted by start_us (enqueue()), so index 0 is always the
    // earliest-start-instant pending byte, matching byte-wire-and-clock.md's release order.
    const QueuedByte& front = rx_queue_[0];
    if (front.start_us > clock_.now_us())
        return false; // "in the future": stays queued (byte-wire-and-clock.md)
    byte = front.byte;
    start_us = front.start_us;
    for (size_t i = 1; i < rx_count_; ++i)
        rx_queue_[i - 1] = rx_queue_[i];
    --rx_count_;
    return true;
}

uint32_t MockWire::bit_rate() const {
    return bit_rate_;
}

void MockWire::set_bit_rate(uint32_t bps) {
    bit_rate_ = bps;
}

const char* MockWire::take_fault() {
    const char* f = fault_;
    fault_ = nullptr;
    return f;
}

void MockWire::advance_to(uint64_t t) {
    // Drains a fault recorded by transmit()'s call stack (see fault_'s declaration in
    // mock_wire.hpp) — this call, unlike transmit(), always runs on the test's own stack,
    // so REQUIRE-ing here is safe.
    INFO((fault_ != nullptr ? fault_ : ""));
    REQUIRE(fault_ == nullptr);
    // byte-wire-and-clock.md: the clock is monotonic and never goes backwards; setting it
    // earlier than its current instant would also silently make already-due RX bytes
    // future again (receive() gates on start_us <= now_us()).
    REQUIRE(t >= clock_.now_us());
    clock_.set(t);
}

size_t MockWire::transcript_size() const {
    // Drains fault_ here too, not just in advance_to(): this and transcript() are the
    // conclusion-drawing surfaces a test actually asserts through, so a fault raised by
    // the last transmit() of a test (with no advance_to() afterward) must still fail the
    // test case here rather than passing silently (see fault_'s declaration).
    INFO((fault_ != nullptr ? fault_ : ""));
    REQUIRE(fault_ == nullptr);
    return transcript_count_;
}

const MockWire::TxRecord& MockWire::transcript(size_t i) const {
    INFO((fault_ != nullptr ? fault_ : ""));
    REQUIRE(fault_ == nullptr);
    REQUIRE(i < transcript_count_);
    return transcript_[i];
}

} // namespace omgp_test
