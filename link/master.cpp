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
    // memcpy(payload_, payload, 0) has no observable effect either way — do_transmit only
    // ever reads back the first len_ bytes of payload_ (via FrameFields fed into
    // encode_frame's `for (i < f.len)` loop).
    // mutant-ok(equivalent, cxx_gt_to_ge): a copy of zero bytes is unobservable either way.
    if (len_ > 0)
        std::memcpy(payload_, payload, len_);
    attempt_count_ = 0;
    open_ = true;

    // A new transaction is itself subject to the T_gap rule (data-model.md §4 "Gap"): the
    // engine, not the caller, guarantees the gap, deferring transmission if begin() is
    // called too soon after the last activity on the bus.
    const uint64_t now = clock_.now_us();
    sub_phase_ = SubPhase::PendingTransmit;
    // trunk §3 requires only ">= T_gap of bus idle", not "exactly T_gap": once the gap has
    // already elapsed by the time begin() is called (the engine's ordinary operating mode,
    // e.g. a superframe scheduler invoking begin() once per T_poll >> T_gap), the deferred
    // instant must be `now`, never a stale past instant computed from an old
    // last_activity_ (PR #137 review, HIGH - see fire_pending()'s own comment).
    const uint64_t gap_elapsed_at =
        has_last_activity_ ? last_activity_ + omgp::TRUNK_T_gap_us : now;
    // mutant-ok(equivalent, cxx_gt_to_ge): `a > b ? a : b` and `a >= b ? a : b` both compute
    // max(a, b) for ALL inputs — they choose different branches only when a == b, and both
    // branches yield the same value there. Proved by construction, not by mutate.sh (blocked
    // in this sandbox). (Rule 11 / PR #137 review, LOW: an earlier wording argued only the
    // a == b case, which on its own does not establish equivalence over the whole domain.)
    deadline_ = gap_elapsed_at > now ? gap_elapsed_at : now;
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
    // Validated at begin() (length/address); cannot fail here.
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
    // in_frame(): an accumulation is open (a FLAG has been seen). On its own this stays true
    // forever on a quiet wire, so it is paired with byte cadence: within a frame each byte
    // starts exactly where the previous ended, so if more than one byte time has passed since
    // the last byte was received, the transmitter has stopped and nothing is in flight.
    // last_rx_us_ is the END of that byte, so the next byte of a live frame would arrive at
    // last_rx_us_ and be drained by any poll at or after it — one full byte time of slack.
    if (!deframer_.in_frame())
        return false;
    return now_us <= last_rx_us_ + byte_time_us(wire_.bit_rate());
}

