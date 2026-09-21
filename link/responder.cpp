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
        uint8_t payload[omgp::LIMIT_max_l3_message];
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
        static_assert(kMaxWire >= 2 + 2 * (kHeaderLen + omgp::LIMIT_max_l3_message + kCrcLen),
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
            // sizeof payload == LIMIT_max_l3_message, kMaxWire static_asserted;
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
    // Reached only while a response is pending and its window has closed -- the state in
    // which the engine keeps reading the wire so its picture of the bus stays complete.
    if (!acceptable(f)) {
        // Not this node's to answer: discarded and counted exactly as while Listening.
        stats_.discards++;
        return;
    }
    // Appended in arrival order while a slot is free. Past kHeldRequests the request is
    // DISCARDED AND COUNTED rather than left unread on the wire: the maintainer ruling of
    // 2026-09-11 (docs/OPEN-QUESTIONS.md "Maintainer rulings ...", item 5; FR-014 and
    // data-model.md §5 carry the marker) put the engine's complete reading of the bus above
    // FR-014's "at once" and §5's "nothing is dropped", because the corner this replaces lost
    // requests at the WIRE, uncounted (74 offered, 7 answered, discards == 0; red team
    // @bab7378 finding 2). The loss is the same order of magnitude; the difference is that
    // this one is in stats() where FR-016's channel exists to show it.
    //
    // This branch does not read f.retry, so it drops a trunk §7 RETRY on the same terms --
    // against FR-015 and FR-016, neither of which item 5 amends. (FR-015 does carry a
    // 2026-09-11 marker, but it is item 7's request-byte-matching amendment, #373, which says
    // WHICH retries match the buffer -- not what happens to one discarded before that test is
    // reached. FR-016 carries no 2026-09-11 marker.) Deliberate (the ruled corner is "a
    // completed request beyond the hold is discarded and counted", and a retry is a request),
    // disclosed rather than assumed: docs/OPEN-QUESTIONS.md 2026-09-14 "item 5 of the
    // 2026-09-11 rulings also discards a trunk §7 retry", Ruling: PENDING -- human. Review
    // @3dfe0e3, @dcde3a2; pinned by the "retry completing when the hold is already full" case.
    if (held_count_ >= kHeldRequests) {
        stats_.discards++;
        return;
    }
    // Modulo kHeldRequests == 2, addition and subtraction coincide: -y ≡ y (mod 2), so
    // (head + count) % 2 == (head - count) % 2 for every reachable pair. Checked exhaustively
    // over the reachable domain (head in {0,1}, count in {0,1,2}): no pair differs. The
    // static_assert below is what keeps this true -- at kHeldRequests == 3 the two DO differ
    // (head=0,count=1 is the first), and the label would then be wrong rather than merely
    // stale, so the depth may not change without revisiting it.
    // mutant-ok(equivalent, cxx_add_to_sub): the mutation and the original coincide mod 2.
    HeldRequest& slot = held_[(held_head_ + held_count_) % kHeldRequests];
    slot.f = f;
    slot.f.payload = slot.payload;
    // f.len is bounded by the Deframer, which refuses anything longer as Discard::BadLength
    // (link/frame.cpp), so this copy cannot overrun slot.payload.
    static_assert(omgp::LIMIT_max_l3_message <= sizeof(HeldRequest::payload),
                  "held_ payloads must hold the longest request the Deframer can deliver");
    // `f.len >= 0` is true for every uint8_t, and memcpy of 0 bytes is a no-op, so widening
    // the comparison changes nothing. Narrowing it the other way DOES lose the payload, and
    // is killed by the distinct-payload assertions in "a request arriving when the hold is
    // already full ..." (deep-verify @6440074 found the suite blind to exactly that).
    // mutant-ok(equivalent, cxx_gt_to_ge): the mutation and the original coincide.
    if (f.len > 0)
        std::memcpy(slot.payload, f.payload, f.len);
    slot.request_end_us = request_end_us;
    held_count_++;
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
        // last_activity_us_ is now worth the same here as it is for the Master, because the
        // drain that produced it has the Master's precondition again: poll() reads to the end
        // of the wire's queue on every path into this line (ruling 2026-09-11, #372 -- a
        // completed request the hold cannot take is discarded and counted, not left unread).
        // The reading is therefore COMPLETE, and T_gap of idle after the last byte of it is a
        // real gap. The `belief_stale` argument this line used to take, and the partial
        // reading it reported, are gone with the stop that created them.
        //
        // What the cap still is NOT (review @6440074 finding 2, kept because only half of it
        // has been fixed): Master argues that any frame starting later than defer_origin +
        // T_gap is a §3 violator "on every path into this state"; on the LATE path it is not,
        // because the host has already timed this node out after T_resp and may legitimately
        // open a new transaction T_gap after the bus goes idle. Such a frame can still be
        // running when the cap expires, and the cap then fires into it. What #372 removes is
        // the BLIND case -- keying down onto a bus the engine stopped reading, red team
        // @6440074 finding 1, whose reproducer is the "does not blind the engine" case in
        // tests/unit/test_link_responder.cpp. The residual is bounded by construction (only
        // a frame that begins after defer_origin + T_gap and is still running at
        // defer_origin + max_frame + T_gap) and is recorded in docs/OPEN-QUESTIONS.md
        // 2026-09-14; no test here demonstrates it, and this comment claims no more than
        // that.
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
    // A bool assigned `false`; the mutation writes 0, which is the same value.
    // mutant-ok(equivalent, cxx_assign_const): the mutation and the original coincide.
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
    // answered in turn (FR-014: late ones counted; past what one late wait can hold,
    // discarded and counted too -- below), a queued trunk §7 retry is replayed (FR-015)
    // WHEN IT IS ONE OF THOSE THE ENGINE CAN ANSWER, and a later request can never overwrite
    // a response still pending in buffer_ (red team @e510b29 finding 1;
    // docs/OPEN-QUESTIONS.md 2026-09-06).
    //
    // That FR-015 clause is qualified, and the qualification is a known divergence rather
    // than an oversight (review @3dfe0e3). hold_or_discard()'s discard branch tests
    // acceptable() only -- dst, the response bit, src, trunk §5's range -- never f.retry, so a
    // retry completing past kHeldRequests inside one late wait is discarded and counted like
    // any other request: it gets neither the buffered frame (FR-015 "MUST retransmit ...
    // unchanged") nor a fresh answer (FR-016 "MUST be treated as new"), and the host sees a
    // second T_resp timeout. Item 5 of the 2026-09-11 rulings amends FR-014 and data-model §5
    // only -- FR-015's own 2026-09-11 marker is item 7's (#373, which bytes a retry must
    // repeat to match the buffer) and does not reach this clause, and FR-016 has none -- so
    // this consequence sits against two MUSTs unamended for it; it is recorded, not resolved
    // here (docs/OPEN-QUESTIONS.md 2026-09-14 "item 5 of the 2026-09-11 rulings also discards
    // a trunk §7 retry", Ruling: PENDING -- human) and pinned by
    // tests/unit/test_link_responder.cpp
    // "a trunk §7 retry completing when the hold is already full ...".
    //
    // A response already due is flushed ahead of every byte, not just once
    // at the end, so a queued request is decoded the instant the wire is free.
    //
    // The other side of that rule, stated rather than hidden: one accepted request leaves
    // Listening and ends this drain, so a single poll() answers at most ONE queued request,
    // oldest first, and a node polled far more slowly than it is addressed answers a small
    // fraction of what it is offered (red team + review @17554c8 finding 1). What has changed
    // under the 2026-09-11 ruling (#372) is the SILENCE, not the throughput: the backlog no
    // longer accumulates unread on the wire, because everything the wire holds is read at
    // every poll and everything read is either answered or counted in stats().discards. Two
    // held requests deep is what one wait can carry; the rest is counted loss, visible to the
    // layer above. FR-014 ("MUST still transmit") and FR-017 ("never outside a response
    // window") still pull opposite ways on a stale queued request; which bound applies is
    // docs/OPEN-QUESTIONS.md 2026-09-06 ("held-request queue is unbounded"), pending human --
    // not decided here, and the 2026-09-11 ruling says so in terms.
    for (;;) {
        transmit_if_due(now_us, /*queue_drained=*/false);
        const bool listening = state_ == State::Listening;
        if (listening && held_count_ > 0) {
            // The wire is free again: requests decoded during the wait are answered, oldest
            // first, before any newer byte is drained, so arrival order is preserved.
            HeldRequest h = held_[held_head_];
            h.f.payload = h.payload;
            // As at the append above: mod kHeldRequests == 2, (head + 1) and (head - 1) are
            // the same index, so the mutation and the original coincide. (The label must be
            // the LAST comment line before the code it governs -- tools/mutate_report.py
            // reads "the line directly beneath". Written above a second comment line, it
            // covered that line instead and the gate reported it stale while the real
            // survivor sat one line further down.)
            // mutant-ok(equivalent, cxx_add_to_sub): the mutation and the original coincide.
            held_head_ = (held_head_ + 1) % kHeldRequests;
            held_count_--;
            on_request(h.f, h.request_end_us);
            continue;
        }
        // Listening: the normal drain. past_window(): a response whose window has closed is
        // waiting for an idle bus, and the only way this engine can see the bus is to read
        // it -- so it does, decoding as it goes, to the END of the queue every time (ruling
        // 2026-09-11, #372). What it decodes there it cannot answer (trunk §3, one
        // transaction at a time): up to kHeldRequests of those are HELD and answered later,
        // and any further one is discarded and counted (hold_or_discard). The drain does NOT
        // stop to avoid that discard, which is what it used to do -- the bytes left behind
        // then were destroyed by the wire's own queue instead, uncounted, and the engine's
        // reading of the bus went stale for the rest of the wait. While Transmitting, and
        // while Scheduled INSIDE the window, bytes wait untouched in the wire's receive queue
        // as before (red team @e510b29 finding 1): there the engine owns the bus by protocol
        // and needs no reading of it.
        const bool waiting = past_window(now_us);
        if (!listening && !waiting)
            break;
        if (!wire_.receive(byte, start_us))
            break;
        // Every byte drained, whatever becomes of it, is evidence the bus was busy: its END
        // instant, since within a frame each byte starts where the previous one ended
        // (Master's identical recording, link/master.cpp:215-227). Recorded on the waiting
        // path too -- that is what keeps the belief complete.
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
    // The drain loop has stopped: the wire's queue is empty, or an accepted request ended
    // it. Only now -- with last_activity_us_ current for every byte the engine could read --
    // can a late response be judged against the bus. On the late path this is always the
    // empty-queue exit, since nothing else can end the drain while a response is waiting:
    // the reading is complete (#372).
    transmit_if_due(now_us, /*queue_drained=*/true);
}

} // namespace link
} // namespace omgp
