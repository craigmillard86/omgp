// OMGP trunk L2 — Master transaction engine: trunk §3 (media access), §7 (retry rule).
// Contract: specs/002-trunk-link-layer/contracts/link-cpp.md "Master engine"; state
// model: specs/002-trunk-link-layer/data-model.md §4/§8. Embedded path: C++17, no
// exceptions, no RTTI, no heap.
//
// Signature-only stub (T031, a separate issue, implements the retry/timing/acceptance
// state machine trunk §3/§7 and the contract above describe). tests/unit/
// test_link_master.cpp (T029) is written against this header so the suite compiles;
// every method here is an inert placeholder — begin() opens no transaction, poll()
// drains nothing and reports no progress — so the test file's own REQUIREs, not a
// missing symbol, are what turn it red until T031 lands (review on PR #137: a compile
// failure here suppresses every other suite's evidence, including CodeQL, for the whole
// pipeline run).
#pragma once

#include "link/byte_wire.hpp"
#include "link/clock.hpp"
#include "link/link_types.hpp"

#include <cstddef>
#include <cstdint>

namespace omgp {
namespace link {

// contracts/link-cpp.md "Master engine".
struct MasterEvent {
    enum Kind : uint8_t { None, Answered, Failed } kind;
    FrameFields response; // valid when kind == Answered
    enum Reason : uint8_t { Timeout, CrcFailed } reason;
};

class Master {
  public:
    Master(ByteWire& wire, Clock& clock, uint8_t host_addr = 0x00);

    Status begin(uint8_t dst, const uint8_t* payload, size_t len);
    MasterEvent poll(uint64_t now_us);
    bool busy() const;
    uint8_t attempts() const;
    void set_bit_rate(uint32_t bps);
    const AddrStats& stats(uint8_t addr) const;
    const BusStats& bus_stats() const;
    void reset_stats();

  private:
    ByteWire& wire_;
    // Stored for constructor-signature parity with the contract and for T031, which
    // needs it for the response-window/gap/timeout state machine; this stub's poll()
    // takes `now_us` as an explicit parameter and never reads clock_ itself (same
    // reasoning as HealthTracker::clock_ in health.hpp).
    Clock& clock_;
    uint8_t host_addr_;
    AddrStats stats_[kAddrCount] = {};
    BusStats bus_stats_ = {};
};

} // namespace link
} // namespace omgp
