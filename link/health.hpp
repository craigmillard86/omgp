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
// ruling 2026-09-14) is assumed by `HealthTracker::set_bit_rate`.
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
    // Call when a status poll for `addr` goes out (poll_due() said so). Besides the
    // poll-period stamp poll_due() reads, this now also records the rate the poll went out at
    // (the rate in use, while !bus_fault()) the same way a probe's own issue is recorded —
    // fixed #578 red team round 5, finding 1: without it, an ordinary poll's later outcome was
    // classified by whatever bus_.bit_rate a set_bit_rate() call and a declare happened to
    // leave behind, not the rate the poll actually used, inverting both of trunk §7's clear
    // rules. Signature unchanged; the rate recorded is read from this tracker's own state, not
    // a new parameter — see link/health.cpp's note_probe() for the shared mechanism.
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
    // The rate in use; assigned by a clear (data-model.md §7) or by set_bit_rate below, and
    // always one of trunk §9's two rates — see set_bit_rate's refusal.
    uint32_t bit_rate() const;
    // trunk §7: the layer above's rate selection — bringing a rig up AT the fallback rate, or
    // restoring the reference rate after a fallback-rate clear, which §7 leaves in place for
    // ever (ruling 2026-09-14). Assigns what bit_rate() reports and, while !bus_fault(), the
    // rate enrolment probes go out at; during a fault the probe's rate is the probe's own
    // (the alternation, the reference pass). NEVER a clear: the call itself moves no node
    // state, notifies nothing and ends no episode. What it does to a probe still outstanding
    // when it is called depends on which probe that is, and the difference is stated rather
    // than claimed away (CLAUDE.md rule 11; red team rounds 1 and 2 on #578): an ORDINARY
    // enrolment probe's outcome — one issued since the last clear — is read exactly as it
    // would have been without the call (BusState::probe_across_clear is what makes that true).
    // That is a statement about the READING, and the state it produces is the other half of
    // it: an ok enrols, so the node is poll_due() at the rate just selected on the strength of
    // an answer at the rate before it, and if it cannot hear the new one it walks to SUSPECT
    // and — as the only enrolled address — declares a BUS_FAULT on a healthy trunk. A CLEAR
    // that makes the identical move strands that probe instead; the two rate-movers differ,
    // the contract's sentence covers only the clear's side ("a FAULT-TIME probe"), and which
    // of the two is right is a contract question recorded in docs/OPEN-QUESTIONS.md 2026-09-16
    // (third entry), ruling pending — not resolved here (red team round 4 on #578, finding 1).
    // Demonstrated by "the node enrolled by the two cases above is then polled at the rate the
    // selection moved to ...". Meanwhile a probe outstanding across a clear IS caught by the
    // late-outcome exception the
    // contract writes as "issued at a rate other than the rate now in use"
    // (contracts/link-cpp.md "Health tracker") — so a selection that moves the rate in use
    // away from that probe's rate voids its §6 transition. Both demonstrated by
    // tests/unit/test_link_busfault.cpp ("a selection under an outstanding probe does not
    // suppress that probe's outcome ..." and "a selection after a clear voids the outcome of a
    // FAULT-TIME probe still outstanding ..."). Counts one rate change when it moves the rate
    // in use or the wire (the two come apart during a fault) and none when it moves neither.
    // Refused — assigning nothing and counting nothing — on every rate other than trunk §9's
    // two: the rate in use is a one-bit quantity everywhere below this line, so a third rate
    // was classified as the reference rate and cleared faults there (red team round 2 on #578,
    // finding 1). STRICTER than Master::set_bit_rate's predicate (link/master.cpp: bps == 0 or
    // a byte time that truncates to 0 us), which it subsumes — both §9 rates have a nonzero
    // byte time. A stated DIVERGENCE from contracts/link-cpp.md's "refused on exactly
    // Master::set_bit_rate's rule", recorded in docs/OPEN-QUESTIONS.md 2026-09-16, ruling
    // pending. The caller moves the wire in the same step: F3 obligation 3 in
    // contracts/link-cpp.md "What F3/F4 need", ASSUMED here, not enforceable at this layer —
    // a caller that selects a rate this refuses puts the two out of step, which is that
    // obligation's violation, not a state this layer can represent.
    void set_bit_rate(uint32_t bps);
    // Bus-level counters (data-model.md §8): `rate_changes` and `bus_faults` as decided by
    // this tracker. `BusStats::discards` is the Master engine's field and is never written
    // here — the tracker sees no frames. (Accessor added by T043 alongside the bus-fault
    // logic the counters describe; contracts/link-cpp.md "Health tracker" amended to match
    // the 2026-09-15 ruling that the tracker and Master own disjoint BusStats fields by design.)
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
        // The rate in use; assigned by a clear or by the layer above's set_bit_rate, and
        // always one of trunk §9's two rates — everything that reads it reduces it to the
        // one-bit "fallback : reference" question `probe_fallback` below asks, so a third
        // value has no meaning here (set_bit_rate refuses one; a clear is called with the
        // two constants only).
        uint32_t bit_rate = omgp::TRUNK_bit_rate;
        bool fault = false; // BUS_FAULT declared
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
        // The rate the host last put on the wire — a probe's rate, a rate pinned by a clear,
        // or one the layer above selected (which moves the wire in the same step, F3
        // obligation 3). Not a data-model.md §7 field: it is how this implementation counts
        // `rate_changes` (§8: at the point the change is decided). Before any probe has been
        // issued it is the rate in use. It is NOT the whole of the counting rule: §8 counts a
        // change of the RATE IN USE too (data-model.md:267), and during a fault the two come
        // apart — see set_bit_rate in health.cpp.
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
        // One bit per address: a §7 CLEAR decided the rate in use while the probe `probe_live`
        // above is waiting on was already outstanding. Set by clear_fault() for every address
        // still owing an outcome, cleared by note_probe() on the next handout to that address;
        // read only alongside that address's live bit, so a stale bit is unreadable. Not a
        // data-model.md §7 field: it is what bounds on_result's late-outcome exception, which
        // contracts/link-cpp.md "Health tracker" writes as "a FAULT-TIME probe".
        //
        // That bound used to be INFERRED — a probe whose rate differs from the rate in use —
        // which held only while a clear was the one writer of `bit_rate`; set_bit_rate is a
        // second one, and with it the inference put an ordinary enrolment probe's answer, on a
        // rig that had never faulted, in the exception's scope and left the node UNENROLLED
        // (red team round 1 on #578, finding 1). Recording "went out while a fault stood"
        // instead was narrower than the rule needs, in the other direction: a whole episode
        // fits inside one outcome window, so an ordinary probe issued BEFORE the declare can
        // still be outstanding at a clear that pins the FALLBACK rate for ever (FR-025/FR-026),
        // and applying its reference-rate answer enrolled the node on a trunk it has never
        // answered — round 15's harm, back (red team round 3 on #578, finding 1). The clear is
        // what both cases have in common: it is the moment this layer decides the rate in use
        // under a frame already on the wire. A WIDENING of the contract's "fault-time probe",
        // recorded in docs/OPEN-QUESTIONS.md 2026-09-16, ruling pending; it suppresses no
        // outcome the contract's own wording does not, since !bus_fault() after an episode is
        // reached only through clear_fault and every fault-time probe still live there takes
        // the bit. Demonstrated by "a selection under an outstanding probe does not suppress
        // that probe's outcome ...", "every node that answers a probe enrols ...", "a probe
        // issued after the clear is not read as the fault-time probe ..." and "a probe issued
        // BEFORE an episode does not enrol its node ..." in
        // tests/unit/test_link_busfault.cpp.
        uint16_t probe_across_clear = 0;
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
