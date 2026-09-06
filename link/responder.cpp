// OMGP trunk L2 — Responder engine implementation: trunk §3 (media access), §7 (retry
// rule); data-model.md §5 "Responder". Makes tests/unit/test_link_responder.cpp (T033)
// and tests/unit/test_link_loop.cpp (T034) pass.
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
} // namespace

Responder::Responder(ByteWire& wire, Clock& /*clock*/, RequestHandler& handler, uint8_t my_addr,
                     uint32_t turnaround_us)
    : wire_(wire), handler_(handler), my_addr_(my_addr),
      turnaround_us_(clamp_turnaround(turnaround_us)) {}

const AddrStats& Responder::stats() const {
    return stats_;
}

void Responder::handle_request(const FrameFields& f, uint64_t request_end_us) {
    // data-model.md §5: a retry of the SAME sequence as the buffered response is served
    // from the buffer, verbatim, with no second call to handler_ — trunk §7's retry rule
    // must be idempotent at L2 (CLAUDE.md rule 2). Any other case (a fresh sequence, or a
    // retry-flagged request that does not match a buffered answer — "different seq" or "no
    // prior answer yet") is a new request.
    const bool is_replay = f.retry && buffer_valid_ && f.seq == buffer_seq_;
    if (is_replay) {
        stats_.replays_served++;
    } else {
        uint8_t resp_payload[LIMIT_max_l3_payload];
        const size_t resp_len =
            handler_.handle(f.payload, f.len, resp_payload, sizeof resp_payload);
        // src/dst swapped from the request; retry/seq echoed (contracts/link-cpp.md
        // "Responder engine") — retry here reflects THIS request, not a later retry of it,
        // which is exactly what a matching future retry replays unmodified.
        const FrameFields resp{f.src,
                               my_addr_,
                               /*response=*/true,
                               f.retry,
                               f.seq,
                               static_cast<uint8_t>(resp_len),
                               resp_payload};
        size_t written = 0;
        // encode_frame cannot fail here — by construction (rule 11): resp_len <= cap ==
        // LIMIT_max_l3_payload (handler_'s own contract, asserted by CountingHandler-style
        // callers), my_addr_/f.src are wire-derived trunk addresses (never 0xFF: the
        // Deframer already discards a request whose src or dst is 0xFF), and buffer_ is
        // sized kMaxWire, encode_frame's own worst-case bound for LIMIT_max_l3_payload.
        encode_frame(resp, buffer_, sizeof buffer_, written);
        buffer_len_ = written;
        buffer_valid_ = true;
        buffer_seq_ = f.seq;
    }
    stats_.transactions++; // "requests handled" (contracts/link-cpp.md), new or replayed
    request_end_us_ = request_end_us;
    deadline_ = request_end_us + turnaround_us_;
    scheduled_ = true;
}

void Responder::transmit_response(uint64_t at_us) {
    wire_.transmit(buffer_, buffer_len_, at_us);
    scheduled_ = false;
}

void Responder::poll(uint64_t now_us) {
    uint8_t byte;
    uint64_t start_us;
    // The only receive path (analysis F1, mirrored from Master::poll): drained to
    // exhaustion, not just until the first accepted request, so bytes already due behind
    // one that ends this poll's processing are never left stranded on the queue.
    while (wire_.receive(byte, start_us)) {
        uint32_t discards_before = 0;
        for (uint32_t d : deframer_.stats().discarded)
            discards_before += d;

        FrameView view{};
        const bool delivered = deframer_.feed(byte, view);

        uint32_t discards_after = 0;
        for (uint32_t d : deframer_.stats().discarded)
            discards_after += d;
        if (discards_after > discards_before) {
            // A corrupt/malformed frame (data-model.md §5: "corrupt frames -> discards
            // counted"). The Deframer's own stats() already has the reason; this engine's
            // AddrStats only needs the fact that something addressed to it was refused.
            stats_.discards++;
            continue;
        }
        if (!delivered)
            continue; // accumulating mid-frame: not yet a decision either way

        const FrameFields& f = view.f;
        const uint64_t byte_end_us = start_us + byte_time_us(wire_.bit_rate());
        // Request acceptance (data-model.md §5): intact frame, dst == my_addr, response ==
        // 0. Anything else — wrong address, or a stray response-bit-set frame — is
        // discarded and counted, never transmitted (contracts/link-cpp.md).
        if (f.dst != my_addr_ || f.response) {
            stats_.discards++;
            continue;
        }
        handle_request(f, byte_end_us);
    }

    if (scheduled_ && now_us >= deadline_) {
        // FR-014: "late" is measured against request_end + T_turn_max, not against this
        // engine's own (possibly shorter) configured deadline_ — a poll cadence coarser
        // than turnaround_us_ can still land inside T_turn_max and count as on time.
        if (now_us > request_end_us_ + omgp::TRUNK_T_turn_max_us)
            stats_.late_responses++;
        transmit_response(now_us);
    }
}

} // namespace link
} // namespace omgp
