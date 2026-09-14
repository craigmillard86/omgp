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
// BusState::probe_fallback (health.hpp) is one bit per address: bit N is address N. Every
// caller passes an address that has already passed is_node_addr or that came from the
// rotation cursor, so the shift is in range BY CONSTRUCTION — and the assert below is what
// keeps "in range" true if the address space ever grows past the width of that field.
static_assert(kAddrCount <= 16, // literal-ok: the bit width of that uint16_t field
              "BusState::probe_fallback holds one bit per address");
constexpr uint16_t probe_bit(uint8_t addr) {
    return static_cast<uint16_t>(1u << addr);
}
// How long a probe can be outstanding, measured from its issue (next_probe's now_us): the
// request's own transmission, the in-flight hold of one worst-case frame, and the response
// window, all at the slowest rate (contracts/link-cpp.md "Behaviour": the window is
// [tx_end, tx_end + T_resp) and the hold extends it by max_frame; trunk §3/§9). Spec symbols
// only (CLAUDE.md rule 4): 2 x kMaxWire x byte_time_us(TRUNK_bit_rate_fallback) + TRUNK_T_resp_us
// ≈ 24.6 ms. One bound for both rates — a reference-rate probe is written off later than it
// need be, never earlier; the harm the bound exists for (an abandoned record frozen for ever)
// is on the long side. The epoch is the issue instant, so a probe ISSUED but never transmitted
// is covered too (round-9 red team on #530, finding 1).
constexpr uint64_t kOutcomeWindowUs =
    2ull * kMaxWire * byte_time_us(omgp::TRUNK_bit_rate_fallback) + omgp::TRUNK_T_resp_us;

// Every address's bit at once, for evaluate_declare()'s per-episode reset. Derived from
// kAddrCount rather than written as a mask, so it stays the whole field if the address space
// grows — up to the width the static_assert above holds it to.
constexpr uint16_t kAllProbeBits = static_cast<uint16_t>((1u << kAddrCount) - 1u);
} // namespace

