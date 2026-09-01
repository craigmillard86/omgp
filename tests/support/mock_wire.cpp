// Test-support: MockWire skeleton (spec 002 T010) — Kind::Respond/Silence scheduling
// and the RX queue/transcript/PRNG machinery every later link/ engine test builds on.
// contracts/mock-wire.md. Garbage/CrcError/Duplicate/Babble/Rate land in T030.
#include "mock_wire.hpp"

#include "catch_amalgamated.hpp"
#include "link/frame.hpp"

#include <cstring>

namespace omgp_test {

uint32_t xorshift32_next(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

MockWire::MockWire(FakeClock& clock) : clock_(clock) {}

void MockWire::set_script(uint8_t node, const Step* steps, size_t count) {
    if (node == 0xFF) {
        wildcard_script_ = steps;
        wildcard_len_ = count;
        wildcard_pos_ = 0;
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
    if (wildcard_script_ != nullptr && wildcard_pos_ < wildcard_len_) {
        return &wildcard_script_[wildcard_pos_++];
    }
    return nullptr; // exhausted/unset: default Respond, TRUNK_T_turn_min_us delay
}

void MockWire::enqueue(uint8_t byte, uint64_t start_us) {
    REQUIRE(rx_count_ < kRxCapacity); // contracts/mock-wire.md "Capacity": never a silent drop
    rx_queue_[(rx_head_ + rx_count_) % kRxCapacity] = QueuedByte{byte, start_us};
    ++rx_count_;
}

void MockWire::record_transcript(const omgp::link::FrameFields& f, uint64_t tx_start_us) {
    REQUIRE(transcript_count_ < kTranscriptCapacity);
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
    // request.dst was accepted by the real Deframer, so it is never 0xFF (Reserved
    // Address is discarded, never delivered) and encode_frame cannot refuse this frame.
    if (encode_frame(response, buf, sizeof buf, written) != Status::Ok)
        return;

    const uint64_t t0 = tx_end + delay_us;
    for (size_t i = 0; i < written; ++i)
        enqueue(buf[i], t0 + static_cast<uint64_t>(i) * omgp::link::byte_time_us(bit_rate_));
}

uint64_t MockWire::transmit(const uint8_t* bytes, size_t n, uint64_t now_us) {
    using omgp::link::byte_time_us;
    using omgp::link::Deframer;
    using omgp::link::FrameView;

    const uint64_t tx_end = now_us + static_cast<uint64_t>(n) * byte_time_us(bit_rate_);

    Deframer parser;
    FrameView view{};
    bool decoded = false;
    for (size_t i = 0; i < n; ++i)
        if (parser.feed(bytes[i], view))
            decoded = true;

    if (decoded) {
        record_transcript(view.f, now_us);

        const Step* step = next_step(view.f.dst);
        const Kind kind = step != nullptr ? step->kind : Kind::Respond;
        const uint32_t delay_us = step != nullptr ? step->delay_us : omgp::TRUNK_T_turn_min_us;

        switch (kind) {
        case Kind::Respond:
            schedule_respond(view.f, tx_end, delay_us);
            break;
        case Kind::Silence:
            break;
        case Kind::Garbage:
        case Kind::CrcError:
        case Kind::Duplicate:
        case Kind::Babble:
        case Kind::Rate:
            break; // T030 (tasks.md): not implemented by this skeleton
        }
    }

    return tx_end;
}

bool MockWire::receive(uint8_t& byte, uint64_t& start_us) {
    if (rx_count_ == 0)
        return false;
    const QueuedByte& front = rx_queue_[rx_head_];
    if (front.start_us > clock_.now_us())
        return false; // "in the future": stays queued (byte-wire-and-clock.md)
    byte = front.byte;
    start_us = front.start_us;
    rx_head_ = (rx_head_ + 1) % kRxCapacity;
    --rx_count_;
    return true;
}

uint32_t MockWire::bit_rate() const {
    return bit_rate_;
}

void MockWire::set_bit_rate(uint32_t bps) {
    bit_rate_ = bps;
}

void MockWire::advance_to(uint64_t t) {
    clock_.set(t);
}

size_t MockWire::transcript_size() const {
    return transcript_count_;
}

const MockWire::TxRecord& MockWire::transcript(size_t i) const {
    REQUIRE(i < transcript_count_);
    return transcript_[i];
}

} // namespace omgp_test
