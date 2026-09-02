// Test-support: MockWire skeleton (spec 002 T010) — Kind::Respond/Silence scheduling
// and the RX queue/transcript/PRNG machinery every later link/ engine test builds on.
// contracts/mock-wire.md. Garbage/CrcError/Duplicate/Babble/Rate land in T030.
#include "mock_wire.hpp"

#include "catch_amalgamated.hpp"
#include "link/frame.hpp"

#include <cstring>

namespace omgp_test {

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

void MockWire::schedule_respond(const omgp::link::FrameFields& request, uint64_t tx_end,
                                uint32_t delay_us) {
    using omgp::link::encode_frame;
    using omgp::link::FrameFields;
    using omgp::link::kMaxWire;
    using omgp::link::Status;

    // Default node behaviour: echo the request payload back (contracts/mock-wire.md
    // "Respond": "the node's RequestHandler ... answers" — a real per-node handler is
    // wired in by a later task; this skeleton's own answer is a valid, deterministic
    // response so Respond is directly usable — retry/timeout scripts drive Silence
    // instead where a real answer must not appear).
    FrameFields response{};
    response.dst = request.src;
    response.src = request.dst;
    response.response = true;
    response.retry = false;
    response.seq = request.seq;
    response.len = request.len;
    response.payload = request.payload;
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

    const uint64_t t0 = tx_end + delay_us;
    for (size_t i = 0; i < written; ++i)
        enqueue(buf[i], t0 + static_cast<uint64_t>(i) * omgp::link::byte_time_us(bit_rate_));
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
        case Kind::Garbage:
        case Kind::CrcError:
        case Kind::Duplicate:
        case Kind::Babble:
        case Kind::Rate:
            // T030 (tasks.md): not implemented by this skeleton. Recorded via fault_
            // (not REQUIRE'd here — this runs on the engine-under-test's call stack, see
            // fault_'s declaration) rather than silently behaving as Silence: a T028/T029
            // script that schedules one of these would otherwise go green while asserting
            // nothing about the behaviour it names.
            if (fault_ == nullptr)
                fault_ = "MockWire: Kind::Garbage/CrcError/Duplicate/Babble/Rate not "
                         "implemented until T030";
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
