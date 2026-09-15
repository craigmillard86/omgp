// OMGP trunk L2 — node health tracker: trunk §6 (ENROLLED/SUSPECT/OFFLINE lifecycle) and
// §7 (bus fault re-probe). Contract: specs/002-trunk-link-layer/contracts/link-cpp.md
// "Health tracker"; data model: specs/002-trunk-link-layer/data-model.md §6/§7. Embedded
// path: C++17, no exceptions, no RTTI, no heap — a fixed kAddrCount-entry table.
//
// Bus fault (trunk §7) is US5 (T043): the declare rule, the alternating-rate re-probe, the
// reference pass and the two clear rules of data-model.md §7 (amended 2026-09-13, F4 —
// docs/OPEN-QUESTIONS.md "F4's two deferred values"). Three obligations on the scheduler
// (F3) that this layer assumes and cannot enforce are recorded in contracts/link-cpp.md
// "What F3/F4 need"; `on_result` and `next_probe` below say where the first two are relied
// on. The third (the wire and this tracker set to the layer above's rate in the same step,
// ruling 2026-09-14) belongs with `HealthTracker::set_bit_rate`, which is NOT implemented
// here — tasks.md T041/T043 carry it as a red-first follow-up slice.
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
// `tick` change records, and so does `next_probe` on one path: writing off an abandoned pass
// probe after the outcome window can end the pass and clear the fault at the fallback rate,
// which applies the deferred §6 transitions and notifies (round 14). Every transition notifies
// `listener` exactly once (SC-006); `on_notice` must not re-enter the tracker from any of the
// three.
class HealthTracker {
  public:
    HealthTracker(Clock& clock, HealthListener& listener);

