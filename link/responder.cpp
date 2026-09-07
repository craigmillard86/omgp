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

bool Responder::acceptable(const FrameFields& f) const {
    // `f.src` is wire-derived: the Deframer validates only `dst == 0xFF` (link/frame.cpp:142),
    // never `src`, so this is the one place the claimed source is bounded before it becomes
    // a response's `dst` (below) and the replay key's `peer`. Trunk §5 confines L2 addresses
    // to ADDR_host..ADDR_backplane_max — kAddrCount of them, module node IDs never among
    // them — and a station is never its own peer. Master::begin (link/master.cpp:87) and
    // HealthTracker (link/health.cpp) bound the same wire-derived class the same way; the
    // Responder was the third reader of one without a bound (red team + review @6c2fe4b
    // finding 2). The static_assert keeps the reserved marker 0xFF — the earlier, narrower
    // screen: a response to it could never be encoded (link/frame.cpp:17-18) — inside the
    // refused range if kAddrCount is ever edited, rather than letting that lapse silently.
    // my_addr_ is the symmetric half: it becomes the `src` of every frame this node
    // originates (below), and was the last address in this engine with no bound at all
    // (red team @71caba0 finding 3). A Responder configured outside trunk §5's range
    // answers nothing rather than putting a non-L2 source on the trunk — the constructor
    // returns no Status, so this is the only place the rule can be enforced, and it is
    // enforced where every other address is.
    static_assert(kAddrCount <= 0xFF, // literal-ok: trunk §5 reserved address, not an event code
                  "kAddrCount must leave trunk §5's reserved address refused by src >= kAddrCount");
    return f.dst == my_addr_ && !f.response && f.src != my_addr_ && f.src < kAddrCount &&
           my_addr_ < kAddrCount;
}

