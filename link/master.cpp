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

    dst_ = dst;
    seq_ = next_seq_[dst];
    next_seq_[dst] = static_cast<uint8_t>((next_seq_[dst] + 1) & 0x0F);
    len_ = static_cast<uint8_t>(len);
    if (len_ > 0)
        std::memcpy(payload_, payload, len_);
    attempt_count_ = 0;
    open_ = true;

    // A new transaction is itself subject to the T_gap rule (data-model.md §4 "Gap"): the
    // engine, not the caller, guarantees the gap, deferring transmission if begin() is
    // called too soon after the last activity on the bus.
    const uint64_t now = clock_.now_us();
    sub_phase_ = SubPhase::PendingTransmit;
    deadline_ = has_last_activity_ ? last_activity_ + omgp::TRUNK_T_gap_us : now;
    fire_pending(now);
    return Status::Ok;
}

void Master::do_transmit(uint64_t at_us) {
    const bool retry = attempt_count_ > 0;
    const FrameFields f{dst_, host_addr_, /*response=*/false, retry, seq_, len_, payload_};
    uint8_t buf[kMaxWire];
    size_t written = 0;
    // Validated at begin() (length/address); cannot fail here.
    encode_frame(f, buf, sizeof buf, written);
    const uint64_t tx_end = wire_.transmit(buf, written, at_us);

    if (attempt_count_ > 0)
        stats_[dst_].retries++;
    ++attempt_count_;
    sub_phase_ = SubPhase::AwaitResponse;
    deadline_ = tx_end + omgp::TRUNK_T_resp_us;
}

void Master::fire_pending(uint64_t now_us) {
    if (open_ && sub_phase_ == SubPhase::PendingTransmit && now_us >= deadline_)
        do_transmit(deadline_);
}

void Master::end_attempt(uint64_t last_activity_us, MasterEvent::Reason reason,
                         MasterEvent& event) {
    AddrStats& s = stats_[dst_];
    if (reason == MasterEvent::Timeout)
        s.timeouts++;
    else
        s.crc_failures++;
    last_activity_ = last_activity_us;
    has_last_activity_ = true;

    if (attempt_count_ < 1u + omgp::TRUNK_retries) {
        // Retries remain: schedule the next attempt, gap-deferred from this activity
        // (data-model.md §4 "Gap") — never terminal yet.
        sub_phase_ = SubPhase::PendingTransmit;
        deadline_ = last_activity_ + omgp::TRUNK_T_gap_us;
        event.kind = MasterEvent::None;
    } else {
        s.transactions++;
        event.kind = MasterEvent::Failed;
        event.reason = reason;
        open_ = false;
    }
}

MasterEvent Master::poll(uint64_t now_us) {
    MasterEvent event{};

    uint8_t byte;
    uint64_t start_us;
    while (open_ && sub_phase_ == SubPhase::AwaitResponse && wire_.receive(byte, start_us)) {
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

        if (bad_crc_after > bad_crc_before) {
            // trunk §7: a CRC-failed response is a failure — ends this attempt at once,
            // no need to wait out the remaining T_resp window.
            end_attempt(byte_end_us, MasterEvent::CrcFailed, event);
            break;
        }
        if (!delivered)
            continue;

        const FrameFields& f = view.f;
        const bool matches = f.response && f.src == dst_ && f.dst == host_addr_ && f.seq == seq_;
        if (matches && frame_open_us < deadline_) {
            AddrStats& s = stats_[dst_];
            s.transactions++;
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
        // Wrong src/dst/seq/response-bit, or a matching frame whose opening instant fell
        // outside the window: discarded silently (trunk §4), counted, and does not end
        // the attempt (data-model.md §4).
        stats_[dst_].discards++;
        last_activity_ = byte_end_us;
        has_last_activity_ = true;
    }

    if (open_ && sub_phase_ == SubPhase::AwaitResponse && event.kind == MasterEvent::None &&
        now_us >= deadline_) {
        end_attempt(deadline_, MasterEvent::Timeout, event);
    }

    fire_pending(now_us);
    return event;
}

} // namespace link
} // namespace omgp