    // One transaction outcome. While bus_fault() that is a probe's — F3 obligation 1 (probes
    // are the only traffic during a fault) is what makes the rate this result arrived at
    // knowable here: it is the rate next_probe() last handed out FOR THIS ADDRESS
    // (data-model.md §7). Per address, not per wire: an extra next_probe() call moves the wire
    // to another address's probe, and reading this outcome at that rate inverts the §7
    // classification (red team round 2 on #530, finding 1).
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
        // When the last probe to this address went out (next_probe's now_us): the epoch of the
        // outstanding-probe exemption's outcome window (BusState::probe_live). Per address —
        // one stamp for the whole set was re-armed by every later probe to ANY address, so a
        // discovery probe kept an abandoned probe's record alive (round-9 red team on #530).
        uint64_t probe_issued_us = 0;
    };

    // Bus state (data-model.md §7). `rate_changes`/`faults` of the data model's BusState live
    // in `stats_` (§8, the shape `bus_stats()` returns); the rest are here.
    //
    // Each field carries its own start-of-life value rather than a position in a brace list in
    // health.cpp's constructor, which `bus_{}` now fills from here: with ten fields, six of them
    // integers, a positional list is one transposition away from compiling with two values
    // swapped. Still an aggregate (C++17) and still no dynamic allocation.
    struct BusState {
        uint32_t bit_rate = omgp::TRUNK_bit_rate; // the rate in use; assigned only by a clear
        bool fault = false;                       // BUS_FAULT declared
        // The alternation's next rate; true (fallback) on declare.
        bool next_probe_fallback = false;
        // Address to enrol on a clear at the fallback rate; 0 = none recorded.
        uint8_t fallback_answerer = 0;
        // One bit per address: answered at the FALLBACK rate in this episode. §7's clear rules
        // name one answerer (`fallback_answerer` above, the data-model field), but two nodes
        // answering the fallback probes is the ordinary shape of a rate-mismatch fault, and the
        // round-8 protection — an ok from an answerer is its fallback answer again, never a
        // reference-rate clear, even after the pass probe overwrote its rate bit — must cover
        // every one of them, not the latest (round-11 red team on #530, finding 2). On a
        // fallback-rate clear every address here takes its deferred §6 transition (each
        // demonstrated it hears that rate), a superset of the data model's single answerer,
        // labelled as such. Emptied by every clear (clear_fault, at either rate — the one reset;
        // every episode ends with a clear and a tracker starts empty, round 12/13).
        uint16_t fallback_seen = 0;
        // Reference-pass outcomes still owed; 0 = no pass running.
        uint8_t ref_pass_left = 0;
        uint8_t pass_addr = 0; // the pass probe in flight; 0 = none outstanding
        // The pass's own cursor, in address order.
        uint8_t pass_next_addr = omgp::ADDR_backplane_min;
        // The rate the host last put on the wire — a probe's rate, or a rate pinned by a
        // clear. Not a data-model.md §7 field: it is how this implementation counts
        // `rate_changes` (§8: at the point the change is decided). Before any probe has been
        // issued it is the rate in use.
        uint32_t wire_rate = omgp::TRUNK_bit_rate;
        // One bit per address (bit N = address N): set when the last probe to that address
        // went out at TRUNK_bit_rate_fallback, clear when it went out at the reference rate.
        // Not a data-model.md §7 field either: since `on_result` carries no rate, this is how
        // the §7 classification ("a valid answer at the reference rate" vs "at the fallback
        // rate") is made, and it is read per address rather than from `wire_rate` so that a
        // next_probe() call with an outcome still outstanding (F3 obligation 2 violated)
        // cannot re-read an answer at a rate its own probe never used — the consequence
        // data-model.md §6 records for that violation, phantom `rate_changes`, is then the
        // whole of it unless the SAME address is probed twice before its first outcome
        // arrives, which obligation 1 assumes away. (Red team round 2 on #530, finding 1.)
        // Reset per EPISODE by evaluate_declare(), to the rate in use: an outcome can arrive
        // for an address the new episode has not probed — a poll issued in the superframe
        // that declared the fault, which obligation 1 (an obligation to ISSUE) does not
        // forbid — and it arrived at that rate, not at whatever a previous episode's last
        // probe to that address used. (Red team round 5 on #530, finding 1.) That reset skips
        // the addresses named by `probe_live` below, whose record is still owed to a probe.
        uint16_t probe_fallback = 0;
        // One bit per address: a probe has gone out to it and its outcome has not yet been
        // read. Set by note_probe(), cleared by the on_result() that consumes the record.
        // It is what makes the per-episode reset above safe: for an address with an
        // OUTSTANDING probe the remembered rate is the only correct record in the system —
        // the new episode's rate in use says nothing about a frame already on the wire — so
        // that address keeps its bit across the boundary (red team round 6 on #530, finding
        // 1). Not a data-model.md §7 field; like probe_fallback it exists only because
        // on_result carries no rate.
        // BOUNDED by the outcome window (round-7 red team on #530, finding 1; bound corrected
        // round 9): a probe's outcome — answer or timeout — reaches on_result within the
        // transaction's request, in-flight hold and response window of its ISSUE
        // (contracts/link-cpp.md "Behaviour": the window is [tx_end, tx_end + T_resp), the
        // hold extends it by one worst-case frame), which health.cpp's kOutcomeWindowUs
        // bounds at the slowest rate for every probe. A bit older than that owes this layer
        // nothing: its probe was dropped at L2, abandoned at a superframe boundary, or its
        // timeout never reported. Unbounded, one such probe exempted its address from the
        // reset in EVERY later episode and a later reference-rate answer from it was read
        // at the stale fallback bit — the fault did not clear at the reference rate and the
        // trunk pinned at the fallback one. evaluate_declare() therefore honours each bit
        // only while now - HealthRecord::probe_issued_us <= kOutcomeWindowUs and drops it
        // otherwise. Round 7's first bound — T_resp measured from issue, one stamp for the
        // set — was shorter than a fallback-rate request's own transmission and re-armed by
        // any later probe; both corrected round 9. Demonstrated by the sweep, the
        // discovery-probe and the abandoned-probe cases in tests/unit/test_link_busfault.cpp;
        // the in-window half by the two "... across an episode boundary" cases. Several bits
        // may be set: two outstanding probes violate neither F3 obligation (obligation 2 is
        // one call per probe issued, not one outcome before the next), so a later handout
        // never drops an earlier live bit — the window is the only bound (round 9's "newest
        // handout supersedes" rule reinstated round 6's oscillation; withdrawn at round 10).
        uint16_t probe_live = 0;
    };

    void notify(Notice notice, uint8_t addr);
    uint8_t next_backplane_addr(uint8_t addr) const; // wraps ADDR_backplane_min..ADDR_backplane_max
    void apply_result(uint8_t addr, bool ok, uint64_t now_us); // the §6 transition table alone
    void note_wire_rate(uint32_t bit_rate);                    // counts a change (§8)
    void note_probe(uint8_t addr, uint32_t bit_rate,
                    uint64_t now_us);                     // remembers that probe's rate
    void evaluate_declare(uint64_t now_us);               // trunk §7 declare rule
    void clear_fault(uint32_t bit_rate, uint64_t now_us); // trunk §7 clear, at that rate
    Probe pass_probe(uint64_t now_us);                    // one reference-pass probe

    // Stored for the constructor-signature parity with Master/Responder (link-cpp.md
    // "Health tracker"). Every method takes `now_us` explicitly — including the bus-fault
    // rules, which are driven by outcomes and, since round 14, by the outcome window closing
    // on a probe that never got one — so clock_ itself is
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
