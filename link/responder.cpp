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
    // At turnaround_us == T_turn_min_us the fall-through path (final `return
    // turnaround_us`) already yields T_turn_min_us, so widening `<` to `<=` returns the
    // identical value via the other branch instead.
    // mutant-ok(equivalent, cxx_lt_to_le): the mutation and the original coincide.
    if (turnaround_us < omgp::TRUNK_T_turn_min_us)
        return omgp::TRUNK_T_turn_min_us;
    // Symmetric with T_turn_min_us above — at turnaround_us == T_turn_max_us the
    // fall-through path already yields T_turn_max_us.
    // mutant-ok(equivalent, cxx_gt_to_ge): the mutation and the original coincide.
    if (turnaround_us > omgp::TRUNK_T_turn_max_us)
        return omgp::TRUNK_T_turn_max_us;
    return turnaround_us;
}

// Sum of every Deframer discard category (data-model.md §3). The Responder's own
// stats() does not distinguish discard reasons (contracts/link-cpp.md: just
// "discards"), so a before/after delta of this total is enough to notice one byte-level
// discard, whatever caused it — mirrors Master::poll's own bad_crc_before/after delta.
uint32_t total_discards(const DeframerStats& s) {
    // total is a fresh local re-initialized on every call; poll() only ever compares two
    // such calls' results against each other (discards_before/after), so any constant
    // initial offset is added to both sides of that comparison identically and cancels
    // out of the delta.
    // mutant-ok(equivalent, cxx_init_const): the mutation and the original coincide.
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

    // state_ == Listening here, by construction of poll(): bytes are only drained from the
    // wire while nothing is Scheduled or Transmitting, so a request can never reach this
    // point while a previous response is pending (red team @e510b29 finding 1 — the
    // earlier "discard while busy" rule here dropped a queued retry against FR-015).

    // f.src == buffer_.peer: a retry with a colliding sequence from a DIFFERENT station
    // must not be served the previous requester's buffered answer (red-team @033182a
    // finding 3; docs/OPEN-QUESTIONS.md 2026-09-06) — treated as new instead, below.
    const bool is_replay =
        f.retry && buffer_.valid && f.seq == buffer_.seq && f.src == buffer_.peer;
    if (is_replay) {
        // data-model.md §5: "retry == 1 && valid && seq == buffer.seq -> retransmit
        // buffer (no handler call)" — the exact bytes already sent, never re-encoded.
        stats_.replays_served++;
    } else {
        // Any other intact request is new (data-model.md §5): a differing sequence, no
        // retry bit, a retry bit with nothing yet buffered to replay (spec US3 AS5), or a
        // retry whose sequence collides with a DIFFERENT station's buffered response.
        uint8_t payload[omgp::LIMIT_max_l3_payload];
        const size_t resp_len = handler_.handle(f.payload, f.len, payload, sizeof payload);
        // A RequestHandler must never return more than its own `cap` (== sizeof payload
        // here); checked before the uint8_t cast below, which would otherwise wrap a
        // value like 256 back into a legal-looking length and silently encode a corrupt
        // response (red-team @033182a finding 2).
        if (resp_len > sizeof payload) {
            stats_.discards++;
            return;
        }
        // data-model.md §5: "src = my_addr, dst = request.src" — the response goes back
        // to whoever sent the request, not to my_addr_ itself.
        const FrameFields resp{f.src,   my_addr_, /*response=*/true,
                               f.retry, f.seq,    static_cast<uint8_t>(resp_len),
                               payload};
        // encode_frame() (link/frame.cpp) unconditionally overwrites `written` as its
        // very first statement on every return path, so this initial value is never
        // observed.
        // mutant-ok(equivalent, cxx_init_const): the mutation and the original coincide.
        size_t written = 0;
        // buffer_.bytes must be big enough for encode_frame's own worst-case bound for
        // the largest payload a RequestHandler can write (it never depends on this
        // call's actual resp_len):
        static_assert(kMaxWire >= 2 + 2 * (kHeaderLen + omgp::LIMIT_max_l3_payload + kCrcLen),
                      "buffer_.bytes must hold encode_frame's worst case for the largest "
                      "response a RequestHandler can write");
        // `f.src == 0xFF` is already excluded above, so ReservedAddress cannot fire on
        // this call's `resp.dst`, and resp_len is already bounded above — but the
        // returned Status is still checked rather than assumed: on any refusal, nothing
        // is scheduled and the existing replay buffer is left untouched, rather than
        // committing a transaction and a buffer entry for a response that was never
        // encoded.
        if (encode_frame(resp, buffer_.bytes, sizeof buffer_.bytes, written) != Status::Ok) {
            // Unreachable: both of encode_frame's refusal conditions are excluded above
            // (ReservedAddress: f.src != 0xFF; BufferTooSmall: resp_len bounded, kMaxWire
            // static_asserted). Kept as defence in depth; no test can reach the counter to
            // observe which way it moves.
            // mutant-ok(accepted, cxx_post_inc_to_post_dec): unreachable by construction.
            stats_.discards++;
            return;
        }
        buffer_.len = static_cast<uint16_t>(written);
        buffer_.seq = f.seq;
        buffer_.peer = f.src;
        buffer_.valid = true;
        stats_.transactions++;
    }

    state_ = State::Scheduled;
    request_end_us_ = request_end_us;
    deadline_us_ = request_end_us + turnaround_us_;
}

