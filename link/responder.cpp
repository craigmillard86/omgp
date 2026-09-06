// OMGP trunk L2 — Responder transaction engine implementation: trunk §3 (media access),
// §7 (retries, replay); data-model.md §5 "Responder". Makes
// tests/unit/test_link_responder.cpp (T033) pass.
#include "link/responder.hpp"

#include "omgp_protocol.h"

#include <cstring>

namespace omgp {
namespace link {

namespace {

uint32_t clamp_turnaround(uint32_t turnaround_us) {
    if (turnaround_us < omgp::TRUNK_T_turn_min_us)
        return omgp::TRUNK_T_turn_min_us;
    if (turnaround_us > omgp::TRUNK_T_turn_max_us)
        return omgp::TRUNK_T_turn_max_us;
    return turnaround_us;
}

// Sum of every Deframer discard category (data-model.md §3). The Responder's own
// stats() does not distinguish discard reasons (contracts/link-cpp.md: just
// "discards"), so a before/after delta of this total is enough to notice one byte-level
// discard, whatever caused it — mirrors Master::poll's own bad_crc_before/after delta.
uint32_t total_discards(const DeframerStats& s) {
    uint32_t total = 0;
    for (uint32_t d : s.discarded)
        total += d;
    return total;
}

} // namespace

Responder::Responder(ByteWire& wire, Clock& clock, RequestHandler& handler, uint8_t my_addr,
                     uint32_t turnaround_us)
    : wire_(wire), clock_(clock), handler_(handler), my_addr_(my_addr),
      turnaround_us_(clamp_turnaround(turnaround_us)) {}

const AddrStats& Responder::stats() const {
    return stats_;
}

void Responder::on_request(const FrameFields& f, uint64_t request_end_us) {
    if (f.dst != my_addr_ || f.response) {
        // Not a request addressed to this node (data-model.md §5 "Request
        // acceptance"): silently discarded, counted (trunk §4).
        stats_.discards++;
        return;
    }

    const bool is_replay = f.retry && buffer_.valid && f.seq == buffer_.seq;
    if (is_replay) {
        // data-model.md §5: "retry == 1 && valid && seq == buffer.seq -> retransmit
        // buffer (no handler call)" — the exact bytes already sent, never re-encoded.
        stats_.replays_served++;
    } else {
        // Any other intact request is new (data-model.md §5): a differing sequence, no
        // retry bit, or a retry bit with nothing yet buffered to replay (spec US3 AS5).
        uint8_t payload[omgp::LIMIT_max_l3_payload];
        const size_t resp_len = handler_.handle(f.payload, f.len, payload, sizeof payload);
        // data-model.md §5: "src = my_addr, dst = request.src" — the response goes back
        // to whoever sent the request, not to my_addr_ itself.
        const FrameFields resp{f.src,   my_addr_, /*response=*/true,
                               f.retry, f.seq,    static_cast<uint8_t>(resp_len),
                               payload};
        size_t written = 0;
        // encode_frame cannot fail here — by construction, pinned (rule 11): my_addr_ is
        // a real trunk address supplied at construction (never 0xFF), so ReservedAddress
        // cannot fire; resp_len <= LIMIT_max_l3_payload (the handler's own cap `sizeof
        // payload`), so PayloadTooLong cannot fire; and buffer_.bytes is sized kMaxWire,
        // encode_frame's own worst-case bound for the largest payload it accepts:
        static_assert(kMaxWire >= 2 + 2 * (kHeaderLen + omgp::LIMIT_max_l3_payload + kCrcLen),
                      "buffer_.bytes must hold encode_frame's worst case for the largest "
                      "response a RequestHandler can write");
        encode_frame(resp, buffer_.bytes, sizeof buffer_.bytes, written);
        buffer_.len = static_cast<uint16_t>(written);
        buffer_.seq = f.seq;
        buffer_.valid = true;
        stats_.transactions++;
    }

    state_ = State::Scheduled;
    request_end_us_ = request_end_us;
    deadline_us_ = request_end_us + turnaround_us_;
}

void Responder::poll(uint64_t now_us) {
    uint8_t byte;
    uint64_t start_us;
    // Drained unconditionally, exactly like Master::poll (FR-011/FR-017): a frame that
    // arrives while a previous response is still Scheduled is still parsed and
    // discarded/counted rather than left to overrun the wire's own RX queue.
    while (wire_.receive(byte, start_us)) {
        const uint32_t discards_before = total_discards(deframer_.stats());
        FrameView view{};
        const bool delivered = deframer_.feed(byte, view);
        if (delivered) {
            const uint64_t request_end_us = start_us + byte_time_us(wire_.bit_rate());
            on_request(view.f, request_end_us);
            continue;
        }
        if (total_discards(deframer_.stats()) > discards_before)
            stats_.discards++;
    }

    if (state_ == State::Scheduled && now_us >= deadline_us_) {
        // FR-014 "late poll": the response is due, but if this is the first poll() call
        // to reach it and now_us is already past the OUTER T_turn_max bound (not just
        // this Responder's own, possibly shorter, configured deadline), it is
        // transmitted at once rather than backdated, and counted rather than silently
        // dropped.
        const uint64_t late_bound_us = request_end_us_ + omgp::TRUNK_T_turn_max_us;
        const bool late = now_us > late_bound_us;
        wire_.transmit(buffer_.bytes, buffer_.len, now_us);
        if (late)
            stats_.late_responses++;
        state_ = State::Listening;
    }
}

} // namespace link
} // namespace omgp
