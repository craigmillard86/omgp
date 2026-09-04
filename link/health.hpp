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
    // MUST NOT call ANY HealthTracker method from inside on_notice: re-entrancy is
    // outside the contract until F3's scheduler states its needs (OPEN-QUESTIONS
    // 2026-09-04). The notify-after-assign ordering that makes one re-entrant recovery
    // happen to work is incidental, not promised; the blanket form deliberately sweeps
    // in the const observers (state/poll_due) as the safe default, and F3's design may
    // narrow it by superseding that entry. (History: reviews on #124.)
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
    // Enrolment rotation over UNENROLLED/OFFLINE backplane addresses. When no candidate
    // exists (healthy-rig steady state), returns Probe{ADDR_host, ...} as the sentinel:
    // ADDR_host is never a real probe target — callers MUST check for it before spending
    // trunk §6's enrolment slot. Contract amendment proposed in docs/OPEN-QUESTIONS.md
    // (2026-09-03 next_probe-sentinel entry).
    Probe next_probe(uint64_t now_us);

    bool bus_fault() const;
    uint32_t bit_rate() const;

  private:
    // data-model §6 also lists `ever_answered`; omitted until a transition rule reads it
    // (recorded divergence, review on #118 — striking it from the data model is a T3 edit).
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
    // clock_ itself is not yet read; the constructor body performs one discarded read,
    // which silences clang's -Wunused-private-field (fuzz preset) without
    // [[maybe_unused]] — some gcc versions reject that attribute on a data member
    // under -Werror=attributes (observed locally, gcc/WSL).
    Clock& clock_;
    HealthListener& listener_;
    HealthRecord records_[kAddrCount];
    uint8_t next_probe_addr_;
};

} // namespace link
} // namespace omgp
