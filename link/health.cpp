// OMGP trunk L2 — node health tracker implementation: trunk §6, §7; transition table
// data-model.md §6, bus state data-model.md §7. Makes tests/unit/test_link_health.cpp (T037)
// and tests/unit/test_link_busfault.cpp (T041) pass.
#include "link/health.hpp"

#include "omgp_protocol.h"

#include <cstddef>

namespace omgp {
namespace link {

namespace {
// Elapsed microseconds treating an earlier `now` as zero, never a wrapped uint64_t:
// now_us is a caller-supplied parameter with no cross-entry-point monotonicity contract
// (red-team on #118 finding 2 — one early stamp forced an instant, silent OFFLINE, and
// in poll_due silently restored a SUSPECT node to full poll rate).
constexpr uint64_t elapsed_us(uint64_t now_us, uint64_t since_us) {
    // mutant-ok(equivalent, cxx_ge_to_gt): at now_us == since_us both arms yield 0.
    return now_us >= since_us ? now_us - since_us : 0; // labelled above: OPEN-QUESTIONS 2026-09-03
}
// data-model.md §6: OFFLINE threshold is TRUNK_offline_after_suspect_ms of SUSPECT time,
// but every clock reading in this engine is in microseconds.
constexpr uint64_t kUsPerMs = 1000; // literal-ok: unit conversion, not a protocol value
constexpr uint64_t kOfflineThresholdUs =
    static_cast<uint64_t>(omgp::TRUNK_offline_after_suspect_ms) * kUsPerMs;
// Not a node: the shared guard for every wire-derived address. link/frame.cpp validates
// only dst == 0xFF (src is copied through), and ADDR_host (0x00) is the host's own
// address (trunk §5) — it must never grow a peer record, or an echoed src=0x00 frame
// enrols the host and every notice carries the addr data-model §9 reserves for
// bus-level events (red-team on #118 finding 1).
// Scope (rule 11): the `addr < kAddrCount` below is the file's residual wrap comparison;
// its <= mutant is DEMONSTRATED killed by the bad-address cases in test_link_health.cpp
// (0x10 writes records_[16], which state(0x10) reads back).
constexpr bool is_node_addr(uint8_t addr) {
    return addr != omgp::ADDR_host && addr < kAddrCount;
}
// data-model.md §9: a bus-level notice (BUS_FAULT, ALERT, BUS_RECOVERED) carries addr 0.
// 0x00 is the host's own address (trunk §5) and is never a node, so it names no record.
constexpr uint8_t kBusAddr = 0; // literal-ok: the data model's bus-level sentinel, not a
                                // protocol value
// Number of backplane addresses in the enrolment rotation (trunk §5: 0x01..0x0F).
constexpr size_t kBackplaneCount =
    static_cast<size_t>(omgp::ADDR_backplane_max) - omgp::ADDR_backplane_min + 1;
} // namespace

HealthTracker::HealthTracker(Clock& clock, HealthListener& listener)
    : clock_(clock), listener_(listener), records_{}, next_probe_addr_(omgp::ADDR_backplane_max),
      bus_{omgp::TRUNK_bit_rate, false, false, 0, 0, 0, omgp::ADDR_backplane_min,
           omgp::TRUNK_bit_rate},
      stats_{} {
    (void)clock_; // discarded read: see the clock_ declaration comment in health.hpp
}

void HealthTracker::notify(Notice notice, uint8_t addr) {
    listener_.on_notice(notice, addr);
}

void HealthTracker::on_result(uint8_t addr, bool ok, uint64_t now_us) {
    // Early return, not `assert` (which compiles out under NDEBUG on the ESP32-S3 target
    // build, rule 10) and not modulo (which would alias 0x11 onto backplane 0x01's record).
    if (!is_node_addr(addr))
        return;

    // trunk §7 / data-model.md §7. The rate this outcome arrived at is the rate the last
    // probe went out at (bus_.wire_rate) — knowable only because probes are the only traffic
    // while a fault is declared (F3 obligation 1, contracts/link-cpp.md "What F3/F4 need",
    // ASSUMED here, not enforced). Outside a fault the rate plays no part.
    const bool at_reference = bus_.wire_rate == omgp::TRUNK_bit_rate;
    const bool fallback_answer = bus_.fault && ok && !at_reference;

    if (fallback_answer) {
        // §7: a fallback-rate answer neither clears the fault nor moves that node's state —
        // enrolling it here would have it status-polled at a rate it cannot hear, fail into
        // SUSPECT and re-declare the fault it just recovered (round-4 red team on #472). It
        // is recorded, and a reference pass over every enrolled address starts.
        // No pass can already be running: every probe issued during one goes out at the
        // reference rate (pass_probe below), so no outcome while a pass runs is a
        // fallback-rate answer. True by construction, not by assumption.
        bus_.fallback_answerer = addr;
        uint8_t enrolled = 0;
        for (const HealthRecord& r : records_)
            if (r.state != HealthState::UNENROLLED)
                ++enrolled;
        bus_.ref_pass_left = enrolled; // exactly |enrolled| probes (FR-026)
        bus_.pass_addr = 0;
        // The cursor starts AT ADDR_backplane_min, never below it: cxx_assign_const rewrites
        // this RHS to the type's zero value, which spends one of pass_probe()'s
        // kBackplaneCount iterations on records_[ADDR_host] and so stops one address short of
        // ADDR_backplane_max. Killed by "a pass whose only enrolled address is the last one in
        // the rotation" in tests/unit/test_link_busfault.cpp.
        bus_.pass_next_addr = omgp::ADDR_backplane_min;
    } else {
        apply_result(addr, ok, now_us);
    }

    // §7: the pass is advanced by the FIRST outcome for the address whose pass probe is in
    // flight, and by nothing else — not by a probe being issued (a tick between the last
    // probe and its result must not fire the clear, round-8 red team) and not by a later
    // outcome for the same address. `pass_addr` is non-zero only while a pass probe is
    // outstanding, and `addr` is a node address here, so this one comparison carries the
    // whole condition: no redundant "a pass is running" conjunct to get out of step with it.
    // A bool initialised `false`; the mutation writes 0, the same value (the same rewrite is
    // recorded on `bool belief_stale = false` in link/responder.cpp poll()).
    // mutant-ok(equivalent, cxx_init_const): the mutation and the original coincide.
    bool pass_ended = false;
    if (bus_.pass_addr == addr) {
        bus_.pass_addr = 0;
        // Cannot wrap: pass_probe() sets pass_addr only while ref_pass_left > 0, and this is
        // the only place ref_pass_left is decremented.
        pass_ended = --bus_.ref_pass_left == 0;
    }

    if (bus_.fault && ok && at_reference) {
        // §7: the host prefers the reference rate — a valid answer there clears at once,
        // mid-pass or not. `addr` has already taken its own §6 transition above.
        clear_fault(omgp::TRUNK_bit_rate, now_us);
    } else if (pass_ended && bus_.fallback_answerer != 0) {
        // §7: the pass drew nothing, so the fallback rate is the rate that works. Conditioned
        // on the recorded answerer, not on ref_pass_left == 0 alone (0 also means "no pass").
        clear_fault(omgp::TRUNK_bit_rate_fallback, now_us);
    }

    evaluate_declare();
}

void HealthTracker::apply_result(uint8_t addr, bool ok, uint64_t now_us) {
    HealthRecord& r = records_[addr];
    switch (r.state) {
    case HealthState::UNENROLLED:
        // trunk §6: UNENROLLED never counts failures, only a valid result enrols it.
        if (ok) {
            r.state = HealthState::ENROLLED;
            r.consecutive_failures = 0;
            notify(Notice::ENROLLED, addr);
        }
        break;
    case HealthState::ENROLLED:
        if (ok) {
            r.consecutive_failures = 0;
        } else if (++r.consecutive_failures >= omgp::TRUNK_suspect_after_failures) {
            r.state = HealthState::SUSPECT;
            r.suspect_since_us = now_us;
            notify(Notice::SUSPECT, addr);
        }
        break;
    case HealthState::SUSPECT:
        if (ok) {
            r.state = HealthState::ENROLLED;
            r.consecutive_failures = 0;
            notify(Notice::RECOVERED, addr);
        } else if (elapsed_us(now_us, r.suspect_since_us) >= kOfflineThresholdUs) {
            r.state = HealthState::OFFLINE;
            notify(Notice::OFFLINE, addr);
        }
        break;
    case HealthState::OFFLINE:
        if (ok) {
            r.state = HealthState::ENROLLED;
            r.consecutive_failures = 0;
            notify(Notice::RECOVERED, addr);
        }
        break;
    }
}

void HealthTracker::tick(uint64_t now_us) {
    // data-model.md §6: the only time-only transition is SUSPECT -> OFFLINE once
    // kOfflineThresholdUs has elapsed since suspect_since, with no on_result involved.
    // The loop bound is the array's own extent, BY CONSTRUCTION (an indexed bound's <=
    // mutant read records_[16], an intra-object overread ASan does not reliably flag),
    // and addr derives from the record's own position — &r - records_ cannot desync
    // under a future continue/break the way a parallel counter can. (History: #124.)
    for (HealthRecord& r : records_) {
        if (r.state == HealthState::SUSPECT &&
            elapsed_us(now_us, r.suspect_since_us) >= kOfflineThresholdUs) {
            r.state = HealthState::OFFLINE;
            notify(Notice::OFFLINE, static_cast<uint8_t>(&r - records_));
        }
    }
    // data-model.md §7: the declare rule is evaluated after every on_result AND every tick.
    // Labelled per rule 11: no tick can newly SATISFY it — the predicate reads "every
    // enrolled node is SUSPECT or OFFLINE", and the only transition above is SUSPECT ->
    // OFFLINE, which moves a node between two members of that set. This call is the data
    // model's evaluation point, not a reachable declaration path, so a mutant deleting it
    // survives by construction rather than for want of a test. (`statement_deletion` named a
    // mutator Mull has no such name for, so the label covered nothing and the survivor came
    // back unlabelled; `cxx_remove_void_call` is Mull's own name for this mutation.)
    evaluate_declare(); // mutant-ok(equivalent, cxx_remove_void_call): see above
}

HealthState HealthTracker::state(uint8_t addr) const {
    // Note: a non-node address reads as UNENROLLED, indistinguishable from a genuinely
    // unenrolled node — callers holding an address that on_result silently ignores see
    // a permanent UNENROLLED (documented per review on #118).
    if (!is_node_addr(addr))
        return HealthState::UNENROLLED;
    return records_[addr].state;
}

bool HealthTracker::poll_due(uint8_t addr, uint64_t now_us) const {
    if (!is_node_addr(addr))
        return false;
    // trunk §7 / data-model.md §6 (amended 2026-09-13, F4): no status polls while a fault is
    // declared — only next_probe() drives the wire, so a node that answered at the fallback
    // rate is never polled at the reference rate it cannot hear.
    if (bus_.fault)
        return false;
    const HealthRecord& r = records_[addr];
    switch (r.state) {
    case HealthState::ENROLLED:
        return true;
    case HealthState::SUSPECT:
        return elapsed_us(now_us, r.last_poll_us) >= kSuspectPollPeriod_us;
    default: // OFFLINE, UNENROLLED: reached only via next_probe's enrolment rotation
        return false;
    }
}

void HealthTracker::mark_polled(uint8_t addr, uint64_t now_us) {
    if (!is_node_addr(addr))
        return;
    records_[addr].last_poll_us = now_us;
}

uint8_t HealthTracker::next_backplane_addr(uint8_t addr) const {
    // In-range by construction (modulo the backplane count): no wrap COMPARISON exists to
    // get wrong, so the off-by-one that walked the cursor into records_[kAddrCount] — an
    // intra-object overflow ASan's default config does not flag — is unwritable here
    // (red-team on #118, mutant M12).
    constexpr uint8_t kBackplanes =
        static_cast<uint8_t>(omgp::ADDR_backplane_max - omgp::ADDR_backplane_min + 1);
    return static_cast<uint8_t>((addr - omgp::ADDR_backplane_min + 1) % kBackplanes +
                                omgp::ADDR_backplane_min);
}

Probe HealthTracker::pass_probe() {
    // trunk §7 reference pass: every enrolled address once, in address order, at the
    // reference rate, without alternating (data-model.md §6/§7, amended 2026-09-13).
    if (bus_.pass_addr != 0) {
        // The pass probe's outcome is still outstanding: hand back the SAME probe and
        // advance nothing. A scheduler that calls this at a superframe boundary with the
        // probe in flight (F3 obligation 2 violated) then still completes the pass; walking
        // the cursor past the probe on the wire would leave a pass that can never end
        // (round-13 red team on #472). Fail-safe under a violated assumption.
        //
        // The call below cannot change anything on this path, BY CONSTRUCTION: `pass_addr`
        // is non-zero only between the loop below issuing a pass probe at TRUNK_bit_rate and
        // that probe's outcome, and nothing can move the wire rate in between — next_probe()
        // routes here whenever fault && ref_pass_left > 0, and the two places that end a pass
        // (on_result's decrement, clear_fault) both zero `pass_addr` first. So wire_rate is
        // already TRUNK_bit_rate and note_wire_rate returns without touching rate_changes.
        // mutant-ok(equivalent, cxx_remove_void_call): a no-op call on this path.
        note_wire_rate(omgp::TRUNK_bit_rate);
        return Probe{bus_.pass_addr, omgp::TRUNK_bit_rate};
    }
    // The <= mutant of this bound adds one iteration, which changes nothing until all
    // kBackplaneCount addresses have come back UNENROLLED — the sentinel path below, which a
    // running pass cannot reach. Every reachable call returns from inside the loop, at the
    // same iteration either way. `++i` is a different matter: its -- mutant underflows
    // `i` to SIZE_MAX and ends the scan after ONE address, and is killed by "a pass whose
    // only enrolled address is the last one in the rotation" in test_link_busfault.cpp.
    // mutant-ok(accepted, cxx_lt_to_le): differs only on the unreachable exhaustion path.
    for (size_t i = 0; i < kBackplaneCount; ++i) {
        const uint8_t addr = bus_.pass_next_addr;
        bus_.pass_next_addr = next_backplane_addr(addr);
        if (records_[addr].state != HealthState::UNENROLLED) { // enrolled: ever answered
            bus_.pass_addr = addr;
            note_wire_rate(omgp::TRUNK_bit_rate);
            return Probe{addr, omgp::TRUNK_bit_rate};
        }
    }
    // Unreachable while a pass is running: a pass starts only during a fault, a fault is
    // declared only with at least one enrolled node, and no transition during a pass
    // un-enrols one. Returning the sentinel rather than spinning keeps the "nothing to
    // probe" contract if that ever changes (rule 11: a guard, not a property).
    // mutant-ok(accepted, cxx_remove_void_call): on the unreachable path described above.
    note_wire_rate(omgp::TRUNK_bit_rate);
    return Probe{omgp::ADDR_host, omgp::TRUNK_bit_rate};
}

Probe HealthTracker::next_probe(uint64_t /*now_us*/) {
    if (bus_.fault && bus_.ref_pass_left > 0)
        return pass_probe();

    // trunk §7 / FR-025: while faulted the probes alternate, STARTING at the fallback rate;
    // data-model.md §7 keeps `bus_.bit_rate` untouched until a clear, so a fault-time probe's
    // rate is the probe's own (round-9 red team on #472).
    uint32_t rate = bus_.bit_rate;
    if (bus_.fault) {
        rate = bus_.next_probe_fallback ? omgp::TRUNK_bit_rate_fallback : omgp::TRUNK_bit_rate;
        bus_.next_probe_fallback = !bus_.next_probe_fallback;
    }
    note_wire_rate(rate);

    // data-model.md §6: round-robin over UNENROLLED/OFFLINE addresses in
    // [ADDR_backplane_min, ADDR_backplane_max] (0x00 is the host and is never a candidate) —
    // plus, while faulted, every SUSPECT address, so the probes reach the nodes whose silence
    // declared the fault at once instead of only after they age to OFFLINE (amended
    // 2026-09-13, F4). Bounded by the full backplane range so the scan can never spin.
    for (size_t i = 0; i < kBackplaneCount; ++i) {
        next_probe_addr_ = next_backplane_addr(next_probe_addr_);
        const HealthState s = records_[next_probe_addr_].state;
        if (s == HealthState::UNENROLLED || s == HealthState::OFFLINE ||
            (bus_.fault && s == HealthState::SUSPECT))
            return Probe{next_probe_addr_, rate};
    }
    // No eligible address (e.g. every backplane node is ENROLLED/SUSPECT — the steady
    // state of a healthy rig): ADDR_host (0x00) is outside the rotation range and is
    // asserted by tests/unit/test_link_health.cpp to never be a real probe target, so it
    // signals "nothing to probe" without adding a field the contract doesn't declare.
    // Unreachable while faulted: no address is ENROLLED then and every other state is a
    // candidate, so the at-least-one enrolled node the declare rule requires is always one.
    return Probe{omgp::ADDR_host, rate};
}

void HealthTracker::note_wire_rate(uint32_t bit_rate) {
    // data-model.md §8: rate_changes is incremented at the point the change is decided —
    // when a probe goes out at a rate other than the last one used, or when a clear pins a
    // new rate. Counting calls instead would count phantom changes.
    if (bit_rate == bus_.wire_rate)
        return;
    bus_.wire_rate = bit_rate;
    ++stats_.rate_changes;
}

void HealthTracker::evaluate_declare() {
    // trunk §7 / data-model.md §7: declare when at least one node is enrolled (has ever
    // answered — FR-023) and every enrolled node is SUSPECT or OFFLINE. A single enrolled
    // node counts as all (ruling Q2, human 2026-08-29).
    if (bus_.fault)
        return;
    uint8_t enrolled = 0;
    uint8_t failing = 0;
    for (const HealthRecord& r : records_) { // records_[ADDR_host] is never written (§5)
        if (r.state == HealthState::UNENROLLED)
            continue;
        ++enrolled;
        if (r.state != HealthState::ENROLLED)
            ++failing;
    }
    if (enrolled == 0 || failing != enrolled)
        return;

    bus_.fault = true;
    bus_.next_probe_fallback = true; // FR-025: re-probing starts at the fallback rate
    // The four pass fields are reset per episode. cxx_assign_const writes the type's zero
    // value, so each `= 0` here is byte-for-byte identical to its mutant. (`ref_pass_left`
    // carries no label: Mull emitted no mutant at that statement in this function — see the
    // #530 deep-verify report — and an unused label is reported as stale.)
    // mutant-ok(equivalent, cxx_assign_const): the mutation and the original coincide.
    bus_.fallback_answerer = 0;
    bus_.ref_pass_left = 0;
    // mutant-ok(equivalent, cxx_assign_const): the mutation and the original coincide.
    bus_.pass_addr = 0;
    // Unlike the three above, this RHS is not zero — but the write is dead in every reachable
    // execution, so its mutant is still indistinguishable: pass_next_addr is read only by
    // pass_probe(), reached only while ref_pass_left > 0, and the sole place that makes
    // ref_pass_left non-zero (on_result's pass start) re-writes pass_next_addr in the same
    // block. `ref_pass_left = 0` two lines up is what makes that hold on entry to the episode.
    // mutant-ok(equivalent, cxx_assign_const): a dead write; no reachable read sees it.
    bus_.pass_next_addr = omgp::ADDR_backplane_min;
    ++stats_.bus_faults;
    notify(Notice::BUS_FAULT, kBusAddr);
    notify(Notice::ALERT, kBusAddr); // FR-024: one system alert per episode
}

void HealthTracker::clear_fault(uint32_t bit_rate, uint64_t now_us) {
    // trunk §7: the fault clears exactly once per episode, at the rate that worked, and the
    // rate in use then stands until the layer above or a human changes it — there is NO
    // automatic return from the fallback rate (ruling 2026-09-13).
    //
    // This bool and the three uint8_t resets at the end of this function are assigned their
    // own type's zero value, which is exactly what cxx_assign_const substitutes: each mutated
    // statement is byte-for-byte identical to the one it replaces, so no test can tell them
    // apart. (In evaluate_declare() the same fields are assigned `true`/ADDR_backplane_min,
    // where the rewrite is NOT identical and the mutants are killed or argued separately.)
    // mutant-ok(equivalent, cxx_assign_const): the mutation and the original coincide.
    bus_.fault = false;
    bus_.bit_rate = bit_rate;
    note_wire_rate(bit_rate);
    // The deferred §6 transition of a fallback-rate answerer is applied BEFORE the pass state
    // is reset and before the declare rule is next evaluated — resetting first would lose the
    // answerer the clear still needs, and the rig would be re-declared faulty at once
    // (round-8 red team on #472). On a clear at the reference rate the recorded answerer is
    // dropped un-applied: it keeps the state it had, since it cannot hear the reference rate
    // and §6 will find it in its own time (data-model.md §7).
    if (bit_rate == omgp::TRUNK_bit_rate_fallback && bus_.fallback_answerer != 0)
        apply_result(bus_.fallback_answerer, true, now_us);
    // mutant-ok(equivalent, cxx_assign_const): the mutation and the original coincide.
    bus_.fallback_answerer = 0;
    // mutant-ok(equivalent, cxx_assign_const): the mutation and the original coincide.
    bus_.ref_pass_left = 0;
    // mutant-ok(equivalent, cxx_assign_const): the mutation and the original coincide.
    bus_.pass_addr = 0;
    notify(Notice::BUS_RECOVERED, kBusAddr);
}

bool HealthTracker::bus_fault() const {
    return bus_.fault;
}

uint32_t HealthTracker::bit_rate() const {
    return bus_.bit_rate;
}

const BusStats& HealthTracker::bus_stats() const {
    return stats_;
}

} // namespace link
} // namespace omgp