HealthTracker::HealthTracker(Clock& clock, HealthListener& listener)
    : clock_(clock), listener_(listener), records_{}, next_probe_addr_(omgp::ADDR_backplane_max),
      // Each BusState field's start-of-life value is declared beside the field in health.hpp,
      // so this is aggregate initialisation from those and not a positional list to keep in
      // step with the struct.
      bus_{}, stats_{} {
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

    // trunk §7 / data-model.md §7. The rate this outcome arrived at is the rate the last probe
    // TO THIS ADDRESS went out at (bus_.probe_fallback, written by note_probe below) —
    // knowable only because probes are the only traffic while a fault is declared (F3
    // obligation 1, contracts/link-cpp.md "What F3/F4 need", ASSUMED here, not enforced).
    // Per address, not the rate last put on the wire: one next_probe() call with this outcome
    // still outstanding (obligation 2 violated by a single superframe) moves the wire to
    // another address's probe, and reading this answer at that rate inverts §7's two clear
    // rules — upward it enrols a node at a rate it cannot hear and oscillates declare/clear,
    // downward it pins the trunk at the fallback rate with no automatic return (red team round
    // 2 on #530, finding 1; the two "an extra next_probe() call ..." cases in
    // tests/unit/test_link_busfault.cpp). And per episode, EXCEPT where a probe is still
    // outstanding: evaluate_declare() resets the bit of every address that owes this layer no
    // outcome to the rate in use — such an outcome answers something issued before the declare,
    // at that rate, and must not be read at a bit some earlier episode left behind (red team
    // round 5 on #530, finding 1) — while an address with a probe in flight keeps its bit,
    // because for it that bit is the only record of a rate already on the wire and the new
    // episode's rate in use says nothing about it (red team round 6 on #530, finding 1).
    // `bus_.probe_live` is what tells the two apart, and this is where a record is consumed.
    // What this still cannot tell apart, stated (rule 11): more than one transaction
    // outstanding to the SAME address at once — the newest probe's rate is what the first
    // outcome back is read at. That is obligation 1's territory, ASSUMED, not enforced.
    // Outside a fault the rate plays no part.
    const bool at_reference = (bus_.probe_fallback & probe_bit(addr)) == 0;
    const bool fallback_answer = bus_.fault && ok && !at_reference;
    // The probe this outcome answers is no longer in flight, so its record stops being the
    // one thing a new episode must not overwrite. Unconditional: an outcome for an address
    // with no outstanding probe clears a bit that is already clear.
    bus_.probe_live = static_cast<uint16_t>(bus_.probe_live & ~probe_bit(addr));

    if (fallback_answer) {
        // §7: a fallback-rate answer neither clears the fault nor moves that node's state —
        // enrolling it here would have it status-polled at a rate it cannot hear, fail into
        // SUSPECT and re-declare the fault it just recovered (round-4 red team on #472). It
        // is recorded, and a reference pass over every enrolled address starts.
        // What IS true by construction: no outcome for an address whose PASS probe is
        // outstanding reaches here, because every pass probe goes out at the reference rate
        // (pass_probe below). The wider claim this comment once made — that no pass can
        // already be running — is false, and labelled so per rule 11 (red team round 2 on
        // #530, finding 2): a pass is running from the moment ref_pass_left is set below, and
        // until its first probe goes out the answerer's remembered rate is still the fallback
        // one, so a duplicate or late outcome for it re-enters this branch and recomputes the
        // pass. Benign, and demonstrated so rather than asserted: |enrolled| cannot change in
        // that window and the cursor is already at ADDR_backplane_min, so the recomputation
        // reproduces the state it overwrites — "a duplicate fallback answer before the first
        // pass probe restarts an identical pass" in tests/unit/test_link_busfault.cpp.
        bus_.fallback_answerer = addr;
        uint8_t enrolled = 0;
        for (const HealthRecord& r : records_)
            if (r.state != HealthState::UNENROLLED)
                ++enrolled;
        bus_.ref_pass_left = enrolled; // exactly |enrolled| probes (FR-026)
        bus_.pass_addr = 0;
        // The cursor starts AT ADDR_backplane_min, and nowhere else. cxx_assign_const rewrites
        // this RHS to a constant that is NOT the type's zero value — demonstrated on #530: the
        // zero rewrite is killed by "a pass whose only enrolled address is the last one in the
        // rotation" (a scan from below the rotation spends one of pass_probe()'s
        // kBackplaneCount iterations on records_[ADDR_host] and stops one address short of
        // ADDR_backplane_max), yet the mutant survived that test. Mull documents 42 as the
        // constant, and a rewrite to 42 reproduces the survivor exactly — the whole unit suite
        // passes with it — so the two cases in tests/unit/test_link_busfault.cpp pin the start
        // from both sides: that one fails for a cursor starting BELOW ADDR_backplane_min, and
        // "a pass over the first and last addresses of the rotation probes them in that order"
        // fails for one starting ABOVE it (a scan from higher up meets ADDR_backplane_max
        // before it wraps round to ADDR_backplane_min). Both demonstrated by those named tests;
        // that 42 is the constant Mull emits is ASSUMED, and nothing here depends on its value.
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
    // A bool initialised `false`. cxx_init_const rewrites the RHS to a constant that is not
    // the type's zero value (see the pass start above), but a bool is read back as `value & 1`
    // — so the mutant either reads `false`, coinciding with the original, or reads `true` and
    // fires the pass-end clear on the FIRST outcome after any fallback answer, which the
    // multi-probe pass cases in tests/unit/test_link_busfault.cpp assert does not happen. It
    // survived them, so it reads `false`. (The same rewrite is recorded on
    // `bool belief_stale = false` in link/responder.cpp poll().)
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
    } else if (pass_ended) {
        // §7: the pass drew nothing, so the fallback rate is the rate that works. `pass_ended`
        // is the whole condition, not `ref_pass_left == 0` (which also means "no pass"): it is
        // set only by the decrement above, i.e. by the outcome of a pass probe. The
        // "&& bus_.fallback_answerer != 0" conjunct this line used to carry was removed as a
        // condition no test could falsify (review round 3 on #530, finding 3(a)): a pass runs
        // only while ref_pass_left > 0, which is set only in the fallback branch above, which
        // records a non-zero answerer in the same block, and clear_fault/evaluate_declare zero
        // the pair together — so the conjunct was true whenever `pass_ended` was. VERIFIED BY
        // CONSTRUCTION over this file, which is a control on the current code, not a language
        // guarantee. clear_fault still guards its own deferred apply_result on the answerer.
        // No divergence from data-model §7, which writes the rule as "fallback_answerer ≠ 0 and
        // the pass outcome that takes ref_pass_left to 0 is itself a failure": the warning it
        // attaches is against `ref_pass_left == 0` ALONE, which is not what is tested here —
        // `pass_ended` says a pass probe's own outcome ended a running pass, and only a pass
        // started by a recorded answerer can run.
        clear_fault(omgp::TRUNK_bit_rate_fallback, now_us);
    }

    evaluate_declare(now_us);
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
    evaluate_declare(now_us); // mutant-ok(equivalent, cxx_remove_void_call): see above
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

Probe HealthTracker::pass_probe(uint64_t now_us) {
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
        // By the same construction `pass_addr`'s remembered probe rate is already the
        // reference rate — the loop below cleared its bit when it issued this very probe, and
        // only a probe going out moves a bit — so there is no note_probe() call here: it could
        // only restate that, and a call no test could falsify is what this file keeps out.
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
            note_probe(addr, omgp::TRUNK_bit_rate, now_us);
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

Probe HealthTracker::next_probe(uint64_t now_us) {
    if (bus_.fault && bus_.ref_pass_left > 0)
        return pass_probe(now_us);

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
            (bus_.fault && s == HealthState::SUSPECT)) {
            note_probe(next_probe_addr_, rate, now_us);
            return Probe{next_probe_addr_, rate};
        }
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

void HealthTracker::note_probe(uint8_t addr, uint32_t bit_rate, uint64_t now_us) {
    // data-model.md §7 needs "a valid answer at the reference rate" told from one at the
    // fallback rate, and on_result carries no rate — so each probe leaves the rate it went out
    // at on its own address (health.hpp, BusState::probe_fallback). Per address, because the
    // wire moves on: while a probe to this address is outstanding, another next_probe() call
    // (F3 obligation 2 violated) puts a different address's probe on the wire at the other
    // rate, and the classification must not follow it (red team round 2 on #530, finding 1).
    // Every caller is a probe about to be issued for a rotation address, in range by the
    // static_assert above; ADDR_host, which is the "nothing to probe" sentinel and never goes
    // on the wire, is not one of them.
    if (bit_rate == omgp::TRUNK_bit_rate_fallback)
        bus_.probe_fallback = static_cast<uint16_t>(bus_.probe_fallback | probe_bit(addr));
    else
        bus_.probe_fallback = static_cast<uint16_t>(bus_.probe_fallback & ~probe_bit(addr));
    // …and the record is owed an outcome from here until on_result reads it — or until the
    // outcome window closes. That is what exempts this address from evaluate_declare()'s
    // per-episode reset (health.hpp, BusState::probe_live; red team round 6 on #530, finding
    // 1), for kOutcomeWindowUs from now (round 7, finding 1: unbounded, an abandoned probe
    // froze the record for ever; round 9: per address, and the whole transaction's window,
    // not T_resp from issue). The stamp is this address's own: a single stamp for the set was
    // re-armed by every later probe to any other address.
    // Only the newest handout is live: F3 obligation 1 puts one transaction on the wire at a
    // time, so a handout to any address means the previous probe's transaction is over — its
    // outcome was consumed, or it was abandoned and will never be read. A live bit that
    // survived a later handout is exactly the abandoned record round 7 and round 9 (finding 2:
    // an unrelated discovery probe kept it alive) describe. The time bound below is for the
    // other case: an abandoned probe with no later handout at all.
    bus_.probe_live = probe_bit(addr);
    records_[addr].probe_issued_us = now_us;
}

void HealthTracker::evaluate_declare(uint64_t now_us) {
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
    // The four pass fields are reset per episode. cxx_assign_const does NOT write the type's
    // zero value (demonstrated at on_result's pass start above), so each label below argues
    // from the write being DEAD — true whatever constant is substituted — never from the
    // mutant being byte-identical. (`ref_pass_left` carries no label: Mull emitted no mutant
    // at that statement in this function — see the #530 deep-verify report — and an unused
    // label is reported as stale.)
    //
    // Dead: fallback_answerer is read only where a pass has ended or a fallback-rate clear is
    // under way, and both follow on_result's pass start, which assigns it there.
    // mutant-ok(equivalent, cxx_assign_const): overwritten before any reachable read.
    bus_.fallback_answerer = 0;
    bus_.ref_pass_left = 0;
    // Dead the same way: pass_probe() reads pass_addr only while ref_pass_left > 0, which the
    // line above makes false until on_result's pass start, and that zeroes pass_addr itself.
    // The one read this does not cover is on_result's `pass_addr == addr`, which a constant
    // INSIDE [ADDR_backplane_min, ADDR_backplane_max] would make fire without a pass and
    // underflow ref_pass_left — demonstrated not to be this constant for the addresses the
    // cases in tests/unit/test_link_busfault.cpp drive outcomes for during a declared fault
    // (kNodeA..kNodeC and ADDR_backplane_max); for the rest of the rotation, ASSUMED.
    // mutant-ok(equivalent, cxx_assign_const): overwritten before any reachable read.
    bus_.pass_addr = 0;
    // Dead in every reachable execution: pass_next_addr is read only by pass_probe(), reached
    // only while ref_pass_left > 0, and the sole place that makes ref_pass_left non-zero
    // (on_result's pass start) re-writes pass_next_addr in the same block. `ref_pass_left = 0`
    // above is what makes that hold on entry to the episode.
    // mutant-ok(equivalent, cxx_assign_const): a dead write; no reachable read sees it.
    bus_.pass_next_addr = omgp::ADDR_backplane_min;
    // The remembered probe rates are per EPISODE as well as per address, and unlike the four
    // resets above this one is LIVE: on_result classifies every fault-time outcome by the bit
    // for its address, and an outcome can arrive for an address this episode has not probed.
    // F3 obligation 1 does not exclude that — it is an obligation to ISSUE ("while bus_fault()
    // is true, issue only the probe next_probe() returns"), and the fault is declared BY an
    // outcome, on the line below, so a status poll already on the wire in the superframe that
    // declares it necessarily lands afterwards and cannot be un-issued. It went out at the rate
    // in use, which is what those addresses' bits are set to here; a bit a PREVIOUS episode left
    // behind is some other episode's rate, and reading an answer at it inverts both of §7's
    // clear rules (red team round 5 on #530, finding 1). To the rate in use and not simply
    // cleared: after a fallback-rate clear that rate IS the rate in use (no automatic return,
    // ruling 2026-09-13), so a cleared bit would be just as wrong in the other direction.
    //
    // `probe_live` is the exception, and it is not a refinement but the other half of the rule
    // (red team round 6 on #530, finding 1): for an address whose probe is still in flight the
    // remembered bit is the rate of a frame ALREADY on the wire, which no later declare can
    // change, so overwriting it inverts exactly the same two clear rules the reset exists to
    // protect — a fallback-rate probe answered after the boundary would clear the new episode
    // at the reference rate and enrol a node at a rate it has just failed to hear. The rate in
    // use is the right answer only where this layer is owed nothing.
    // All four combinations are DEMONSTRATED, one case each, by the two "a new episode ..." and
    // two "... outstanding across an episode boundary" / "... answered in the next episode"
    // cases in tests/unit/test_link_busfault.cpp.
    // …and only while that probe can still be answered (HealthRecord::probe_issued_us): past
    // kOutcomeWindowUs the outcome is not coming, the record is owed nothing, and honouring it
    // would freeze a dead frame's rate into this and every later episode (round 7, finding 1;
    // round 9: judged per address against the slowest transaction's whole window, because a
    // bound of T_resp from issue wrote off every fallback-rate probe older than 200 us — its
    // request alone takes longer — and one stamp for the set was re-armed by any later probe).
    // Demonstrated by the abandoned-probe, sweep and discovery-probe cases in
    // tests/unit/test_link_busfault.cpp; the in-window half by the two boundary cases.
    for (uint8_t a = omgp::ADDR_backplane_min; a <= omgp::ADDR_backplane_max; ++a)
        if ((bus_.probe_live & probe_bit(a)) != 0 &&
            elapsed_us(now_us, records_[a].probe_issued_us) > kOutcomeWindowUs)
            bus_.probe_live = static_cast<uint16_t>(bus_.probe_live & ~probe_bit(a));
    const uint16_t in_use = bus_.bit_rate == omgp::TRUNK_bit_rate_fallback ? kAllProbeBits : 0;
    bus_.probe_fallback = static_cast<uint16_t>((bus_.probe_fallback & bus_.probe_live) |
                                                (in_use & ~bus_.probe_live));
    ++stats_.bus_faults;
    notify(Notice::BUS_FAULT, kBusAddr);
    notify(Notice::ALERT, kBusAddr); // FR-024: one system alert per episode
}

void HealthTracker::clear_fault(uint32_t bit_rate, uint64_t now_us) {
    // trunk §7: the fault clears exactly once per episode, at the rate that worked, and the
    // rate in use then stands until the layer above or a human changes it — there is NO
    // automatic return from the fallback rate (ruling 2026-09-13).
    //
    // cxx_assign_const does not substitute the type's zero value (demonstrated at on_result's
    // pass start), so this bool and the three uint8_t resets at the end of this function are
    // argued one by one, not as a block. This one is a bool, read back as `value & 1`: the
    // mutant either reads `false`, coinciding with the original, or leaves the fault standing
    // through every clear — which every REQUIRE_FALSE(bus_fault()) in
    // tests/unit/test_link_busfault.cpp would catch. It survived them, so it reads `false`.
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
    // The three resets are dead writes whatever constant replaces them: the fault is clear
    // from here, so next_probe()'s pass branch and on_result()'s fallback branch are both shut
    // until the next declare, and evaluate_declare() rewrites fallback_answerer and
    // ref_pass_left before a pass can start — which then rewrites pass_addr itself.
    // mutant-ok(equivalent, cxx_assign_const): overwritten before any reachable read.
    bus_.fallback_answerer = 0;
    // mutant-ok(equivalent, cxx_assign_const): overwritten before any reachable read.
    bus_.ref_pass_left = 0;
    // Same caveat as the pass_addr reset in evaluate_declare(): on_result's
    // `pass_addr == addr` is the one read no rewrite is shut out of, and the node addresses
    // the cases in tests/unit/test_link_busfault.cpp drive outcomes for are demonstrated not
    // to be this constant. For the rest of the rotation, ASSUMED.
    // mutant-ok(equivalent, cxx_assign_const): overwritten before any reachable read.
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
