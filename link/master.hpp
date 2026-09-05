// OMGP trunk L2 — Master transaction engine: trunk §3 (media access), §7 (retry rule).
// Contract: specs/002-trunk-link-layer/contracts/link-cpp.md "Master engine"; data model:
// specs/002-trunk-link-layer/data-model.md §4 "Transaction (Master)", §8 "Statistics".
// Makes tests/unit/test_link_master.cpp (T029) pass. Embedded path: C++17, no exceptions,
// no RTTI, no heap — one open transaction's state plus fixed per-address statistics.
#pragma once

#include "link/byte_wire.hpp"
#include "link/clock.hpp"
#include "link/frame.hpp"
#include "link/link_types.hpp"

namespace omgp {
namespace link {

// One outcome of a poll() call (data-model.md §4 "Events"); each transaction yields
// exactly one terminal event (Answered or Failed).
struct MasterEvent {
    enum Kind : uint8_t { None, Answered, Failed } kind = None;
    // Valid only when kind == Answered: payload points into Master's own buffer and is
    // valid only until the next poll() call (contracts/link-cpp.md "Master engine").
    FrameFields response{};
    enum Reason : uint8_t { Timeout, CrcFailed } reason = Timeout;
};

// Drives one destination-addressed request/response transaction at a time over a
// ByteWire (trunk §3 media access): per-destination sequencing, retry, the T_resp/T_gap
// timing rules, and statistics (data-model.md §4/§8).
class Master {
  public:
    Master(ByteWire& wire, Clock& clock, uint8_t host_addr = omgp::ADDR_host);

    // Busy if a transaction is already open; PayloadTooLong/ReservedAddress as
    // encode_frame (contracts/link-cpp.md). An accepted transaction may still defer its
    // transmission to satisfy T_gap (data-model.md §4 "Gap") — busy() is already true
    // when this returns Ok either way.
    Status begin(uint8_t dst, const uint8_t* payload, size_t len);

    // The only receive path (analysis F1): drains ByteWire::receive() into the engine's
    // own Deframer, each byte with its start-bit instant, then drives the state machine.
    MasterEvent poll(uint64_t now_us);

    bool busy() const;
    // Number of transmissions sent so far (1..3) for the open/last transaction.
    uint8_t attempts() const;
    void set_bit_rate(uint32_t bps);

    const AddrStats& stats(uint8_t addr) const;
    const BusStats& bus_stats() const;
    void reset_stats();

  private:
    enum class SubPhase : uint8_t { AwaitResponse, PendingTransmit };

    void do_transmit(uint64_t at_us);
    // Ends the current attempt (timeout or CRC-failed response): counts it, updates
    // last_activity, and either schedules a retry (data-model.md §4 "Gap") or concludes
    // the transaction Failed.
    void end_attempt(uint64_t last_activity_us, MasterEvent::Reason reason, MasterEvent& event);
    // Fires a transmission whose scheduled instant (PendingTransmit's deadline_) has been
    // reached by now_us — the one place "deferred to that instant" (data-model.md §4
    // "Gap") is honoured, for both a fresh begin() and a scheduled retry.
    void fire_pending(uint64_t now_us);

    ByteWire& wire_;
    Clock& clock_;
    uint8_t host_addr_;

    Deframer deframer_;
    // Instant of the FLAG byte that opened the response frame accumulation now in
    // progress — persisted across poll() calls, mirroring MockWire's own open_flag_us_
    // (tests/support/mock_wire.hpp): a response split across two poll() calls must still
    // get its true opening instant for the T_resp acceptance check.
    uint64_t resp_open_us_ = 0;

    uint8_t next_seq_[kAddrCount] = {};

    bool open_ = false;
    SubPhase sub_phase_ = SubPhase::AwaitResponse;
    uint8_t dst_ = 0;
    uint8_t seq_ = 0;
    uint8_t attempt_count_ = 0; // transmissions sent so far for the open/last transaction
    uint8_t payload_[LIMIT_max_l3_payload] = {};
    uint8_t len_ = 0;
    // AwaitResponse: the response-window deadline (tx_end + T_resp). PendingTransmit: the
    // instant transmission is deferred to (data-model.md §4 "Gap").
    uint64_t deadline_ = 0;
    // AwaitResponse only: tx_end of the CURRENT attempt - the acceptance window's lower
    // bound (contracts/link-cpp.md: "[tx_end, tx_end + T_resp)"). Needed because a retry
    // reuses the same seq (trunk §7): without it, a same-seq response that arrived just
    // after a PRIOR attempt's window closed and is only drained once the retry re-enters
    // AwaitResponse would satisfy frame_open_us < deadline_ and be mistaken for the retry's
    // own answer (PR #137 review, MEDIUM).
    uint64_t window_start_us_ = 0;

    // last_activity/T_gap is bus-wide (trunk §3 media access governs the shared bus, not
    // a per-destination resource): end of the last accepted response's last byte, the
    // last byte of a discarded frame, or the timeout instant (data-model.md §4 "Gap").
    // Unset until the very first transmission ever, which is therefore never gap-deferred.
    bool has_last_activity_ = false;
    uint64_t last_activity_ = 0;

    uint8_t response_buf_[LIMIT_max_l3_payload] = {};

    AddrStats stats_[kAddrCount] = {};
    BusStats bus_stats_ = {};
};

} // namespace link
} // namespace omgp
