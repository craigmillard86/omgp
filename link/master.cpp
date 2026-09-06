// OMGP trunk L2 — Master transaction engine implementation: trunk §3 (media access), §7
// (retry rule); data-model.md §4 "Transaction (Master)", §8 "Statistics". Makes
// tests/unit/test_link_master.cpp (T029) pass.
#include "link/master.hpp"

#include "omgp_protocol.h"

#include <cstring>

namespace omgp {
namespace link {

Master::Master(ByteWire& wire, Clock& clock, uint8_t host_addr)
    : wire_(wire), clock_(clock), host_addr_(host_addr) {}

bool Master::busy() const {
    return open_;
}

uint8_t Master::attempts() const {
    return attempt_count_;
}

void Master::set_bit_rate(uint32_t bps) {
    // byte_time_us() has a nonzero precondition and the engine calls it on every poll (cadence
    // and frame-time bounds), so a zero rate accepted here would make the engine violate that
    // precondition from the inside — an assert in a debug build, a divide-by-zero otherwise
    // (PR #137 red-team, LOW). The same hazard sits one step out: above 10 Mb/s the integer
    // byte time truncates to 0 µs, frame_arriving()'s cadence slack and max_frame_us() (hence
    // the courtesy cap) collapse to nothing, and both protections become silent no-ops (PR #137
    // red-team @3a15d29, LOW). So the precondition the engine actually needs is stated once: a
    // byte must take at least one microsecond at the rate in force. Refused rather than
    // clamped: no rate is a defensible stand-in for an unusable one, and trunk §9's rates both
    // satisfy this. rate_changes counts changes actually applied, so a refused call does not
    // bump it (docs/OPEN-QUESTIONS.md 2026-09-06, two "set_bit_rate" entries). This guard
    // screens THIS caller only: max_frame_us()/frame_arriving() compute from wire_.bit_rate(),
    // and ByteWire::set_bit_rate is a second, unguarded door — the precondition belongs on
    // ByteWire (a contract change, pending: OPEN-QUESTIONS 2026-09-06 "screens one caller";
    // PR #137 red-team @2627be9, LOW). Assumed, not enforced here.
    if (bps == 0 || byte_time_us(bps) == 0)
        return;
    wire_.set_bit_rate(bps);
    bus_stats_.rate_changes++;
}

const AddrStats& Master::stats(uint8_t addr) const {
    // addr is wire-derived (encode_frame/the Deframer refuse only dst == 0xFF, so
    // 0x10..0xFE survive intact); mirror HealthTracker's bounds guard for the same
    // address-keyed-table shape rather than trust the caller (PR #137 review, MEDIUM;
    // red-team #118 finding 5 class).
    static const AddrStats kOutOfRange{};
    if (addr >= kAddrCount)
        return kOutOfRange;
    return stats_[addr];
}

const BusStats& Master::bus_stats() const {
    return bus_stats_;
}

void Master::reset_stats() {
    for (AddrStats& s : stats_)
        s = AddrStats{};
    bus_stats_ = BusStats{};
}

Status Master::begin(uint8_t dst, const uint8_t* payload, size_t len) {
    if (open_)
        return Status::Busy;
    // Same refusals as encode_frame (contracts/link-cpp.md), checked before any
    // transaction state changes: a refused begin() must leave next_seq_ untouched and
    // transmit nothing.
    if (len > omgp::LIMIT_max_l3_payload)
        return Status::PayloadTooLong;
    // Master's own guard, beyond encode_frame's: next_seq_/stats_ are kAddrCount-entry
    // tables indexed directly by dst (data-model.md §4 "Sequence", §8 "Statistics"), so any
    // dst outside that range must be refused here even though encode_frame and the Deframer
    // let 0x10..0xFE through as syntactically valid trunk addresses (PR #137 review, HIGH:
    // begin(0x20, ...) previously wrote next_seq_[0x20]/stats_[0x20], past both tables).
    //
    // This single guard also delivers encode_frame's own trunk §5 refusal of the reserved
    // 0xFF: a separate `dst == 0xFF` branch ahead of it was unreachable — dead code, and an
    // un-killable mutant (PR #137 review, LOW). The subsumption is pinned structurally rather
    // than assumed, so it cannot lapse silently if kAddrCount ever changes:
    static_assert(kAddrCount <= 0xFF, // literal-ok: trunk §5 reserved address, not an event code
                  "kAddrCount must leave trunk §5's reserved address refused by dst >= kAddrCount");
    if (dst >= kAddrCount)
        return Status::ReservedAddress;

    dst_ = dst;
    seq_ = next_seq_[dst];
    next_seq_[dst] = static_cast<uint8_t>((next_seq_[dst] + 1) & 0x0F);
    len_ = static_cast<uint8_t>(len);
    // The guard exists for begin(dst, nullptr, 0): memcpy's pointer arguments are declared
    // non-null even for a zero count, so the unguarded call is UB. On the wire the two are
    // indistinguishable — do_transmit only reads the first len_ bytes of payload_ (FrameFields
    // into encode_frame's `for (i < f.len)`) — but NOT equivalent: UBSan's nonnull-attribute
    // check reports the unguarded call ("null pointer passed as argument 2, which is declared
    // to never be null"), demonstrated on "…fallback bit rate…" (test_link_master.cpp:494,
    // one of five begin(dst, nullptr, 0) callers) with the mutant emulated as `if (true)` — the
    // source-level `len_ >= 0` does not compile under -Werror=type-limits. The native preset
    // has no -fno-sanitize-recover (CMakeLists.txt:17), so that report is printed and the
    // suite still exits 0 — the mutant survives by exit code, hence `accepted`, not
    // `equivalent` (PR #137 review @41983fe, LOW).
    // mutant-ok(accepted, cxx_gt_to_ge): differs only by memcpy(_, nullptr, 0) — UB, UBSan-only.
    if (len_ > 0)
        std::memcpy(payload_, payload, len_);
    attempt_count_ = 0;
    open_ = true;

    // A new transaction is itself subject to the T_gap rule (data-model.md §4 "Gap"): the
    // engine, not the caller, guarantees the gap, deferring transmission if begin() is
    // called too soon after the last activity on the bus.
    const uint64_t now = clock_.now_us();
    sub_phase_ = SubPhase::PendingTransmit;
    // The deferred instant is last_activity_ + T_gap (data-model.md §4 "Gap"), or `now` when
    // nothing has ever been on the bus. trunk §3 requires only ">= T_gap of bus idle", not
    // "exactly T_gap": when that instant is already in the past by the time begin() is called
    // (the engine's ordinary operating mode — a superframe scheduler invoking begin() once per
    // T_poll >> T_gap), fire_pending() below transmits at `now` because `now >= deadline_`;
    // the transmit instant is never a stale past one (PR #137 review, HIGH). That property
    // lives in fire_pending()'s `now_us >= deadline_`, NOT in a max(now, gap_elapsed_at) here:
    // an earlier revision computed that max and credited it with the fix, but the two are
    // indistinguishable at every call site — when the gap has elapsed, neither the push clause
    // (last_activity_ + T_gap is not > either candidate) nor the cap (both candidates are
    // <= defer_origin + max_frame + T_gap) can bind, and do_transmit(now) runs either way with
    // defer_origin_us_ then dead until end_attempt() reassigns it (PR #137 red-team @40355cf,
    // LOW: an unkillable, untagged mutant). Removed rather than tagged.
    deadline_ = has_last_activity_ ? last_activity_ + omgp::TRUNK_T_gap_us : now;
    defer_origin_us_ = deadline_; // start of the bounded wait — see the member
    fire_pending(now);
    return Status::Ok;
}

void Master::do_transmit(uint64_t at_us) {
    const bool retry = attempt_count_ > 0;
    const FrameFields f{dst_, host_addr_, /*response=*/false, retry, seq_, len_, payload_};
    uint8_t buf[kMaxWire];
    // encode_frame's own first statement is `written = 0;` (link/frame.cpp) — unconditional,
    // before any return path — so this initial value can never be read.
    // mutant-ok(equivalent, cxx_init_const): any constant here is behaviourally identical.
    size_t written = 0;
    // encode_frame cannot fail here — by construction, pinned (rule 11): its three refusals
    // (link/frame.cpp) are PayloadTooLong, refused identically at begin(); ReservedAddress
    // (0xFF), subsumed by begin()'s dst >= kAddrCount (static_assert above); and
    // BufferTooSmall, which needs `cap < 2 + 2*(kHeaderLen + len + kCrcLen)` — impossible
    // for len <= LIMIT_max_l3_payload while buf is kMaxWire bytes:
    static_assert(
        kMaxWire >= 2 + 2 * (kHeaderLen + omgp::LIMIT_max_l3_payload + kCrcLen),
        "buf must hold encode_frame's worst case for the largest payload begin() accepts");
    // Were any of those to lapse, encode_frame would leave written == 0 and the wire would
    // see nothing while the transaction was counted and a T_resp window opened (PR #137
    // review @0263d0f, LOW) — hence the pins rather than a return-value branch that no test
    // could reach.
    encode_frame(f, buf, sizeof buf, written);
    const uint64_t tx_end = wire_.transmit(buf, written, at_us);

    // FR-011a: "transactions" counts transactions STARTED, not concluded (contracts/
    // link-cpp.md / data-model.md §8) — the first transmission of a NEW transaction, not
    // each retry (which has its own `retries` counter below), and not its eventual
    // Answered/Failed outcome (PR #137 review, MEDIUM: this used to be counted once per
    // conclusion, so an in-flight transaction under-reported its own existence).
    if (attempt_count_ == 0)
        stats_[dst_].transactions++;
    else
        stats_[dst_].retries++;
    ++attempt_count_;
    sub_phase_ = SubPhase::AwaitResponse;
    window_start_us_ = tx_end;
    deadline_ = tx_end + omgp::TRUNK_T_resp_us;
}

uint64_t Master::max_frame_us() const {
    return static_cast<uint64_t>(kMaxWire) * byte_time_us(wire_.bit_rate());
}

bool Master::frame_arriving(uint64_t now_us) const {
    // A frame is STILL ARRIVING only if all three hold:
    //  (a) in_frame(): an accumulation is open (a FLAG has been seen). On its own this stays
    //      true forever on a quiet wire, so it is paired with
    //  (b) byte cadence: within a frame each byte starts exactly where the previous ended
    //      (spec.md "transmission-time model"), so if more than one byte time has passed since
    //      the last byte was received, the transmitter has stopped and nothing is in flight.
    //      last_rx_us_ is the END of that byte, so the next byte of a live frame would arrive
    //      at last_rx_us_ and be drained by any poll at or after it — one byte time of slack.
    //  (c) a time cap: less than one worst-case frame (kMaxWire byte times) has elapsed since
    //      the opening FLAG. (b) alone is only a bound in BYTES, because this predicate is
    //      sampled at poll() instants against the MOST RECENT byte: a station that puts one
    //      byte on the wire at every poll instant satisfies (b) at every observation, and the
    //      hold then ends only at Discard::TooLong — kMaxWire bytes, i.e. ~kMaxWire poll
    //      periods (284 ms at T_poll, 0.5 % duty; PR #137 red-team, HIGH). No legitimate
    //      response can outlast the cap: it is at most kMaxWire bytes long and contiguous, so
    //      it closes >= 2 byte times before it (SC-008's achievable maximum is 140 wire bytes).
    //      For a contiguous stall the cap and the cadence bound release at the same instant
    //      (deadline + max_frame when the FLAG arrived at deadline - 1), so the cap is the
    //      MAXIMUM hold at any cadence, not a change to the contiguous case — true by
    //      construction of the conjunction; the exact boundary is pinned by test_link_master
    //      "the time cap on the T_resp hold is exactly resp_open + kMaxWire byte times".
    if (!deframer_.in_frame())
        return false;
    return now_us <= last_rx_us_ + byte_time_us(wire_.bit_rate()) &&
           now_us <= resp_open_us_ + max_frame_us();
}

void Master::fire_pending(uint64_t now_us) {
    if (!(open_ && sub_phase_ == SubPhase::PendingTransmit))
        return;
    // The bus must be idle for >= T_gap before the engine transmits (data-model.md §4 "Gap";
    // trunk §3). The instant computed at defer time is only a LOWER bound: activity the drain
    // loop has since seen pushes it out, re-evaluated on every poll (PR #137 red-team, MEDIUM:
    // the deferred instant was computed once and never pushed back, so a frame arriving during
    // the gap let the master transmit over an in-flight frame / 0 µs after a discarded one's
    // last byte). Never transmit with less than T_gap of idle after the last byte received.
    //
    // That single clause is also what keeps the engine off a frame that is still ARRIVING
    // (PR #137 red-team, HIGH: the master drove the line into a frame another station had
    // already started). No second in-flight branch is needed here, and adding one would be an
    // unkillable duplicate (red-team M5): every received byte sets last_activity_ to that
    // BYTE'S END, and within a frame bytes are contiguous (spec.md "Assumptions", transmission-
    // time model: each byte starts where the previous one ended) — at any poll instant `now`,
    // the last byte drained is the one with start <= now < start + byte_time, so its end is
    // strictly greater than `now`. Hence last_activity_ > now, and the deferred instant
    // (last_activity_ + T_gap) stays ahead of `now` for as long as bytes keep coming, at any
    // CONSTANT bit rate. Proved by construction from the drain loop's unconditional recording
    // of EVERY byte due at `now` (the loop never exits early — it did once, on the attempt-
    // ending byte, and the argument was false for exactly the bytes left behind it: PR #137
    // red-team @3a15d29, HIGH) plus that contiguity — not merely by the tests that exercise
    // it. Two things it does NOT cover: a begin() with no poll(now) immediately before it sees
    // no bytes at all (master.hpp begin(); #138), and a rate change mid-frame. The constancy
    // matters: each byte's end is computed at DRAIN time from the rate then in force, so a
    // set_bit_rate() upwards while a frame put on the wire at the old rate is still arriving makes
    // those bytes appear to end early, and the perceived holes can let this transmit out inside
    // that frame (PR #137 red-team @40355cf, LOW — a byte's own duration is not something ByteWire
    // reports; open in #138 / docs/OPEN-QUESTIONS.md 2026-09-06 "rate change mid-stream").
    // Every protection claim below is therefore stated for a constant rate.
    uint64_t want_us = deadline_;
    // mutant-ok(equivalent, cxx_gt_to_ge): at equality the branch re-stores want_us's own value.
    if (has_last_activity_ && last_activity_ + omgp::TRUNK_T_gap_us > want_us)
        want_us = last_activity_ + omgp::TRUNK_T_gap_us;

    // ...but the push-out is BOUNDED (PR #137 red-team, HIGH: a station holding bytes on the
    // wire continuously pushed the instant out on every poll — no attempt, no retry, no
    // outcome, busy() true permanently). trunk §3: the host is the ONLY initiator ("no
    // multi-master arbitration, no CSMA, no token"), and the >= T_gap of idle it owes is
    // between ITS OWN transactions (FR-010's head clause). Deferring for activity that is not
    // the host's own is a courtesy on top of that, and it ends one worst-case frame plus T_gap
    // past the instant the transmission was first deferred to (defer_origin_us_): long enough
    // for any single frame already on the wire at that instant to finish AND receive its full
    // gap, and no longer. Past the cap the engine transmits on schedule and the transaction
    // "fails or succeeds on its own merits" (spec.md Edge Cases, "Babble") — a station still
    // occupying the wire then is a §3 violator, and a trunk that stays jammed is found by §7's
    // failure accounting on the REAL outcomes (every node failing -> BUS_FAULT), never by this
    // engine synthesising a Failed from how busy the bus looked at its poll instants. Three
    // earlier revisions did exactly that and each was falsified: elapsed time abandoned idle-bus
    // transactions at the superframe cadence; a stale deadline_ made the bound unreachable at
    // that cadence; a single-instant "gap still denied" was held true forever by one stray byte
    // per superframe. The engine observes and transmits only at poll() instants, so ANY
    // inference "the bus is unusable" drawn from them is spoofable or a false positive, and any
    // Failed it produces is read by the caller as a NODE outcome.
    //
    // What the cap establishes, each by construction from this function's contents (at a
    // constant bit rate — see above):
    //  - liveness: after this statement deadline_ <= cap_us always, so any poll at or after
    //    cap_us transmits, whatever the cadence and whatever is on the wire;
    //  - the cap can only LOWER the instant, so it never re-creates the elapsed-time misfire
    //    on an idle bus (there want_us is already <= cap_us and is transmitted at unchanged);
    //  - protection: a frame whose start bit lands at or before defer_origin_us_ + T_gap is
    //    never transmitted over (its last byte ends by defer_origin + T_gap + max_frame, i.e.
    //    no later than the cap), and one starting at or before defer_origin_us_ additionally
    //    gets its full T_gap.
    //    Any frame starting later than that is a §3 violator on every path into this state
    //    (a retry's origin is already > T_turn_max past the polled node's window; a fresh
    //    begin()'s origin is >= T_gap after the last byte seen).
    // max_frame_us() is kMaxWire (142) byte times — the codec's sizing bound, deliberately
    // above SC-008's 140-byte achievable worst case — recomputed here so a set_bit_rate()
    // during the deferral is honoured at once (PR #137 review, LOW).
    const uint64_t cap_us = defer_origin_us_ + max_frame_us() + omgp::TRUNK_T_gap_us;
    // `a > b ? b : a` and `a >= b ? b : a` both compute min(a, b) for ALL inputs — they pick
    // different branches only when a == b, where both branches yield the same value. (A label
    // governs only the line directly beneath it — tools/mutate_report.py — so it comes last.)
    // mutant-ok(equivalent, cxx_gt_to_ge): min(a, b) either way; at a == b both branches agree.
    if (want_us > cap_us)
        want_us = cap_us;
    deadline_ = want_us;
    if (now_us >= deadline_)
        do_transmit(now_us);
}

void Master::end_attempt(uint64_t last_activity_us, MasterEvent::Reason reason,
                         MasterEvent& event) {
    AddrStats& s = stats_[dst_];
    if (reason == MasterEvent::Timeout)
        s.timeouts++;
    else
        s.crc_failures++;
    // Take the LATEST of what was already recorded (e.g. a discarded frame's own last byte,
    // set by poll()'s drain loop just before this call) and this conclusion's own instant —
    // never rewind (data-model.md §4 "Gap": last_activity is "the last byte of a discarded
    // frame, OR the timeout instant", not whichever happens to run last in the code; PR #137
    // review/red-team, MEDIUM). An unconditional overwrite here made that clause dead code:
    // a frame discarded just past deadline_ recorded its own (later) end, only for this
    // unconditional assignment to immediately rewind it back to the (earlier) deadline_.
    // mutant-ok(equivalent, cxx_gt_to_ge): at equality the branch re-stores the same value.
    if (last_activity_us > last_activity_)
        last_activity_ = last_activity_us;
    has_last_activity_ = true;

    if (attempt_count_ < 1u + omgp::TRUNK_retries) {
        // Retries remain: schedule the next attempt, gap-deferred from this activity
        // (data-model.md §4 "Gap") — never terminal yet.
        sub_phase_ = SubPhase::PendingTransmit;
        deadline_ = last_activity_ + omgp::TRUNK_T_gap_us;
        defer_origin_us_ = deadline_; // as in begin() — see the member
        event.kind = MasterEvent::None;
    } else {
        // FR-011a: "transactions" is counted once, at do_transmit()'s first attempt (PR
        // #137 review, MEDIUM) — not concluded a second time here.
        event.kind = MasterEvent::Failed;
        event.reason = reason;
        // cxx_assign_const substitutes the type's zero value for an assignment's RHS
        // (0/false/nullptr — see `written = 0`'s label in do_transmit()): open_ is already
        // being assigned `false`, its own zero value, so the mutated statement is
        // byte-for-byte identical to this one — nothing for any test to distinguish.
        // mutant-ok(equivalent, cxx_assign_const): the mutation and the original coincide.
        open_ = false;
    }
}

MasterEvent Master::poll(uint64_t now_us) {
    MasterEvent event{};

    uint8_t byte;
    uint64_t start_us;
    // FR-011: drained unconditionally, not just while AwaitResponse — a frame arriving
    // while no transaction is open, or while one is gap-deferred between attempts, is
    // still discarded and counted rather than left to overrun a real UART FIFO or bleed
    // into whichever window opens next (PR #137 review, MEDIUM).
    //
    // And drained to EXHAUSTION: this loop never `break`s, not even on the byte that ends an
    // attempt (an in-window CRC failure, or the Answered delivery). It used to, and bytes
    // already due behind that byte stayed unread — so fire_pending() at the bottom of this
    // very poll(), or a begin() the caller issued on the terminal event (the intended F3
    // loop), judged the bus idle from a last_activity_ that predated them and transmitted
    // INTO a frame another station had started in the meantime (PR #137 red-team @3a15d29,
    // HIGH). fire_pending()'s protection argument rests on this loop recording every received
    // byte's end; an early exit made that recording conditional and the argument false. The
    // attempt-ending byte's outcome survives the rest of the drain because every acceptance
    // check below is gated on `awaiting`, re-evaluated per byte and false once the attempt
    // has ended: later bytes can only be recorded as activity and discarded/counted.
    while (wire_.receive(byte, start_us)) {
        const bool is_flag = (byte == omgp::TRUNK_flag_byte);
        // The instant that opened the accumulation now forming, captured BEFORE this
        // byte's own update (mirrors mock_wire.cpp's transmit(): a frame delivered by
        // this very (closing) FLAG byte must see the PREVIOUS (opening) FLAG's instant).
        const uint64_t frame_open_us = resp_open_us_;
        const uint32_t bad_crc_before =
            deframer_.stats().discarded[static_cast<size_t>(Discard::BadCrc)];

        FrameView view{};
        const bool delivered = deframer_.feed(byte, view);
        if (is_flag)
            resp_open_us_ = start_us;

        const uint32_t bad_crc_after =
            deframer_.stats().discarded[static_cast<size_t>(Discard::BadCrc)];
        const uint64_t byte_end_us = start_us + byte_time_us(wire_.bit_rate());
        const bool awaiting = open_ && sub_phase_ == SubPhase::AwaitResponse;
        const bool in_window = frame_open_us >= window_start_us_ && frame_open_us < deadline_;

        // EVERY byte off the wire is bus activity, whatever the Deframer then does with it
        // (deliver, discard, accumulate mid-frame, or ignore while Hunting): FR-010 counts the
        // "last byte transmitted or received", and data-model.md §4 "Gap" measures idle from
        // it. Recorded once, here, rather than in each outcome branch — the per-branch
        // recording missed bytes the Deframer neither delivers nor counts as a discard (PR
        // #137 red-team, MEDIUM), which let a begin()/retry start with less than T_gap of real
        // idle after a partial or ignored burst. last_rx_us_ additionally feeds
        // frame_arriving()'s cadence test, so it tracks received bytes ONLY.
        last_rx_us_ = byte_end_us;
        // Not equivalent to an unconditional store: a byte drained AFTER a rate rise can end
        // before one drained at the slower rate (test "a rate rise between two polls does not
        // rewind last_activity"). Only the equality case is behaviourally identical:
        // mutant-ok(equivalent, cxx_gt_to_ge): at equality the branch re-stores the same value.
        if (byte_end_us > last_activity_)
            last_activity_ = byte_end_us;
        has_last_activity_ = true;

        if (bad_crc_after > bad_crc_before) {
            if (awaiting && in_window) {
                // trunk §7: a CRC-failed response IN the window is a failure — ends this
                // attempt at once, no need to wait out the remaining T_resp window. The
                // drain goes on (see the loop comment): `awaiting` is false from here.
                end_attempt(byte_end_us, MasterEvent::CrcFailed, event);
                continue;
            }
            // A CRC-bad frame outside any open attempt's own window (or with no
            // transaction open at all) is not attributable to a specific dst_ — the
            // Deframer's own stats() already counts it (PR #137 review, MEDIUM: this branch
            // used to end the attempt unconditionally, even for a stray CRC failure that had
            // nothing to do with the open transaction). Its bus activity is already recorded
            // by the unconditional last_activity_/last_rx_us_ update above.
            continue;
        }
        if (!delivered)
            continue; // accumulating mid-frame, or structurally discarded: activity recorded above

        const FrameFields& f = view.f;
        if (awaiting && f.response && f.src == dst_ && f.dst == host_addr_ && f.seq == seq_ &&
            in_window) {
            // As at begin()'s own memcpy guard above — a copy of zero bytes is
            // unobservable, and ev.response.len == 0 tells the caller not to read
            // response_buf_ beyond it.
            // mutant-ok(equivalent, cxx_gt_to_ge): either way, no observable difference.
            if (f.len > 0)
                std::memcpy(response_buf_, f.payload, f.len);
            event.kind = MasterEvent::Answered;
            event.response =
                FrameFields{f.dst, f.src, f.response, f.retry, f.seq, f.len, response_buf_};
            // cxx_assign_const substitutes the type's zero value for an assignment's RHS
            // (0/false/nullptr — see `written = 0`'s label above): open_ is already being
            // assigned `false`, its own zero value, so the mutated statement is
            // byte-for-byte identical to this one. There is nothing for any test to
            // distinguish (concurrent PR #137 review-fix pass, cross-checked here).
            // mutant-ok(equivalent, cxx_assign_const): the mutation and the original coincide.
            open_ = false;
            continue; // this byte's bus activity is already recorded above; keep draining
        }
        // Wrong src/dst/seq/response-bit, a matching frame whose opening instant fell
        // outside the window, or no transaction open at all: discarded silently (trunk
        // §4), counted (FR-011), and does not end the attempt (data-model.md §4). Charged
        // to dst_ only while dst_'s own response window is running — spec US2 AC6 scopes
        // the per-destination counter to frames "arriving during an open response window".
        // Otherwise (gap-deferred before a first or retried transmission, or no transaction
        // at all) there is no window to attribute it to, so the frame's own claimed source
        // is charged instead (PR #137 review @3a15d29, LOW; bounds-checked: an intact
        // frame's src is wire-derived and can claim any byte 0x00..0xFE). A claimed src of
        // 0x10..0xFE has no AddrStats slot and is counted NOWHERE — FR-011 says every discard
        // is counted, FR-011a's block is per address; recorded, not resolved here
        // (docs/OPEN-QUESTIONS.md 2026-09-06 "claimed src is out of range"; #138 item 3).
        if (awaiting)
            stats_[dst_].discards++;
        // `<=` here differs only at f.src == kAddrCount, where it writes one AddrStats past the
        // table — undefined behaviour with no defined observable to assert on. The native
        // preset's ASan catches it (test "an unsolicited frame while idle whose claimed source
        // is exactly kAddrCount"); the mutation build runs with sanitizers off (tools/mutate.sh),
        // so that case cannot kill the mutant there.
        // mutant-ok(accepted, cxx_lt_to_le): only an out-of-bounds write differs; ASan-only.
        else if (f.src < kAddrCount)
            stats_[f.src].discards++;
        // This frame's bus activity (for the T_gap rule, data-model.md §4 "Gap") is already
        // recorded by the unconditional last_activity_ update at the top of the loop, which
        // covers every received byte rather than only the outcomes that used to have their own
        // recording (PR #137 review/red-team, MEDIUM).
    }

    // trunk §3: the timeout gates the START BIT, not full delivery — "if the host sees no
    // start bit within T_resp, the request has failed". A frame that opened inside the
    // window (deframer_.in_frame(), with resp_open_us_ still inside [window_start_us_,
    // deadline_)) must be allowed to finish arriving however long that takes, not be
    // abandoned mid-flight the instant the window's nominal end passes (PR #137
    // review/red-team, HIGH: this previously timed out any response whose payload was
    // long enough that its closing FLAG arrived after tx_end + T_resp, which excludes
    // every payload above ~11 bytes at TRUNK_bit_rate and all of them at the fallback rate).
    // BOUNDED (PR #137 review/red-team, MEDIUM): "allowed to finish" is not "forever".
    // frame_arriving() is true from the opening FLAG onward while bytes keep coming at byte
    // cadence, goes false about one byte time after they stop, and in any case goes false one
    // worst-case frame (max_frame_us()) after the opening FLAG (PR #137 red-team @9547634,
    // HIGH: cadence alone is sampled only at poll instants, so it bounds the hold in BYTES,
    // not time). So a genuine response finishes however long it legitimately takes (always
    // inside the cap), a node that opened a frame then stalled (a trunk §7 failure class)
    // stops holding the timeout off almost immediately, one that trickles a byte per poll
    // stops holding it off at resp_open + max_frame at the latest, and a frame that merely
    // CLOSED in the window (its closing FLAG is also the next frame's opening delimiter) no
    // longer suppresses the timeout at all once the wire goes quiet — the forever-wedge this
    // PR opened with.
    const bool frame_pending_in_window =
        frame_arriving(now_us) && resp_open_us_ >= window_start_us_ && resp_open_us_ < deadline_;
    if (open_ && sub_phase_ == SubPhase::AwaitResponse && event.kind == MasterEvent::None &&
        now_us >= deadline_ && !frame_pending_in_window) {
        end_attempt(deadline_, MasterEvent::Timeout, event);
    }

    fire_pending(now_us);
    return event;
}

} // namespace link
} // namespace omgp
