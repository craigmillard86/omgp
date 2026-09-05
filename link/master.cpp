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
    if (dst == 0xFF) // literal-ok: trunk §5 reserved broadcast address, not an L3 event code
        return Status::ReservedAddress;
    // Master's own guard, beyond encode_frame's: next_seq_/stats_ are kAddrCount-entry
    // tables indexed directly by dst (data-model.md §4 "Sequence", §8 "Statistics"), so any
    // dst outside that range must be refused here even though encode_frame and the Deframer
    // let 0x10..0xFE through as syntactically valid trunk addresses (PR #137 review, HIGH:
    // begin(0x20, ...) previously wrote next_seq_[0x20]/stats_[0x20], past both tables).
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
    const uint64_t gap_elapsed_at = has_last_activity_ ? last_activity_ + omgp::TRUNK_T_gap_us : now;
    // mutant-ok(equivalent): proved by construction, not by mutate.sh (blocked in this
    // sandbox) — at gap_elapsed_at == now both arms of any relational flip here yield the
    // same value (now == gap_elapsed_at), so this is max(gap_elapsed_at, now) either way.
    deadline_ = gap_elapsed_at > now ? gap_elapsed_at : now;
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

void Master::fire_pending(uint64_t now_us) {
    // deadline_ is always <= now_us here (the guard above), so this is a no-op given the
    // fix in begin() above; kept as a second, independent guard against ever transmitting
    // at a backdated instant (PR #137 review, HIGH) should deadline_ ever again be computed
    // stale by some future caller of fire_pending.
    // mutant-ok(equivalent): proved by construction, not by mutate.sh (blocked in this
    // sandbox) — only reached when now_us >= deadline_ (the guard on the line above), so
    // now_us > deadline_ picks now_us and now_us == deadline_ picks deadline_, which then
    // equals now_us anyway: every relational flip of this comparison yields now_us either
    // way, for every input this line can ever see.
    if (open_ && sub_phase_ == SubPhase::PendingTransmit && now_us >= deadline_)
        do_transmit(now_us > deadline_ ? now_us : deadline_);
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
        event.kind = MasterEvent::None;
    } else {
        // FR-011a: "transactions" is counted once, at do_transmit()'s first attempt (PR
        // #137 review, MEDIUM) — not concluded a second time here.
        event.kind = MasterEvent::Failed;
        event.reason = reason;
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

        if (bad_crc_after > bad_crc_before) {
            if (awaiting && in_window) {
                // trunk §7: a CRC-failed response IN the window is a failure — ends this
                // attempt at once, no need to wait out the remaining T_resp window.
                end_attempt(byte_end_us, MasterEvent::CrcFailed, event);
                break;
            }
            // A CRC-bad frame outside any open attempt's own window (or with no
            // transaction open at all) is not attributable to a specific dst_ — the
            // Deframer's own stats() already counts it — but it is still real bus
            // activity for T_gap purposes (data-model.md §4 "Gap"; PR #137 review,
            // MEDIUM: this branch used to end the attempt unconditionally, even for a
            // stray CRC failure that had nothing to do with the open transaction).
            if (byte_end_us > last_activity_)
                last_activity_ = byte_end_us;
            has_last_activity_ = true;
            continue;
        }
        if (!delivered)
            continue;

        const FrameFields& f = view.f;
        if (awaiting && f.response && f.src == dst_ && f.dst == host_addr_ &&
            f.seq == seq_ && in_window) {
            // As at begin()'s own memcpy guard above — a copy of zero bytes is
            // unobservable, and ev.response.len == 0 tells the caller not to read
            // response_buf_ beyond it.
            // mutant-ok(equivalent, cxx_gt_to_ge): either way, no observable difference.
            if (f.len > 0)
                std::memcpy(response_buf_, f.payload, f.len);
            event.kind = MasterEvent::Answered;
            event.response =
                FrameFields{f.dst, f.src, f.response, f.retry, f.seq, f.len, response_buf_};
            open_ = false;
            last_activity_ = byte_end_us;
            has_last_activity_ = true;
            break;
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
        // Overwritten by whichever terminal path (Answered above, or CrcFailed/Timeout via
        // end_attempt) concludes this transaction, before it is ever read by a later
        // mutant-ok(equivalent, cxx_assign_const): begin()'s gap check — see end_attempt().
        last_activity_ = byte_end_us;
        // Same reasoning as last_activity_ above — every terminal path sets this true
        // mutant-ok(equivalent, cxx_assign_const): again from its own evidence before conclusion.
        has_last_activity_ = true;
    }

    // trunk §3: the timeout gates the START BIT, not full delivery — "if the host sees no
    // start bit within T_resp, the request has failed". A frame that opened inside the
    // window (deframer_.in_frame(), with resp_open_us_ still inside [window_start_us_,
    // deadline_)) must be allowed to finish arriving however long that takes, not be
    // abandoned mid-flight the instant the window's nominal end passes (PR #137
    // review/red-team, HIGH: this previously timed out any response whose payload was
    // long enough that its closing FLAG arrived after tx_end + T_resp, which excludes
    // every payload above ~11 bytes at TRUNK_bit_rate and all of them at the fallback rate).
    const bool frame_pending_in_window =
        deframer_.in_frame() && resp_open_us_ >= window_start_us_ && resp_open_us_ < deadline_;
    if (open_ && sub_phase_ == SubPhase::AwaitResponse && event.kind == MasterEvent::None &&
        now_us >= deadline_ && !frame_pending_in_window) {
        end_attempt(deadline_, MasterEvent::Timeout, event);
    }

    fire_pending(now_us);
    return event;
}

} // namespace link
} // namespace omgp