void Responder::on_request(const FrameFields& f, uint64_t request_end_us) {
    if (!acceptable(f)) {
        // Not a request addressed to this node (data-model.md §5 "Request acceptance"), or
        // one whose claimed source this node could never legitimately answer: silently
        // discarded, counted (trunk §4), and — load-bearing for FR-015 — never allowed to
        // reach buffer_, where its `src` would evict a real station's replay entry.
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
            // Unreachable: all three of encode_frame's refusal conditions (link/frame.hpp)
            // are excluded above (PayloadTooLong and BufferTooSmall: resp_len bounded by
            // sizeof payload == LIMIT_max_l3_payload, kMaxWire static_asserted;
            // ReservedAddress: f.src != 0xFF). Kept as defence in depth; no test can reach
            // the counter to observe which way it moves.
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

void Responder::hold_or_discard(const FrameFields& f, uint64_t request_end_us) {
    // Reached only from the late path, where the engine drains the wire to see the bus
    // (poll()). A frame it could not answer anyway is discarded and counted exactly as it
    // would be while Listening — the same screen, so the only difference from before this
    // round is WHEN the discard is counted, not whether.
    if (!acceptable(f)) {
        stats_.discards++;
        return;
    }
    // Otherwise it is a request this node owes an answer to, and the wire's receive queue
    // is no longer holding it for us: it moves into the engine, unanswered, until the
    // pending response has gone out. Nothing is dropped (FR-015/FR-016; red team @e510b29
    // finding 1). held_.valid is false here by construction — poll() stops draining while
    // it is set — so no held request can be overwritten by a later one.
    held_.f = f;
    held_.f.payload = held_.payload;
    // f.len is bounded by the Deframer, which refuses anything longer as Discard::BadLength
    // (link/frame.cpp) — so this copy cannot overrun held_.payload.
    static_assert(omgp::LIMIT_max_l3_payload <= sizeof(HeldRequest::payload),
                  "held_.payload must hold the longest request the Deframer can deliver");
    if (f.len > 0)
        std::memcpy(held_.payload, f.payload, f.len);
    held_.request_end_us = request_end_us;
    held_.valid = true;
}

bool Responder::past_window(uint64_t now_us) const {
    // The OUTER bound of trunk §9's turnaround, not this engine's own (possibly shorter)
    // configured deadline: up to it, FR-017's "never outside a response window" is what
    // every other station is obeying too, so the bus is this node's by protocol.
    return state_ == State::Scheduled && now_us > request_end_us_ + omgp::TRUNK_T_turn_max_us;
}

uint64_t Responder::max_frame_us() const {
    // kMaxWire byte times — the codec's own worst-case stuffed-frame bound, recomputed on
    // every call so a set_bit_rate() during the wait is honoured at once (Master's
    // max_frame_us(), link/master.cpp:173-175, verbatim reasoning).
    return static_cast<uint64_t>(kMaxWire) * byte_time_us(wire_.bit_rate());
}

void Responder::transmit_if_due(uint64_t now_us, bool queue_drained) {
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
    const bool late = past_window(now_us);
    if (late) {
        // The belief below is only as good as what the engine has actually seen, so the
        // late transmit happens once poll()'s drain loop has stopped — never at the top of
        // an iteration with bytes still unread behind it, which is precisely how the
        // engine used to key down inside another station's frame.
        if (!queue_drained)
            return;
        // ...at once, but not blind. Inside the window the bus is this node's by protocol
        // (trunk §3: the host is the only initiator and is waiting out T_resp); outside it
        // that guarantee is gone, and ByteWire's contract — "the engine never transmits
        // while it believes the bus is busy" — obliges the engine to form the belief from
        // what is actually on the wire, as Master::fire_pending does (link/master.cpp:205).
        // poll() has drained every byte due at now_us before calling this on the late path,
        // so last_activity_us_ is current (red team @71caba0 finding 1: the engine keyed
        // down strictly inside another station's frame, corrupting a frame addressed to a
        // THIRD node, which trunk §7 then escalates to SUSPECT).
        //
        // Never transmit with less than T_gap of idle after the last byte received
        // (data-model.md §4 "Gap"). Within a frame bytes are contiguous, so the last byte
        // drained at any poll instant ends strictly after now_us — the wait therefore holds
        // for as long as bytes keep coming, at a constant bit rate (Master's own argument,
        // link/master.cpp:215-235; the same rate-change caveat applies, docs/OPEN-QUESTIONS
        // 2026-09-06 "rate change mid-stream").
        if (!has_defer_origin_) {
            defer_origin_us_ = now_us;
            has_defer_origin_ = true;
        }
        // ...but bounded, for the reason Master's cap exists (link/master.cpp:241-273): a
        // station holding the wire without pause would otherwise deny the gap forever and
        // starve the response, against FR-014's "MUST still transmit". One worst-case frame
        // plus T_gap past the instant the transmission was first deferred is long enough for
        // any single frame already on the wire then to finish AND receive its full gap, and
        // no longer; past it, a station still occupying the wire is a §3 violator and the
        // transaction goes out on schedule.
        const uint64_t cap_us = defer_origin_us_ + max_frame_us() + omgp::TRUNK_T_gap_us;
        uint64_t want_us = has_activity_ ? last_activity_us_ + omgp::TRUNK_T_gap_us : now_us;
        // `a > b ? b : a` and `a >= b ? b : a` both compute min(a, b); they differ only at
        // a == b, where both branches yield the same value (Master's label, :278-281).
        // mutant-ok(equivalent, cxx_gt_to_ge): min(a, b) either way.
        if (want_us > cap_us)
            want_us = cap_us;
        if (now_us < want_us)
            return; // deferred: still Scheduled, retried on the next poll()
    }
    transmit_until_us_ = wire_.transmit(buffer_.bytes, buffer_.len, now_us);
    if (late)
        stats_.late_responses++;
    has_defer_origin_ = false;
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
    //
    // The other side of that rule, stated rather than hidden: one accepted request leaves
    // Listening and ends this drain, so a single poll() answers at most ONE queued
    // request, oldest first, and nothing bounds the backlog's age or depth or counts a
    // request that waits in it (red team + review @17554c8 finding 1: requests arriving
    // faster than poll() is called starve a later one indefinitely, every answer late,
    // stats() otherwise unmoved). FR-014 ("MUST still transmit") and FR-017 ("never
    // outside a response window") pull opposite ways on a stale queued request; which
    // bound applies is docs/OPEN-QUESTIONS.md 2026-09-06 ("held-request queue is
    // unbounded"), pending human -- not decided here.
    for (;;) {
        transmit_if_due(now_us, /*queue_drained=*/false);
        const bool listening = state_ == State::Listening;
        if (listening && held_.valid) {
            // The wire is free again: the request decoded during the wait is answered
            // before any newer byte is drained, so the queue keeps its arrival order.
            const HeldRequest h = held_;
            held_.valid = false;
            FrameFields f = h.f;
            f.payload = h.payload;
            on_request(f, h.request_end_us);
            continue;
        }
        // Listening: the normal drain. past_window(): a response whose window has closed
        // is waiting for an idle bus, and the ONLY way this engine can see the bus is to
        // read it — so it does, and holds what it decodes (red team @71caba0 finding 1).
        // While Transmitting, and while Scheduled INSIDE the window, bytes still wait
        // untouched in the wire's receive queue (red team @e510b29 finding 1).
        if (!listening && !(past_window(now_us) && !held_.valid))
            break;
        if (!wire_.receive(byte, start_us))
            break;
        // Every byte drained, whatever becomes of it, is evidence the bus was busy: its
        // END instant, since within a frame each byte starts where the previous one ended
        // (Master's identical recording, link/master.cpp:215-227).
        last_activity_us_ = start_us + byte_time_us(wire_.bit_rate());
        has_activity_ = true;

        const uint32_t discards_before = total_discards(deframer_.stats());
        FrameView view{};
        const bool delivered = deframer_.feed(byte, view);
        if (delivered) {
            const uint64_t request_end_us = start_us + byte_time_us(wire_.bit_rate());
            if (listening) {
                on_request(view.f, request_end_us);
            } else {
                hold_or_discard(view.f, request_end_us);
            }
            continue;
        }
        if (total_discards(deframer_.stats()) > discards_before)
            stats_.discards++;
    }
    // The drain loop has stopped: either the wire's queue is empty, or a decoded request
    // is held behind the response that must go out first. Only now can a late response be
    // judged against the bus (transmit_if_due's own comment) -- the in-window path already
    // fired at the top of the loop.
    transmit_if_due(now_us, /*queue_drained=*/true);
}

} // namespace link
} // namespace omgp
