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
    // mutant-ok(equivalent, cxx_lt_to_le): at turnaround_us == T_turn_min_us the
    // fall-through path (final `return turnaround_us`) already yields T_turn_min_us, so
    // widening `<` to `<=` returns the identical value via the other branch instead.
    if (turnaround_us < omgp::TRUNK_T_turn_min_us)
        return omgp::TRUNK_T_turn_min_us;
    // mutant-ok(equivalent, cxx_gt_to_ge): symmetric with T_turn_min_us above — at
    // turnaround_us == T_turn_max_us the fall-through path already yields T_turn_max_us.
    if (turnaround_us > omgp::TRUNK_T_turn_max_us)
        return omgp::TRUNK_T_turn_max_us;
    return turnaround_us;
}

// Sum of every Deframer discard category (data-model.md §3). The Responder's own
// stats() does not distinguish discard reasons (contracts/link-cpp.md: just
// "discards"), so a before/after delta of this total is enough to notice one byte-level
// discard, whatever caused it — mirrors Master::poll's own bad_crc_before/after delta.
uint32_t total_discards(const DeframerStats& s) {
    // mutant-ok(equivalent, cxx_init_const): total is a fresh local re-initialized on
    // every call; poll() only ever compares two such calls' results against each other
    // (discards_before/after), so any constant initial offset is added to both sides of
    // that comparison identically and cancels out of the delta.
    uint32_t total = 0;
    for (uint32_t d : s.discarded)
        total += d;
    return total;
}

} // namespace

Responder::Responder(ByteWire& wire, Clock& clock, RequestHandler& handler, uint8_t my_addr,
                     uint32_t turnaround_us)
    : wire_(wire), clock_(clock), handler_(handler), my_addr_(my_addr),
      turnaround_us_(clamp_turnaround(turnaround_us)) {
    (void)clock_; // discarded read: see the clock_ declaration comment in responder.hpp
}

const AddrStats& Responder::stats() const {
    return stats_;
}

void Responder::on_request(const FrameFields& f, uint64_t request_end_us) {
    if (f.dst != my_addr_ || f.response ||
        f.src == 0xFF) { // literal-ok: trunk §5 reserved address, not an L3 event code
        // Not a request addressed to this node (data-model.md §5 "Request
        // acceptance"), or its claimed source is the reserved marker 0xFF: a response's
        // `dst` IS `f.src` (below), and encode_frame refuses to originate `dst == 0xFF`
        // (link/frame.cpp:17-18) — such a request could never be answered. The Deframer
        // validates only `dst == 0xFF` (link/frame.cpp:142), never `src`, so this must be
        // rejected here rather than left to poison the replay buffer. Either way:
        // silently discarded, counted (trunk §4).
        stats_.discards++;
        return;
    }

    if (state_ == State::Scheduled) {
        // A previous request's response is still pending transmission — not yet due
        // either, since poll() (transmit_if_due) already flushes anything overdue ahead
        // of this frame. Trunk §3 is half-duplex with one transaction at a time (spec.md
        // Edge Cases: the host never has two transactions to one node in flight), so a
        // second accepted request landing here can only be a rogue/babbling station or a
        // second host. Discard it and count it rather than silently overwriting the
        // pending response and its replay buffer.
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
        // mutant-ok(equivalent, cxx_init_const): encode_frame() (link/frame.cpp)
        // unconditionally overwrites `written` as its very first statement on every
        // return path, so this initial value is never observed.
        size_t written = 0;
        // buffer_.bytes must be big enough for encode_frame's own worst-case bound for
        // the largest payload a RequestHandler can write (it never depends on this
        // call's actual resp_len):
        static_assert(kMaxWire >= 2 + 2 * (kHeaderLen + omgp::LIMIT_max_l3_payload + kCrcLen),
                      "buffer_.bytes must hold encode_frame's worst case for the largest "
                      "response a RequestHandler can write");
        // `f.src == 0xFF` is already excluded above, so ReservedAddress cannot fire on
        // this call's `resp.dst` — but a RequestHandler is application code (F4-facing)
        // and could still misbehave (e.g. return more than its own `cap`), so the
        // returned Status is checked rather than assumed: on any refusal, nothing is
        // scheduled and the existing replay buffer is left untouched, rather than
        // committing a transaction and a buffer entry for a response that was never
        // encoded.
        if (encode_frame(resp, buffer_.bytes, sizeof buffer_.bytes, written) != Status::Ok) {
            stats_.discards++;
            return;
        }
        buffer_.len = static_cast<uint16_t>(written);
        buffer_.seq = f.seq;
        buffer_.valid = true;
        stats_.transactions++;
    }

    state_ = State::Scheduled;
    request_end_us_ = request_end_us;
    deadline_us_ = request_end_us + turnaround_us_;
}

void Responder::transmit_if_due(uint64_t now_us) {
    if (state_ != State::Scheduled || now_us < deadline_us_)
        return;
    // FR-014 "late poll": the response is due, but if this is the first poll() call to
    // reach it and now_us is already past the OUTER T_turn_max bound (not just this
    // Responder's own, possibly shorter, configured deadline), it is transmitted at once
    // rather than backdated, and counted rather than silently dropped.
    const uint64_t late_bound_us = request_end_us_ + omgp::TRUNK_T_turn_max_us;
    const bool late = now_us > late_bound_us;
    wire_.transmit(buffer_.bytes, buffer_.len, now_us);
    if (late)
        stats_.late_responses++;
    state_ = State::Listening;
}

void Responder::poll(uint64_t now_us) {
    uint8_t byte;
    uint64_t start_us;
    // Drained unconditionally, exactly like Master::poll (FR-011/FR-017). A response
    // already due is flushed ahead of every byte (not just once at the end): several
    // requests queued ahead of an infrequent poll() call must each still be answered in
    // turn, rather than a later one's on_request() silently overwriting a not-yet-
    // transmitted response that was, by this call's own now_us, already due.
    while (wire_.receive(byte, start_us)) {
        transmit_if_due(now_us);

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

    transmit_if_due(now_us);
}

} // namespace link
} // namespace omgp