void Responder::transmit_if_due(uint64_t now_us) {
    if (state_ == State::Transmitting) {
        // Strict `<`: transmit_until_us_ is "the instant of the final stop bit" (ByteWire),
        // so a poll() at exactly that instant already finds the wire free and drains the
        // next queued request at once, not one poll later (killing test: "two requests
        // queued before one late poll…", the poll at exactly tx_end).
        if (now_us < transmit_until_us_)
            return;
        state_ = State::Listening;
    }
    if (state_ != State::Scheduled || now_us < deadline_us_)
        return;
    // FR-014 "late poll": the response is due, but if this is the first poll() call to
    // reach it and now_us is already past the OUTER T_turn_max bound (not just this
    // Responder's own, possibly shorter, configured deadline), it is transmitted at once
    // rather than backdated, and counted rather than silently dropped.
    const uint64_t late_bound_us = request_end_us_ + omgp::TRUNK_T_turn_max_us;
    const bool late = now_us > late_bound_us;
    transmit_until_us_ = wire_.transmit(buffer_.bytes, buffer_.len, now_us);
    if (late)
        stats_.late_responses++;
    state_ = State::Transmitting;
}

void Responder::poll(uint64_t now_us) {
    uint8_t byte;
    uint64_t start_us;
    // The wire is drained only while Listening. Trunk §3 is half-duplex, one transaction
    // at a time: while a response is Scheduled (encoded, not yet due) or Transmitting
    // (physically occupying the wire until transmit_until_us_) any bytes that have already
    // arrived stay where they are — in the wire's receive queue, exactly as they would in
    // a UART's RX FIFO — and are picked up, intact, by the first poll() after the wire is
    // free. So several requests queued ahead of an infrequent poll() call are each
    // answered in turn (FR-014: late ones counted, none dropped), a queued trunk §7 retry
    // is replayed (FR-015), and a later request can never overwrite a response still
    // pending in buffer_ (red team @e510b29 finding 1; docs/OPEN-QUESTIONS.md
    // 2026-09-06). A response already due is flushed ahead of every byte, not just once
    // at the end, so a queued request is decoded the instant the wire is free.
    for (;;) {
        transmit_if_due(now_us);
        if (state_ != State::Listening)
            break;
        if (!wire_.receive(byte, start_us))
            break;

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
    // No trailing transmit_if_due(): every exit from the loop above is preceded by one,
    // and a request accepted by on_request() (state_ -> Scheduled) loops back to the top.
}

} // namespace link
} // namespace omgp
