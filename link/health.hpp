// OMGP trunk L2 — node health tracker: trunk §6 (ENROLLED/SUSPECT/OFFLINE lifecycle) and
// §7 (bus fault re-probe). Contract: specs/002-trunk-link-layer/contracts/link-cpp.md
// "Health tracker"; data model: specs/002-trunk-link-layer/data-model.md §6/§7. Embedded
// path: C++17, no exceptions, no RTTI, no heap — a fixed kAddrCount-entry table.
//
// Bus fault (trunk §7) is US5 (T043): the declare rule, the alternating-rate re-probe, the
// reference pass and the two clear rules of data-model.md §7 (amended 2026-09-13, F4 —
// docs/OPEN-QUESTIONS.md "F4's two deferred values"). Two obligations on the scheduler (F3)
// that this layer assumes and cannot enforce are recorded in contracts/link-cpp.md
// "What F3/F4 need"; `on_result` and `next_probe` below say where each one is relied on.
#pragma once

#include "link/clock.hpp"
#include "link/link_types.hpp"

namespace omgp {
namespace link {

// Notification sink for state transitions (data-model.md §9); each call carries the
// address the transition applies to (0 for the bus-level BUS_FAULT/ALERT/BUS_RECOVERED
// notices). Implemented by F3's scheduler and by a recording listener in tests.
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

    // One transaction outcome. While bus_fault() that is a probe's — F3 obligation 1 (probes
    // are the only traffic during a fault) is what makes the rate this result arrived at
    // knowable here: it is the rate next_probe() last handed out (data-model.md §7).
    void on_result(uint8_t addr, bool ok, uint64_t now_us);
    void tick(uint64_t now_us); // time-only transitions: SUSPECT -> OFFLINE

    HealthState state(uint8_t addr) const;
    // ENROLLED: true; SUSPECT: every kSuspectPollPeriod_us; else false — and false for every
    // address while bus_fault() (data-model.md §6, amended 2026-09-13: no status polls while
    // a fault is declared, only next_probe() drives the wire).
    bool poll_due(uint8_t addr, uint64_t now_us) const;
    void mark_polled(uint8_t addr, uint64_t now_us);
    // Enrolment rotation over UNENROLLED/OFFLINE backplane addresses. When no candidate
    // exists (healthy-rig steady state), returns Probe{ADDR_host, ...} as the sentinel:
    // ADDR_host is never a real probe target — callers MUST check for it before spending
    // trunk §6's enrolment slot. Contract amendment proposed in docs/OPEN-QUESTIONS.md
    // (2026-09-03 next_probe-sentinel entry).
    //
    // While bus_fault() the candidate set also includes SUSPECT addresses and the rate
    // alternates fallback, reference, fallback, … per call — except during a reference pass,
    // when every probe goes out at TRUNK_bit_rate (data-model.md §6/§7, amended 2026-09-13).
    // Each call advances the rotation and the alternation: F3 obligation 2 is to call it once
    // per probe issued.
    Probe next_probe(uint64_t now_us);

    bool bus_fault() const;
    uint32_t bit_rate() const; // the rate in use; assigned only by a clear (data-model.md §7)
    // Bus-level counters (data-model.md §8): `rate_changes` and `bus_faults` as decided by
    // this tracker. `BusStats::discards` is the Master engine's field and is never written
    // here — the tracker sees no frames. (Accessor added by T043 alongside the bus-fault
    // logic the counters describe; contracts/link-cpp.md "Health tracker" amended to match,
    // pending a ruling.)
    const BusStats& bus_stats() const;

  private:
    // data-model §6 also lists `ever_answered`; omitted until a transition rule reads it
    // (recorded divergence, review on #118 — striking it from the data model is a T3 edit).
    struct HealthRecord {
        HealthState state = HealthState::UNENROLLED;
        uint8_t consecutive_failures = 0;
        uint64_t suspect_since_us = 0;
        uint64_t last_poll_us = 0;
    };

    // Bus state (data-model.md §7). `rate_changes`/`faults` of the data model's BusState live
    // in `stats_` (§8, the shape `bus_stats()` returns); the rest are here.
    struct BusState {
        uint32_t bit_rate;         // the rate in use; assigned only by a clear
        bool fault;                // BUS_FAULT declared
        bool next_probe_fallback;  // the alternation's next rate; true (fallback) on declare
        uint8_t fallback_answerer; // address to enrol on a clear at the fallback rate; 0 = none
        uint8_t ref_pass_left;     // reference-pass outcomes still owed; 0 = no pass running
        uint8_t pass_addr;         // the pass probe in flight; 0 = none outstanding
        uint8_t pass_next_addr;    // the pass's own cursor, in address order
        // The rate the host last put on the wire — a probe's rate, or a rate pinned by a
        // clear. Not a data-model.md §7 field: it is how this implementation both counts
        // `rate_changes` (§8: at the point the change is decided) and tells a fallback-rate
        // answer from a reference-rate one, since `on_result` carries no rate. Sound only
        // under F3 obligation 1; before any probe has been issued it is the rate in use.
        uint32_t wire_rate;
    };

    void notify(Notice notice, uint8_t addr);
    uint8_t next_backplane_addr(uint8_t addr) const; // wraps ADDR_backplane_min..ADDR_backplane_max
    void apply_result(uint8_t addr, bool ok, uint64_t now_us); // the §6 transition table alone
    void note_wire_rate(uint32_t bit_rate);                    // counts a change (§8)
    void evaluate_declare();                                   // trunk §7 declare rule
    void clear_fault(uint32_t bit_rate, uint64_t now_us);      // trunk §7 clear, at that rate
    Probe pass_probe();                                        // one reference-pass probe

    // Stored for the constructor-signature parity with Master/Responder (link-cpp.md
    // "Health tracker"). Every method takes `now_us` explicitly — including the bus-fault
    // rules, which are driven by outcomes and never by elapsed time — so clock_ itself is
    // still not read; the constructor body performs one discarded read, which silences
    // clang's -Wunused-private-field (fuzz preset) without [[maybe_unused]] — some gcc
    // versions reject that attribute on a data member under -Werror=attributes (observed
    // locally, gcc/WSL).
    Clock& clock_;
    HealthListener& listener_;
    HealthRecord records_[kAddrCount];
    uint8_t next_probe_addr_;
    BusState bus_;
    BusStats stats_;
};

} // namespace link
} // namespace omgp