void Master::fire_pending(uint64_t now_us) {
    if (!(open_ && sub_phase_ == SubPhase::PendingTransmit))
        return;
    // The bus must be idle for >= T_gap before the engine transmits (data-model.md §4 "Gap";
    // trunk §3). The instant computed at defer time is only a LOWER bound — two things push
    // it out, both re-evaluated on every poll (PR #137 red-team, MEDIUM: the deferred instant
    // was computed once and never pushed back, so a frame arriving during the gap let the
    // master transmit over an in-flight frame / 0 µs after a discarded one's last byte):
    //  (1) a frame the drain loop already delivered/discarded during the deferral advanced
    //      last_activity_ — never transmit with less than T_gap of idle after it;
    if (has_last_activity_ && last_activity_ + omgp::TRUNK_T_gap_us > deadline_)
        deadline_ = last_activity_ + omgp::TRUNK_T_gap_us;
    // That single clause is also what keeps the engine off a frame that is still ARRIVING
    // (PR #137 red-team, HIGH: the master drove the line into a frame another station had
    // already started). No second in-flight branch is needed here, and adding one would be an
    // unkillable duplicate (red-team M5): every received byte sets last_activity_ to that
    // BYTE'S END, and within a frame bytes are contiguous — at any poll instant `now`, the
    // last byte drained is the one with start <= now < start + byte_time, so its end is
    // strictly greater than `now`. Hence last_activity_ > now, and the deferred instant
    // (last_activity_ + T_gap) stays ahead of `now` for as long as bytes keep coming, at any
    // bit rate. Proved by construction from the drain loop's unconditional recording plus
    // contiguous framing (trunk §4) — not merely by the tests that exercise it.
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
        if (byte_end_us > last_activity_)
            last_activity_ = byte_end_us;
        has_last_activity_ = true;

        if (bad_crc_after > bad_crc_before) {
            if (awaiting && in_window) {
                // trunk §7: a CRC-failed response IN the window is a failure — ends this
                // attempt at once, no need to wait out the remaining T_resp window.
                end_attempt(byte_end_us, MasterEvent::CrcFailed, event);
                break;
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
            break; // this byte's bus activity is already recorded above
        }
        // Wrong src/dst/seq/response-bit, a matching frame whose opening instant fell
        // outside the window, or no transaction open at all: discarded silently (trunk
        // §4), counted (FR-011), and does not end the attempt (data-model.md §4). A
        // transaction in progress (open_, including gap-deferred between attempts) is
        // charged to its own dst_; fully idle, there is no dst_ to charge, so the
        // frame's own claimed source is used instead (bounds-checked: an intact frame's
        // src is wire-derived and can claim any byte 0x00..0xFE).
        if (open_)
            stats_[dst_].discards++;
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
    // BOUNDED (PR #137 review/red-team, MEDIUM): "allowed to finish" is not "forever", and the
    // bound is byte CADENCE, not a fixed worst-case-frame cap. frame_arriving() is true from
    // the opening FLAG onward while bytes keep coming, and goes false about one byte time
    // after they stop — so a genuine response finishes however long it legitimately takes, a
    // node that opened a frame then stalled (a trunk §7 failure class) stops holding the
    // timeout off almost immediately, and a frame that merely CLOSED in the window (its
    // closing FLAG is also the next frame's opening delimiter) no longer suppresses the
    // timeout at all once the wire goes quiet — the forever-wedge this PR opened with.
    const bool frame_pending_in_window =
        frame_arriving(now_us) && resp_open_us_ >= window_start_us_ && resp_open_us_ < deadline_;
    if (open_ && sub_phase_ == SubPhase::AwaitResponse && event.kind == MasterEvent::None &&
        now_us >= deadline_ && !frame_pending_in_window) {
        end_attempt(deadline_, MasterEvent::Timeout, event);
    }

    // Babble bound (PR #137 red-team, HIGH; trunk §7 names babble as a failure mode).
    // fire_pending() pushes a deferred transmission out for ongoing bus activity so the engine
    // never drives the line over an arriving frame — but a station holding bytes on the wire
    // continuously would otherwise push it out forever: no attempt, no retry, no outcome, and
    // busy() true permanently. Past defer_cap_us_ (one worst-case frame beyond the instant this
    // transmission was originally deferred to) the bus is not "briefly busy with a frame", it is
    // unusable, so the transaction concludes rather than hanging.
    //
    // The test is that the bus is STILL DENYING a gap window, not that time has passed
    // (PR #137 red-team, HIGH — an elapsed-time-only guard could not tell "a station has held
    // the wire for a frame time" from "nobody called poll() for a while", and since
    // TRUNK_T_poll_us (2000) exceeds max_frame_us (1420), the documented superframe cadence
    // (trunk §6) abandoned EVERY gap-deferred transaction and retry on a completely idle wire):
    //   now_us >= defer_cap_us   — the whole wait budget really has elapsed, AND
    //   now_us <  deadline_      — the transmit instant is STILL in the future, i.e. activity
    //                              keeps pushing it out and no T_gap window has been offered.
    // On an idle bus deadline_ is never pushed, so now_us >= deadline_ and this cannot fire —
    // fire_pending() below transmits instead, however late the poll arrives. That is exactly
    // the rule docs/OPEN-QUESTIONS.md (2026-09-05, babble) recommended: conclude only when the
    // bus has not offered a T_gap window within the budget.
    //
    // Terminal, not a retry: this attempt never reached the wire (attempt_count_ is untouched,
    // so `transactions` — which FR-011a counts at first transmission — is correctly not
    // counted either), and retrying into a bus that is still babbling would only extend the
    // hang. Reported as Failed{Timeout} because that is the outcome the engine has today;
    // distinguishing "the node did not answer" from "the trunk was unusable" needs a bus-fault
    // outcome, which is a spec question recorded in docs/OPEN-QUESTIONS.md and tracked in #138.
    //
    // No per-node counter is charged: the request never reached dst_, so booking it a timeout
    // would mark an innocent node on a bus condition it had no part in — three of those feed
    // trunk §7's "3 consecutive failed transactions -> SUSPECT" (PR #137 review, MEDIUM).
    // Recording the bus condition itself belongs with the bus-fault outcome (#138).
    const uint64_t defer_cap_us = defer_origin_us_ + max_frame_us();
    if (open_ && sub_phase_ == SubPhase::PendingTransmit && event.kind == MasterEvent::None &&
        now_us >= defer_cap_us && now_us < deadline_) {
        if (now_us > last_activity_)
            last_activity_ = now_us;
        has_last_activity_ = true;
        event.kind = MasterEvent::Failed;
        event.reason = MasterEvent::Timeout;
        open_ = false;
    }

    fire_pending(now_us);
    return event;
}

} // namespace link
} // namespace omgp
