// OMGP trunk L2 — node health tracker: trunk §6 (ENROLLED/SUSPECT/OFFLINE lifecycle) and
// §7 (bus fault re-probe). Contract: specs/002-trunk-link-layer/contracts/link-cpp.md
// "Health tracker"; data model: specs/002-trunk-link-layer/data-model.md §6/§7. Embedded
// path: C++17, no exceptions, no RTTI, no heap — a fixed kAddrCount-entry table.
//
// Bus-fault declare/clear (trunk §7) is US5 (T043) and stubbed here per
// specs/002-trunk-link-layer/tasks.md T038: bus_fault() always false, bit_rate() always
// TRUNK_bit_rate, next_probe() never alternates rate.
#pragma once

#include "link/clock.hpp"
#include "link/link_types.hpp"

namespace omgp {
namespace link {

// Notification sink for state transitions (data-model.md §9); each call carries the
// address the transition applies to (0 for bus-level notices, not yet emitted by this
// stub). Implemented by F3's scheduler and by a recording listener in tests.
struct HealthListener {
    virtual void on_notice(Notice notice, uint8_t addr) = 0;

  protected:
    ~HealthListener() = default;
};

// Result of an enrolment-rotation probe (data-model.md §6): the address to probe next and
// the bit rate to probe it at.
struct Probe {
    uint8_t addr;
    uint32_t bit_rate;
};

// Fixed 16-entry health table keyed by trunk address (data-model.md §6). `on_result` and
// `tick` are the only ways a record changes state; every transition notifies `listener`
// exactly once (SC-006).
class HealthTracker {
  public:
    HealthTracker(Clock& clock, HealthListener& listener);

    void on_result(uint8_t addr, bool ok, uint64_t now_us);
    void tick(uint64_t now_us); // time-only transitions: SUSPECT -> OFFLINE

    HealthState state(uint8_t addr) const;
    bool poll_due(uint8_t addr, uint64_t now_us) const;
    void mark_polled(uint8_t addr, uint64_t now_us);
    Probe next_probe(uint64_t now_us); // enrolment rotation over UNENROLLED/OFFLINE addresses

    bool bus_fault() const;
    uint32_t bit_rate() const;

  private:
    struct HealthRecord {
        HealthState state = HealthState::UNENROLLED;
        uint8_t consecutive_failures = 0;
        uint64_t suspect_since_us = 0;
        uint64_t last_poll_us = 0;
    };

    void notify(Notice notice, uint8_t addr);
    uint8_t next_backplane_addr(uint8_t addr) const; // wraps ADDR_backplane_min..ADDR_backplane_max

    // Stored for the constructor-signature parity with Master/Responder (link-cpp.md
    // "Health tracker") and for T043 (US5), which reads it for bus-fault re-probe timing
    // (data-model.md §7). Every method in this T038 stub takes `now_us` explicitly, so
    // clock_ itself is not yet read — clang's -Wunused-private-field would otherwise flag
    // it under the fuzz preset (tools/fuzz-smoke.sh, clang++ -Werror).
    [[maybe_unused]] Clock& clock_;
    HealthListener& listener_;
    HealthRecord records_[kAddrCount];
    uint8_t next_probe_addr_;
};

} // namespace link
} // namespace omgp
