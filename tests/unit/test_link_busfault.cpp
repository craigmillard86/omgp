// trunk §7 "Bus health": pins HealthTracker's BUS_FAULT declare rule, the alternating-rate
// re-probe, the reference pass and the two clear rules of
// specs/002-trunk-link-layer/data-model.md §7 (amended 2026-09-13, F4 ruling 2026-09-13 —
// docs/OPEN-QUESTIONS.md "F4's two deferred values"), with an explicit FakeClock and a
// recording HealthListener. Written before link/health.cpp implements any of it
// (CLAUDE.md rule 8; spec 002 T041, US5).
//
// Contract: specs/002-trunk-link-layer/contracts/link-cpp.md "Health tracker".
// Per-address ENROLLED/SUSPECT/OFFLINE lifecycle (US4) is tests/unit/test_link_health.cpp's
// subject; this file drives it only far enough to reach the bus-level rules.
//
// Two obligations these tests honour because F3 owes them to the tracker
// (contracts/link-cpp.md "What F3/F4 need"; they are ASSUMPTIONS of the rule, not
// properties this layer enforces): while a fault is declared, probes are the only traffic —
// every on_result below is the outcome of the probe next_probe() last returned — and
// next_probe() is called exactly once per probe issued.
#include "catch_amalgamated.hpp"
#include "fake_clock.hpp"
#include "heap_guard.hpp"
#include "link/health.hpp"
#include "link/link_types.hpp"
#include "omgp_protocol.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace omgp::link;
using omgp_test::FakeClock;

namespace {

// Records every (Notice, addr) in delivery order, so a test can assert content, order and
// count (SC-006: exactly one notification per transition; FR-024: one BUS_FAULT per episode).
struct RecordingListener : HealthListener {
    struct Entry {
        Notice notice;
        uint8_t addr;
    };
    std::vector<Entry> entries;
    bool overrun_noted = false;

    // Reserved up front so a HEAP_FREE_SCOPE measures HealthTracker's allocations, not this
    // harness's vector doubling; an overrun says so rather than being blamed on the tracker
    // (the same trap reviews on #124 sprang in test_link_health.cpp). 64 covers the longest
    // scripted episode here.
    RecordingListener() {
        entries.reserve(64);
    }

    void on_notice(Notice notice, uint8_t addr) override {
        if (entries.size() == entries.capacity() && !overrun_noted) {
            overrun_noted = true;
            std::fprintf(stderr,
                         "RecordingListener: reserve(%zu) outgrown - undersized "
                         "reserve or a runaway notice storm\n",
                         entries.capacity());
        }
        entries.push_back({notice, addr});
    }

    size_t count(Notice notice) const {
        size_t n = 0;
        for (const Entry& e : entries)
            if (e.notice == notice)
                ++n;
        return n;
    }
};

constexpr uint8_t kNodeA = omgp::ADDR_backplane_min;     // 0x01
constexpr uint8_t kNodeB = omgp::ADDR_backplane_min + 1; // 0x02
constexpr uint8_t kNodeC = omgp::ADDR_backplane_min + 2; // 0x03
// Never enrolled by ThreeNodeRig: the enrolment rotation probes it, the reference pass (which
// covers only addresses that have ever answered) skips it, so a probe to it can stay
// outstanding across a whole episode.
constexpr uint8_t kNodeD = omgp::ADDR_backplane_min + 3; // 0x04

// data-model.md §9: a bus-level notice carries addr 0, never a node address.
constexpr uint8_t kBusAddr = 0;

constexpr uint64_t kThresholdUs = uint64_t{omgp::TRUNK_offline_after_suspect_ms} * 1000;

// One valid response enrols an address (data-model.md §6, UNENROLLED | ok -> ENROLLED).
void enrol(HealthTracker& tracker, uint8_t addr, uint64_t at_us) {
    tracker.on_result(addr, true, at_us);
}

// Drives an ENROLLED address to SUSPECT with exactly TRUNK_suspect_after_failures
// consecutive failures, the last of them at `at_us` — so `at_us` is its suspect_since.
void fail_to_suspect(HealthTracker& tracker, uint8_t addr, uint64_t at_us) {
    for (uint32_t i = omgp::TRUNK_suspect_after_failures; i > 0; --i)
        tracker.on_result(addr, false, at_us - (i - 1));
}

// Issues probes the way F3 must (one next_probe() call per probe put on the wire) until the
// probe that goes out is for `addr` at `rate`, and returns it. Bounded by construction: the
// fault-time rotation covers all 15 backplane addresses (nothing is ENROLLED while a fault
// is declared) and the alternation has period 2, so every (address, rate) pair recurs within
// 2 x 15 calls — 15 being odd is what makes the two cycles co-prime.
Probe probe_until(HealthTracker& tracker, uint8_t addr, uint32_t rate) {
    constexpr int kCycle = 2 * (omgp::ADDR_backplane_max - omgp::ADDR_backplane_min + 1);
    for (int i = 0; i < kCycle; ++i) {
        const Probe p = tracker.next_probe(0);
        if (p.addr == addr && p.bit_rate == rate)
            return p;
    }
    FAIL("no probe for that address at that rate within one full rotation x alternation cycle");
    return Probe{omgp::ADDR_host, rate};
}

// The three-node rig every episode below starts from: A, B and C enrolled at t = 0, then
// each driven to SUSPECT one millisecond apart so their OFFLINE clocks are distinguishable
// (data-model.md §6: suspect_since is per record). The fault is declared by C's SUSPECT
// transition — the moment the last enrolled node stops answering.
struct ThreeNodeRig {
    static constexpr uint64_t kSuspectA = 1'000;
    static constexpr uint64_t kSuspectB = 2'000;
    static constexpr uint64_t kSuspectC = 3'000;

    static void build(HealthTracker& tracker) {
        enrol(tracker, kNodeA, 0);
        enrol(tracker, kNodeB, 0);
        enrol(tracker, kNodeC, 0);
        fail_to_suspect(tracker, kNodeA, kSuspectA);
        fail_to_suspect(tracker, kNodeB, kSuspectB);
        fail_to_suspect(tracker, kNodeC, kSuspectC);
    }
};

} // namespace

// ---------------------------------------------------------------- declare rule (FR-024) ---

TEST_CASE("three enrolled nodes SUSPECT at once declare BUS_FAULT once, with one ALERT", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);

    REQUIRE(tracker.bus_fault());
    REQUIRE(listener.count(Notice::BUS_FAULT) == 1);
    REQUIRE(listener.count(Notice::ALERT) == 1);
    REQUIRE(tracker.bus_stats().bus_faults == 1);

    // The two bus notices are adjacent and last: nothing else transitions on that result.
    REQUIRE(listener.entries[listener.entries.size() - 2].notice == Notice::BUS_FAULT);
    REQUIRE(listener.entries[listener.entries.size() - 2].addr == kBusAddr);
    REQUIRE(listener.entries.back().notice == Notice::ALERT);
    REQUIRE(listener.entries.back().addr == kBusAddr);

    // FR-024 "once per episode", not once per failing result: the nodes keep failing.
    for (uint64_t i = 1; i <= 6; ++i)
        tracker.on_result(kNodeA, false, ThreeNodeRig::kSuspectC + i);
    REQUIRE(listener.count(Notice::BUS_FAULT) == 1);
    REQUIRE(listener.count(Notice::ALERT) == 1);
    REQUIRE(tracker.bus_stats().bus_faults == 1);
}

TEST_CASE("a strict subset of the enrolled nodes failing declares nothing", "[link]") {
    // spec.md US5 acceptance scenario 2: two enrolled nodes, one SUSPECT, its peer still
    // ENROLLED -> the Story 4 state machine alone, no bus-level conclusion.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    enrol(tracker, kNodeA, 0);
    enrol(tracker, kNodeB, 0);
    fail_to_suspect(tracker, kNodeA, 1'000);

    REQUIRE(tracker.state(kNodeA) == HealthState::SUSPECT);
    REQUIRE(tracker.state(kNodeB) == HealthState::ENROLLED);
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(listener.count(Notice::BUS_FAULT) == 0);
    REQUIRE(listener.count(Notice::ALERT) == 0);
    REQUIRE(tracker.bus_stats().bus_faults == 0);

    // Ageing that one node to OFFLINE is still a strict subset — still no fault.
    tracker.tick(1'000 + kThresholdUs);
    REQUIRE(tracker.state(kNodeA) == HealthState::OFFLINE);
    REQUIRE(tracker.state(kNodeB) == HealthState::ENROLLED);
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bus_stats().bus_faults == 0);
}

TEST_CASE("a single enrolled node going SUSPECT declares BUS_FAULT (one node is all nodes)",
          "[link]") {
    // Ruling Q2 (human, 2026-08-29; docs/OPEN-QUESTIONS.md), FR-024: the fallback re-probe,
    // not the node count, is what tells a dead node from a dead bus.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    enrol(tracker, kNodeA, 0);
    fail_to_suspect(tracker, kNodeA, 1'000);

    REQUIRE(tracker.bus_fault());
    REQUIRE(listener.count(Notice::BUS_FAULT) == 1);
    REQUIRE(listener.count(Notice::ALERT) == 1);
    REQUIRE(tracker.bus_stats().bus_faults == 1);
}

TEST_CASE("addresses that never answered do not count towards the bus-fault rule", "[link]") {
    // FR-023 + the declare rule's |enrolled| >= 1: a rig where nothing has ever answered has
    // no enrolled node, so no amount of silence is a bus fault (it is an empty trunk).
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    HEAP_FREE_SCOPE({
        for (uint8_t a = omgp::ADDR_backplane_min; a <= omgp::ADDR_backplane_max; ++a)
            for (uint32_t i = 0; i < omgp::TRUNK_suspect_after_failures + 1; ++i)
                tracker.on_result(a, false, i);
    });

    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bus_stats().bus_faults == 0);
    REQUIRE(listener.entries.empty());
}

// ------------------------------------------- alternating re-probe (FR-025, FR-026) --------

TEST_CASE("while faulted next_probe alternates fallback, reference, fallback ... counting each "
          "change",
          "[timing:bit_rate_fallback]") {
    // FR-025: re-probing STARTS at the fallback rate; FR-026: it alternates thereafter.
    // data-model.md §8: rate_changes counts each change, at the point it is decided.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    // A control first: outside a fault the rotation never changes the rate, so rate_changes
    // counts rate changes and not next_probe() calls.
    enrol(tracker, kNodeA, 0);
    for (int i = 0; i < 4; ++i)
        REQUIRE(tracker.next_probe(0).bit_rate == omgp::TRUNK_bit_rate);
    REQUIRE(tracker.bus_stats().rate_changes == 0);

    fail_to_suspect(tracker, kNodeA, 1'000);
    REQUIRE(tracker.bus_fault());
    REQUIRE(tracker.bus_stats().rate_changes == 0); // declaring the fault issues no probe

    const uint32_t expected[6] = {omgp::TRUNK_bit_rate_fallback, omgp::TRUNK_bit_rate,
                                  omgp::TRUNK_bit_rate_fallback, omgp::TRUNK_bit_rate,
                                  omgp::TRUNK_bit_rate_fallback, omgp::TRUNK_bit_rate};
    for (uint32_t i = 0; i < 6; ++i) {
        const Probe p = tracker.next_probe(0);
        REQUIRE(p.bit_rate == expected[i]);
        REQUIRE(p.addr != omgp::ADDR_host); // a faulted trunk always has something to probe
        REQUIRE(tracker.bus_stats().rate_changes == i + 1);
    }

    // data-model.md §7: `bit_rate` is assigned only by a clear — the probe's rate is the
    // probe's own, so the rate in use is still the reference rate mid-fault.
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);
}

TEST_CASE("the fault-time rotation reaches SUSPECT addresses, not only OFFLINE/UNENROLLED ones",
          "[link]") {
    // data-model.md §6 (amended 2026-09-13, F4): while `fault` the candidate set also includes
    // every SUSPECT address, so the alternating probes reach the nodes whose silence declared
    // the fault at once, rather than only after they age to OFFLINE.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    REQUIRE(tracker.state(kNodeA) == HealthState::SUSPECT);

    bool seen_a = false;
    constexpr int kCycle = 2 * (omgp::ADDR_backplane_max - omgp::ADDR_backplane_min + 1);
    for (int i = 0; i < kCycle && !seen_a; ++i)
        seen_a = tracker.next_probe(0).addr == kNodeA;
    REQUIRE(seen_a);
}

TEST_CASE("no address is poll_due while a fault is declared", "[link]") {
    // data-model.md §6 (amended 2026-09-13, F4) and FR-026: no status polls while faulted —
    // only next_probe() drives the wire, so a node that answered at the fallback rate is
    // never polled at the reference rate it cannot hear.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    enrol(tracker, kNodeA, 0);
    tracker.mark_polled(kNodeA, 0);
    REQUIRE(tracker.poll_due(kNodeA, 0)); // ENROLLED: due, and the contrast this test needs

    fail_to_suspect(tracker, kNodeA, 1'000);
    REQUIRE(tracker.bus_fault());
    for (uint8_t a = omgp::ADDR_backplane_min; a <= omgp::ADDR_backplane_max; ++a)
        REQUIRE_FALSE(tracker.poll_due(a, 1'000 + kSuspectPollPeriod_us));

    // …and polling resumes once the fault clears (here at the reference rate, at once).
    const Probe p = probe_until(tracker, kNodeA, omgp::TRUNK_bit_rate);
    tracker.on_result(p.addr, true, 2'000);
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.poll_due(kNodeA, 2'000));
}

// ---------------------------------- an outcome is classified by ITS OWN probe's rate -------

TEST_CASE("an extra next_probe() call does not re-read a fallback-rate answer as a reference-rate "
          "one",
          "[timing:bit_rate_fallback]") {
    // data-model.md §7 (red team round 2 on #530, finding 1): `on_result` carries no bit rate,
    // so the rate an outcome arrived at is inferred. Inferring it from the rate last put on the
    // WIRE makes one extra next_probe() call — F3 obligation 2 violated by a single superframe —
    // re-read this fallback-rate answer as a reference-rate one: the fault would clear at the
    // reference rate and this node would be enrolled at a rate it cannot hear, fail back into
    // SUSPECT and re-declare the fault, without bound. The rate is therefore remembered PER
    // ADDRESS, so the extra call (which moves the wire to another address's probe) cannot
    // change how B's answer is read.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    probe_until(tracker, kNodeB, omgp::TRUNK_bit_rate_fallback);
    tracker.next_probe(0);                   // the extra call: no probe was issued for it
    tracker.on_result(kNodeB, true, 10'000); // B answers — at the FALLBACK rate

    REQUIRE(tracker.bus_fault());                           // §7: a fallback answer clears nothing
    REQUIRE(tracker.state(kNodeB) == HealthState::SUSPECT); // …and its §6 transition is deferred
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 0);

    // …and what it did start is the reference pass, which here draws nothing.
    uint64_t t = 11'000;
    for (int i = 0; i < 3; ++i) {
        const Probe p = tracker.next_probe(0);
        REQUIRE(p.bit_rate == omgp::TRUNK_bit_rate);
        tracker.on_result(p.addr, false, t += 1'000);
    }
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback);
    REQUIRE(tracker.state(kNodeB) == HealthState::ENROLLED); // the deferred transition, on clear
    REQUIRE(tracker.bus_stats().bus_faults == 1);            // one episode, not an oscillation
}

TEST_CASE("an extra next_probe() call does not re-read a reference-rate answer as a fallback-rate "
          "one",
          "[timing:bit_rate_fallback]") {
    // The other direction of the same finding: with the rate taken from the wire, one extra
    // next_probe() call makes this reference-rate answer look like a fallback one, so instead
    // of clearing at once (FR-026) the trunk runs a reference pass and — the pass drawing
    // nothing — is pinned at the fallback rate with no automatic return (ruling 2026-09-13):
    // the downgrade data-model §7's superseded Clear clause was replaced to prevent.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    probe_until(tracker, kNodeB, omgp::TRUNK_bit_rate);
    tracker.next_probe(0);                   // the extra call: no probe was issued for it
    tracker.on_result(kNodeB, true, 10'000); // B answers — at the REFERENCE rate

    REQUIRE_FALSE(tracker.bus_fault()); // FR-026: a reference-rate answer clears at once
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate); // not downgraded
    REQUIRE(tracker.state(kNodeB) == HealthState::ENROLLED);
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);
    REQUIRE(tracker.bus_stats().bus_faults == 1);
}

TEST_CASE("a reference-rate probe clears the fallback record of the address it goes to",
          "[timing:bit_rate_fallback]") {
    // The other half of note_probe's per-address record (link/health.cpp): setting the bit for
    // a fallback-rate probe is only one branch — the reference-rate branch must CLEAR that
    // address's bit, or the address stays marked "last heard at the fallback rate" for the
    // rest of the episode and its later reference-rate answer is read as a fallback one:
    // instead of clearing at once (FR-026) the trunk runs a reference pass, and a pass that
    // draws nothing leaves it pinned at the fallback rate (ruling 2026-09-13).
    //
    // No case above reaches this: each one probes its node at only ONE rate, and the two
    // "an extra next_probe() call ..." cases above turn on a reference-rate probe to a
    // DIFFERENT address leaving B's bit alone. So B is probed here at the fallback rate
    // first, is silent, and is probed again at the reference rate.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    const Probe fb = probe_until(tracker, kNodeB, omgp::TRUNK_bit_rate_fallback);
    REQUIRE(fb.addr == kNodeB);
    tracker.on_result(kNodeB, false, 5'000); // B is silent at the fallback rate

    const Probe ref = probe_until(tracker, kNodeB, omgp::TRUNK_bit_rate);
    REQUIRE(ref.addr == kNodeB); // the same address, now at the reference rate
    tracker.on_result(kNodeB, true, 10'000);

    REQUIRE_FALSE(tracker.bus_fault()); // FR-026: a reference-rate answer clears at once
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate); // not downgraded
    REQUIRE(tracker.state(kNodeB) == HealthState::ENROLLED);
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);
    REQUIRE(tracker.bus_stats().bus_faults == 1); // one episode, not an oscillation
}

TEST_CASE("a new episode reads an answer to a pre-declare poll at the rate that poll went out at",
          "[timing:bit_rate_fallback]") {
    // BusState::probe_fallback is per EPISODE as well as per address (link/health.cpp,
    // evaluate_declare). An outcome can arrive for an address the current episode has not
    // probed at all, and F3 obligation 1 does not exclude it: that obligation is one to ISSUE
    // ("while bus_fault() is true, issue only the probe next_probe() returns"), and the fault
    // is declared BY an outcome, at the tail of on_result — so a status poll already on the
    // wire in the superframe that declares it necessarily lands afterwards and cannot be
    // un-issued. That poll went out at the rate in use; a bit LEFT OVER from a previous
    // episode is some other episode's rate.
    //
    // Here A's bit is set by episode 1's fallback-rate probe and episode 1 clears at the
    // reference rate, so the bit outlives it. Read as the rate of A's answer in episode 2 it
    // inverts FR-026: the reference-rate answer clears nothing, starts a reference pass, and a
    // pass that draws nothing pins the trunk at the fallback rate with no automatic return
    // (ruling 2026-09-13) — from an answer that arrived at the reference rate.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    probe_until(tracker, kNodeA, omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeA, false, 5'000); // A is silent at the fallback rate: its bit is set
    probe_until(tracker, kNodeB, omgp::TRUNK_bit_rate);
    tracker.on_result(kNodeB, true, 6'000); // B clears episode 1 at the reference rate

    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate); // the rate in use from here
    REQUIRE(tracker.state(kNodeA) == HealthState::SUSPECT);

    // Episode 2: B is the only node answering, so when it stops every enrolled node is SUSPECT
    // again. A is SUSPECT and poll_due, so F3 polled it in that same superframe — at the rate
    // in use, the reference rate — and its answer lands after the declare.
    fail_to_suspect(tracker, kNodeB, 10'000);
    REQUIRE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);
    const size_t before = listener.entries.size();

    tracker.on_result(kNodeA, true, 10'001);

    REQUIRE_FALSE(tracker.bus_fault());                  // FR-026: it clears at once…
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate); // …at the reference rate, not downgraded
    REQUIRE(tracker.state(kNodeA) == HealthState::ENROLLED); // applied, not deferred to a pass
    REQUIRE(listener.entries.size() == before + 2);
    REQUIRE(listener.entries[before].notice == Notice::RECOVERED);
    REQUIRE(listener.entries[before].addr == kNodeA);
    REQUIRE(listener.entries[before + 1].notice == Notice::BUS_RECOVERED);
    REQUIRE(listener.entries[before + 1].addr == kBusAddr);
    REQUIRE(tracker.bus_stats().bus_faults == 2); // two episodes, not an oscillation
}

TEST_CASE("a new episode at the fallback rate reads a pre-declare poll's answer as a fallback one",
          "[timing:bit_rate_fallback]") {
    // The other direction of the same per-episode reset, and why it is to the RATE IN USE and
    // not simply to zero. Episode 1 clears at the fallback rate, and its reference pass leaves
    // every enrolled address's bit CLEAR. If those bits stood into episode 2, A's answer — to a
    // poll that went out at the fallback rate, the rate in use — would be read as a
    // reference-rate one and clear the fault AT the reference rate: A is enrolled at a rate it
    // cannot hear, fails back into SUSPECT and re-declares the fault it just cleared, which is
    // the oscillation data-model §7's "prefer the reference rate" rule exists to avoid.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    probe_until(tracker, kNodeC, omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeC, true, 10'000); // C answers at the fallback rate
    uint64_t t = 11'000;
    for (int i = 0; i < 3; ++i) { // the pass draws nothing: clear at the fallback rate
        const Probe p = tracker.next_probe(0);
        tracker.on_result(p.addr, false, t += 1'000);
    }
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback); // the rate in use from here
    REQUIRE(tracker.state(kNodeC) == HealthState::ENROLLED);
    REQUIRE(tracker.state(kNodeA) == HealthState::SUSPECT);

    // Episode 2, declared when C stops answering. A's poll went out at the fallback rate.
    fail_to_suspect(tracker, kNodeC, 20'000);
    REQUIRE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback);

    tracker.on_result(kNodeA, true, 20'001);

    REQUIRE(tracker.bus_fault()); // §7: a fallback-rate answer clears nothing on its own…
    REQUIRE(tracker.state(kNodeA) == HealthState::SUSPECT); // …and its §6 transition is deferred
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback); // no upgrade from that answer
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);          // still only episode 1's

    // What it did start is a reference pass, and that pass drawing nothing is what clears
    // episode 2 — at the fallback rate, applying A's deferred enrolment.
    uint64_t t2 = 21'000;
    for (int i = 0; i < 3; ++i) {
        const Probe p = tracker.next_probe(0);
        REQUIRE(p.bit_rate == omgp::TRUNK_bit_rate);
        tracker.on_result(p.addr, false, t2 += 1'000);
    }
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback);
    REQUIRE(tracker.state(kNodeA) == HealthState::ENROLLED);
    REQUIRE(tracker.bus_stats().bus_faults == 2);
}

TEST_CASE("an outcome outstanding across an episode boundary is read at its own probe's rate",
          "[timing:bit_rate_fallback]") {
    // The per-episode reset must not overwrite the record of a probe whose outcome has NOT yet
    // arrived: there the bit is the only correct record in the system. The two "a new episode
    // ..." cases above drive an outcome for an address the new episode has not probed, but
    // never one whose probe was still outstanding when the episode turned over (red team round
    // 6 on #530, finding 1).
    //
    // Upward direction: C is probed at the FALLBACK rate in episode 1, episode 1 clears at the
    // reference rate on another node's answer, episode 2 is declared, and only then does C's
    // probe answer. Reset to the rate in use, that answer reads as a reference-rate one and
    // clears episode 2 at 1 Mbit — enrolling C at a rate it has just demonstrated it cannot
    // hear, which is the declare/clear oscillation §7's deferral exists to prevent.
    //
    // The outstanding probe is 10 ms old at the boundary — inside the outcome window the
    // exemption is bounded by since round 7/9 (kOutcomeWindowUs in health.cpp: the slowest
    // transaction's request, in-flight hold and response window, ≈ 24.6 ms), so its record is
    // still owed an outcome and the reset must not touch it.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    // Three probes issued before any of them is answered — F3 obligation 2 is one call per
    // probe issued, not one outcome before the next call, and the alternation makes the third
    // a fallback-rate one (see the "an extra next_probe() call ..." cases above).
    const Probe p1 = tracker.next_probe(0);
    const Probe p2 = tracker.next_probe(0);
    const Probe p3 = tracker.next_probe(0);
    REQUIRE(p1.addr == kNodeA);
    REQUIRE(p1.bit_rate == omgp::TRUNK_bit_rate_fallback);
    REQUIRE(p2.addr == kNodeB);
    REQUIRE(p2.bit_rate == omgp::TRUNK_bit_rate);
    REQUIRE(p3.addr == kNodeC);
    REQUIRE(p3.bit_rate == omgp::TRUNK_bit_rate_fallback); // C's outcome is left outstanding

    tracker.on_result(kNodeB, true, 5'000); // p2 answers: episode 1 clears at the reference rate
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);

    fail_to_suspect(tracker, kNodeB, 10'000); // B stops answering: episode 2, 10 ms after p3
    REQUIRE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);

    tracker.on_result(kNodeC, true, 10'001); // p3's outcome at last — a FALLBACK-rate probe's

    REQUIRE(tracker.bus_fault());                           // §7: it clears nothing on its own…
    REQUIRE(tracker.state(kNodeC) == HealthState::SUSPECT); // …and its §6 transition is deferred
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);    // no clear, so no rate decided yet
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);    // still only episode 1's
    REQUIRE(tracker.bus_stats().bus_faults == 2);
}

TEST_CASE("an abandoned probe does not freeze its address's rate record: the outstanding-probe "
          "exemption expires with the outcome window",
          "[timing:bit_rate_fallback][timing:T_resp]") {
    // Round-7 red team on #530, finding 1 (their RT1, reproduced by the maintainer 2026-09-14):
    // a probe whose outcome never arrives — dropped at L2, abandoned by the scheduler at a
    // superframe boundary, a timeout F3 never reports — left `probe_live` set for ever, so the
    // address was exempt from the per-episode reset in EVERY later episode and a later
    // reference-rate answer from it (a pre-declare status poll's) was read at the stale
    // fallback bit: the fault did not clear at the reference rate, the node stayed SUSPECT, and
    // the pass drew nothing — the trunk pinned at the fallback rate. The bound: a probe's
    // outcome arrives within the slowest transaction's request + in-flight hold + response
    // window (kOutcomeWindowUs in health.cpp, ≈ 24.6 ms at the fallback rate; round-9 red
    // team: the round-7 bound of T_resp measured from issue was shorter than a fallback-rate
    // request's own transmission), so a record older than that owes nothing and the reset
    // applies to it.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    REQUIRE(tracker.bus_fault());
    const Probe a = tracker.next_probe(0); // A @ fallback, t = 0 — never answered, and no later
    REQUIRE(a.addr == kNodeA);             // handout supersedes it: only time can write it off
    REQUIRE(a.bit_rate == omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeB, true, 6'000); // B's answer (a pre-declare poll's, read at the rate
                                            // in use): episode 1 clears at the reference rate
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.state(kNodeA) == HealthState::SUSPECT);

    fail_to_suspect(tracker, kNodeB, 50'000); // episode 2, 50 ms after A's probe: past the window
    REQUIRE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);

    tracker.on_result(kNodeA, true, 50'001); // a REFERENCE-rate answer (a pre-declare poll's)

    REQUIRE_FALSE(tracker.bus_fault());                      // FR-026: clears at once…
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);     // …at the reference rate
    REQUIRE(tracker.state(kNodeA) == HealthState::ENROLLED); // applied, not deferred to a pass
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 2);
}

TEST_CASE("an outstanding fallback-rate probe is deferred at any age inside the outcome window",
          "[timing:bit_rate_fallback][timing:T_resp]") {
    // Round-9 red team on #530, finding 1: the round-7 bound (TRUNK_T_resp_us from the ISSUE
    // instant) was shorter than a fallback-rate request's own transmission — at 86 us per
    // byte a 3-byte request ends 258 us after issue, before its response window even opens —
    // so every outstanding probe older than 200 us had its record overwritten and its
    // fallback answer misread as a reference-rate one: round 6's oscillation, reinstated. The
    // bound is the slowest transaction's whole outcome window (health.cpp, kOutcomeWindowUs):
    // swept here from 100 us to 20 ms of issue-to-declare gap, every row defers.
    for (const uint64_t gap : {100u, 200u, 201u, 600u, 2'000u, 10'000u, 20'000u}) {
        CAPTURE(gap);
        FakeClock clock;
        RecordingListener listener;
        HealthTracker tracker(clock, listener);
        ThreeNodeRig::build(tracker);
        const uint64_t at = 30'000 - gap;
        const Probe p1 = tracker.next_probe(at); // A @ fallback
        const Probe p2 = tracker.next_probe(at); // B @ reference
        const Probe p3 = tracker.next_probe(at); // C @ fallback, left outstanding
        REQUIRE(p1.bit_rate == omgp::TRUNK_bit_rate_fallback);
        REQUIRE(p2.bit_rate == omgp::TRUNK_bit_rate);
        REQUIRE(p3.addr == kNodeC);
        REQUIRE(p3.bit_rate == omgp::TRUNK_bit_rate_fallback);
        tracker.on_result(kNodeB, true, at + 10); // episode 1 clears at the reference rate
        REQUIRE_FALSE(tracker.bus_fault());
        fail_to_suspect(tracker, kNodeB, 30'000); // episode 2
        REQUIRE(tracker.bus_fault());
        tracker.on_result(kNodeC, true, 30'001); // p3's fallback answer, inside its window
        REQUIRE(tracker.bus_fault());            // deferred, not a clear at 1 Mbit
        REQUIRE(tracker.state(kNodeC) ==
                HealthState::SUSPECT); // not enrolled at a rate it cannot hear
    }
}

TEST_CASE("an unrelated discovery probe does not keep an abandoned probe's record alive",
          "[timing:bit_rate_fallback][timing:T_resp]") {
    // Round-9 red team on #530, finding 2: one timestamp for the whole live set was rewritten
    // by EVERY note_probe(), so a discovery probe to any other address issued just before the
    // declare re-armed the exemption for a probe abandoned arbitrarily long ago — the
    // enrolment rotation probes every UNENROLLED address continuously, so that is the
    // ordinary case, not a corner. The issue instant is per address now.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);
    ThreeNodeRig::build(tracker);
    REQUIRE(probe_until(tracker, kNodeA, omgp::TRUNK_bit_rate_fallback).addr ==
            kNodeA); // t = 0, abandoned
    REQUIRE(probe_until(tracker, kNodeB, omgp::TRUNK_bit_rate).addr == kNodeB);
    tracker.on_result(kNodeB, true, 6'000); // episode 1 clears at the reference rate
    REQUIRE_FALSE(tracker.bus_fault());
    const Probe d = tracker.next_probe(49'900); // enrolment rotation: an UNENROLLED address
    REQUIRE(d.addr != kNodeA);
    REQUIRE(d.addr != kNodeB);
    fail_to_suspect(tracker, kNodeB, 50'000); // episode 2, 50 ms after A's probe
    REQUIRE(tracker.bus_fault());
    tracker.on_result(kNodeA, true, 50'001); // a REFERENCE-rate answer (a pre-declare poll's)
    REQUIRE_FALSE(tracker.bus_fault());      // FR-026: clears at once
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);
    REQUIRE(tracker.state(kNodeA) == HealthState::ENROLLED);
}

TEST_CASE("a duplicate fallback answer after the pass has probed the answerer does not clear "
          "the fault at the reference rate",
          "[timing:bit_rate_fallback]") {
    // Round-8 red team on #530, finding 1 [HIGH]: the pass probe to the answerer goes out at
    // the reference rate and note_probe() overwrites the answerer's own bit, so a second
    // delivery of the SAME fallback answer (golden rule 2: retries at L2 are always safe, so a
    // duplicate outcome is a legitimate input) read as a reference-rate one and cleared the
    // fault at 1 Mbit — a false BUS_RECOVERED, the node enrolled at a rate it cannot hear, the
    // reference pass bypassed, the fault re-declared on the next poll round. A node is strapped
    // to ONE rate (trunk §2): the answerer cannot hear the pass probe, so an ok outcome for it
    // while the pass runs is that answer again, never a reference-rate one, and it restarts the
    // pass — the benign reading "a duplicate fallback answer before the first pass probe
    // restarts an identical pass" already pins, now on the other side of the first pass probe.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);
    ThreeNodeRig::build(tracker);
    REQUIRE(probe_until(tracker, kNodeC, omgp::TRUNK_bit_rate_fallback).addr == kNodeC);
    tracker.on_result(kNodeC, true, 10'000); // the fallback answer: the pass starts
    uint64_t t = 11'000;
    for (uint8_t expect : {kNodeA, kNodeB}) { // the pass draws nothing from A and B
        const Probe p = tracker.next_probe(t);
        REQUIRE(p.addr == expect);
        REQUIRE(p.bit_rate == omgp::TRUNK_bit_rate);
        tracker.on_result(p.addr, false, t += 1'000);
    }
    const Probe pc = tracker.next_probe(t); // the pass probes the answerer at the reference rate
    REQUIRE(pc.addr == kNodeC);
    REQUIRE(pc.bit_rate == omgp::TRUNK_bit_rate);

    tracker.on_result(kNodeC, true, t + 10); // the fallback answer, delivered again

    REQUIRE(tracker.bus_fault());                           // NOT a clear at 1 Mbit
    REQUIRE(tracker.state(kNodeC) == HealthState::SUSPECT); // still deferred
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 0);
    // …and it is a NO-OP for the pass (round-10 red team: read as a fresh fallback answer it
    // restarted the pass, and a babbling or retried answerer starved the trunk for ever): the
    // pass probe to C is still the one outstanding, its real outcome ends the pass, and the
    // fault clears at the fallback rate after exactly |enrolled| probes.
    const Probe again = tracker.next_probe(t + 20);
    REQUIRE(again.addr == kNodeC); // re-yielded while its outcome is outstanding
    REQUIRE(again.bit_rate == omgp::TRUNK_bit_rate);
    tracker.on_result(kNodeC, false, t + 30); // the pass probe's own outcome: nothing heard
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback);
    REQUIRE(tracker.state(kNodeC) == HealthState::ENROLLED);
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);
}

TEST_CASE("a fallback answerer that keeps answering cannot stop the pass from ending",
          "[timing:bit_rate_fallback]") {
    // Round-10 red team on #530, finding 1 [HIGH]: reading every ok outcome for the recorded
    // answerer as a fresh fallback answer restarted the whole pass each time, so an L2 retry
    // (golden rule 2) or a babbling module at that address kept the pass from ever ending —
    // no clear at either rate, no status polls while faulted: one module starved the trunk.
    // The same answer recorded again is a no-op; the pass ends after |enrolled| outcomes.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);
    ThreeNodeRig::build(tracker);
    REQUIRE(probe_until(tracker, kNodeC, omgp::TRUNK_bit_rate_fallback).addr == kNodeC);
    tracker.on_result(kNodeC, true, 10'000); // the fallback answer: the pass starts
    uint64_t t = 11'000;
    int pass_outcomes = 0;
    for (int i = 0; i < 12 && tracker.bus_fault(); ++i) { // far more than a 3-probe pass needs
        const Probe p = tracker.next_probe(t);
        REQUIRE(p.bit_rate == omgp::TRUNK_bit_rate);
        tracker.on_result(kNodeC, true, t + 1); // C answers AGAIN, every superframe (babble/retry)
        tracker.on_result(p.addr, false, t += 1'000); // the pass probe's own outcome
        ++pass_outcomes;
    }
    REQUIRE(pass_outcomes == 3);        // exactly |enrolled| probes
    REQUIRE_FALSE(tracker.bus_fault()); // the pass drew nothing…
    REQUIRE(tracker.bit_rate() ==
            omgp::TRUNK_bit_rate_fallback); // …and cleared at the fallback rate
    REQUIRE(tracker.state(kNodeC) == HealthState::ENROLLED);
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);
}

TEST_CASE("a fallback answerer whose answers keep arriving does not stall the pass: the first "
          "outcome for its outstanding pass probe advances the pass whatever its value",
          "[timing:bit_rate_fallback]") {
    // Round-11 red team on #530, finding 1 [HIGH]: round 10 made every ok outcome for the
    // recorded answerer a complete no-op, returning BEFORE the pass-advance block — and
    // pass_probe() re-yields pass_addr while its outcome is outstanding, so once the pass
    // probed the answerer every later ok advanced nothing and the same address was handed
    // out for ever: no clear at either rate, no polls while faulted. The round-10 case only
    // escaped because it fed a second, `false` outcome for the same probe; a babbling or
    // retrying answerer inside the pass probe's window reports ok and never a timeout.
    // Now: an ok for an address that answered at the fallback rate this episode is still
    // never a reference-rate clear, but if that address's pass probe is outstanding it IS
    // that probe's outcome and advances the pass.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);
    ThreeNodeRig::build(tracker);
    REQUIRE(probe_until(tracker, kNodeC, omgp::TRUNK_bit_rate_fallback).addr == kNodeC);
    tracker.on_result(kNodeC, true, 10'000); // the fallback answer: the pass starts
    uint64_t t = 11'000;
    int outcomes = 0;
    for (int i = 0; i < 12 && tracker.bus_fault(); ++i) {
        const Probe p = tracker.next_probe(t);
        REQUIRE(p.bit_rate == omgp::TRUNK_bit_rate);
        // ONE outcome per pass probe: ok for the answerer (its answer keeps arriving inside the
        // window), nothing heard from anyone else — never a second, `false` outcome.
        tracker.on_result(p.addr, p.addr == kNodeC, t += 1'000);
        ++outcomes;
    }
    REQUIRE(outcomes == 3);             // |enrolled| probes, then it ends
    REQUIRE_FALSE(tracker.bus_fault()); // the pass drew nothing…
    REQUIRE(tracker.bit_rate() ==
            omgp::TRUNK_bit_rate_fallback); // …and cleared at the fallback rate
    REQUIRE(tracker.state(kNodeC) == HealthState::ENROLLED);
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);
}

TEST_CASE("every node that answered at the fallback rate this episode is protected, not only "
          "the latest: a duplicate from an earlier answerer never clears at the reference rate",
          "[timing:bit_rate_fallback]") {
    // Round-11 red team on #530, finding 2 [HIGH]: `fallback_answerer` is one address, so
    // when a second node answered at the fallback rate the first lost round 8's protection —
    // the pass probe still overwrote its bit to the reference rate, and its next duplicate
    // cleared the fault at 1 Mbit. Two nodes answering the fallback probes is the ORDINARY
    // shape of a rate-mismatch fault. Per episode the tracker now remembers every address that
    // answered at the fallback rate; an ok from any of them is that answer again.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);
    ThreeNodeRig::build(tracker);
    // Three probes handed out before any answers (obligation-2 slack, sanctioned by the
    // "an extra next_probe() call" cases): A @ fallback, B @ reference, C @ fallback.
    const Probe pa = tracker.next_probe(4'000);
    const Probe pb = tracker.next_probe(4'000);
    const Probe pc = tracker.next_probe(4'000);
    REQUIRE(pa.addr == kNodeA);
    REQUIRE(pa.bit_rate == omgp::TRUNK_bit_rate_fallback);
    REQUIRE(pb.addr == kNodeB);
    REQUIRE(pc.addr == kNodeC);
    REQUIRE(pc.bit_rate == omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeA, true, 5'000); // A answers at the fallback rate: the pass starts
    tracker.on_result(kNodeC, true, 5'100); // C answers at the fallback rate too
    REQUIRE(tracker.bus_fault());
    uint64_t t = 6'000;
    for (uint8_t expect : {kNodeA, kNodeB}) { // the pass: A, then B, nothing heard
        const Probe p = tracker.next_probe(t);
        REQUIRE(p.addr == expect);
        REQUIRE(p.bit_rate == omgp::TRUNK_bit_rate);
        tracker.on_result(p.addr, false, t += 1'000);
    }
    const Probe pass_c = tracker.next_probe(t); // the pass probes C at the reference rate
    REQUIRE(pass_c.addr == kNodeC);
    REQUIRE(pass_c.bit_rate == omgp::TRUNK_bit_rate);

    tracker.on_result(kNodeC, true, t + 10); // C's fallback answer, delivered again

    REQUIRE(tracker.bus_fault());                           // NOT a clear at 1 Mbit
    REQUIRE(tracker.state(kNodeC) == HealthState::SUSPECT); // still deferred
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 0);
    // That ok was the outstanding pass probe's outcome: the pass drew nothing and ends here,
    // clearing at the fallback rate and enrolling BOTH nodes that answered there.
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback);
    REQUIRE(tracker.state(kNodeA) == HealthState::ENROLLED);
    REQUIRE(tracker.state(kNodeC) == HealthState::ENROLLED);
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);
}

TEST_CASE("a second outstanding probe does not cancel the first's episode-boundary exemption",
          "[timing:bit_rate_fallback][timing:T_resp]") {
    // Round-10 review on #530, finding 1 [HIGH]: round 9's "newest handout supersedes" rule
    // dropped A's live bit when B's probe went out, so at the episode-2 declare A's fallback
    // bit was reset to the reference rate in use and A's fallback answer, 150 us after issue,
    // cleared the fault at 1 Mbit — a false BUS_RECOVERED, A enrolled at a rate it cannot hear.
    // Two outstanding probes violate neither F3 obligation (obligation 2 is one call per probe
    // issued, not one outcome before the next call), so the rule is withdrawn: every
    // outstanding probe keeps its record while its outcome can still arrive.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);
    ThreeNodeRig::build(tracker);
    const Probe a = tracker.next_probe(4'000); // A @ fallback
    const Probe b = tracker.next_probe(4'001); // B @ reference — A's probe still outstanding
    REQUIRE(a.addr == kNodeA);
    REQUIRE(a.bit_rate == omgp::TRUNK_bit_rate_fallback);
    REQUIRE(b.addr == kNodeB);
    REQUIRE(b.bit_rate == omgp::TRUNK_bit_rate);
    tracker.on_result(kNodeB, true, 4'002); // episode 1 clears at the reference rate
    REQUIRE_FALSE(tracker.bus_fault());
    fail_to_suspect(tracker, kNodeB, 4'100); // episode 2, rate in use reference
    REQUIRE(tracker.bus_fault());
    tracker.on_result(kNodeA, true, 4'150); // A's FALLBACK probe answered, 150 us after issue
    REQUIRE(tracker.bus_fault());           // deferred to a reference pass…
    REQUIRE(tracker.state(kNodeA) == HealthState::SUSPECT); // …not enrolled at 1 Mbit
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1); // only episode 1's
}

TEST_CASE("an outstanding reference-rate probe answered in the next episode still clears at once",
          "[timing:bit_rate_fallback]") {
    // Round 9 rewrote this pin to assert the opposite under a "newest handout supersedes"
    // rule; round 10 showed that rule contradicts the obligations as written (two outstanding
    // probes violate neither — obligation 2 is one call per probe issued, not one outcome
    // before the next call) and re-opened round 6's oscillation for every outstanding probe
    // that was not the newest. The rule is withdrawn and this pin restored: an outstanding
    // probe keeps its record across the boundary for as long as its outcome can still arrive
    // (kOutcomeWindowUs, per address), however many handouts follow it.
    // The other direction of the same reset, on an episode whose rate in use is the FALLBACK
    // one: D is probed at the reference rate in episode 1 and answers only in episode 2. Reset
    // to the rate in use, that answer reads as a fallback-rate one, so instead of clearing at
    // once (FR-026) the trunk runs a reference pass and stays pinned at 115 200 with no
    // automatic return (ruling 2026-09-13).
    //
    // D is never enrolled, which is what keeps its probe outstanding across the boundary: the
    // reference pass that ends episode 1 covers only addresses that have answered, so nothing
    // in between delivers an outcome for D. probe_until() is deliberately NOT used to reach it
    // — it walks the whole rotation, which would probe D a second time before its first outcome
    // arrives, the one case this classification is documented as unable to tell apart.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    tracker.next_probe(0); // A @ fallback
    tracker.next_probe(0); // B @ reference
    const Probe c = tracker.next_probe(0);
    const Probe d = tracker.next_probe(0);
    REQUIRE(c.addr == kNodeC);
    REQUIRE(c.bit_rate == omgp::TRUNK_bit_rate_fallback);
    REQUIRE(d.addr == kNodeD);
    REQUIRE(d.bit_rate == omgp::TRUNK_bit_rate); // outcome deliberately left outstanding

    tracker.on_result(kNodeC, true, 10'000); // a fallback-rate answer: the pass starts
    uint64_t t = 11'000;
    for (int i = 0; i < 3; ++i) { // the pass covers A, B and C only, and draws nothing
        const Probe p = tracker.next_probe(t);
        REQUIRE(p.addr != kNodeD);
        tracker.on_result(p.addr, false, t += 1'000);
    }
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback); // the rate in use from here
    REQUIRE(tracker.state(kNodeD) == HealthState::UNENROLLED);

    fail_to_suspect(tracker, kNodeC, 20'000); // C stops answering: episode 2
    REQUIRE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback);

    tracker.on_result(kNodeD, true, 20'001); // D's outcome at last — a REFERENCE-rate probe's

    REQUIRE_FALSE(tracker.bus_fault());                      // FR-026: it clears at once…
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);     // …at the reference rate
    REQUIRE(tracker.state(kNodeD) == HealthState::ENROLLED); // applied, not deferred to a pass
    REQUIRE(tracker.bus_stats().bus_faults == 2);
}

// --------------------------------------------------- clear at the reference rate (FR-026) --

TEST_CASE("a valid answer at the reference rate clears the fault at the reference rate at once",
          "[timing:bit_rate_fallback]") {
    // spec.md US5 acceptance scenario 3 (amended 2026-09-13, F4): the host PREFERS the
    // reference rate — a reference-rate answer needs no pass.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    const size_t before = listener.entries.size();

    const Probe p = probe_until(tracker, kNodeB, omgp::TRUNK_bit_rate);
    REQUIRE(p.bit_rate == omgp::TRUNK_bit_rate);
    tracker.on_result(kNodeB, true, 10'000);

    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);
    REQUIRE(tracker.state(kNodeB) == HealthState::ENROLLED);

    // The answering node's own §6 transition and the bus notice, in that order, and nothing
    // else: the fault was not re-declared, and the other two nodes did not transition.
    REQUIRE(listener.entries.size() == before + 2);
    REQUIRE(listener.entries[before].notice == Notice::RECOVERED);
    REQUIRE(listener.entries[before].addr == kNodeB);
    REQUIRE(listener.entries[before + 1].notice == Notice::BUS_RECOVERED);
    REQUIRE(listener.entries[before + 1].addr == kBusAddr);
    REQUIRE(tracker.state(kNodeA) == HealthState::SUSPECT);
    REQUIRE(tracker.state(kNodeC) == HealthState::SUSPECT);
}

// ------------------------------------ fallback answer, reference pass, clear (FR-026) ------

TEST_CASE("a fallback-rate answer neither clears the fault nor moves that node's state",
          "[timing:bit_rate_fallback]") {
    // data-model.md §7: the answer is RECORDED (fallback_answerer) and its §6 transition is
    // DEFERRED — enrolling it at once would have it status-polled at a rate it cannot hear.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    const size_t before = listener.entries.size();

    probe_until(tracker, kNodeB, omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeB, true, 10'000);

    REQUIRE(tracker.bus_fault());
    REQUIRE(tracker.state(kNodeB) == HealthState::SUSPECT); // not ENROLLED, not RECOVERED
    REQUIRE(listener.entries.size() == before);             // no notice at all
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);
    REQUIRE(tracker.bus_stats().bus_faults == 1);
}

TEST_CASE("after a fallback-rate answer the pass probes every enrolled address once, in address "
          "order, at the reference rate",
          "[timing:bit_rate_fallback]") {
    // FR-026: "exactly |enrolled| probes", reference rate, WITHOUT alternating.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    probe_until(tracker, kNodeB, omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeB, true, 10'000);
    REQUIRE(tracker.bus_fault());

    // The recorded answerer is itself enrolled, so the pass is three probes: A, B, C.
    const uint8_t expected[3] = {kNodeA, kNodeB, kNodeC};
    uint64_t t = 11'000;
    for (int i = 0; i < 3; ++i) {
        const Probe p = tracker.next_probe(0);
        REQUIRE(p.addr == expected[i]);
        REQUIRE(p.bit_rate == omgp::TRUNK_bit_rate); // no alternation during the pass
        if (i < 2)
            REQUIRE(tracker.bus_fault()); // not cleared before the pass has finished
        tracker.on_result(p.addr, false, t += 1'000);
    }
}

TEST_CASE("a duplicate fallback answer before the first pass probe restarts an identical pass",
          "[timing:bit_rate_fallback]") {
    // Pins the window the fallback branch's comment in link/health.cpp now names (red team
    // round 2 on #530, finding 2): between the answer that starts a pass and the first pass
    // probe, the remembered rate for the answerer is still the fallback one, so a duplicate or
    // late outcome for it re-enters that branch and recomputes the pass. It is benign, and the
    // assertions below are what says so: |enrolled| cannot change in that window and the pass
    // cursor is already at ADDR_backplane_min, so the recomputation reproduces the state it
    // overwrites — the pass is still exactly |enrolled| probes in address order, and the clear
    // is still one BUS_RECOVERED at the fallback rate.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    probe_until(tracker, kNodeC, omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeC, true, 10'000); // starts the pass
    tracker.on_result(kNodeC, true, 10'001); // the duplicate, before the first pass probe

    REQUIRE(tracker.bus_fault());
    REQUIRE(tracker.state(kNodeC) == HealthState::SUSPECT); // still deferred, not enrolled twice

    const uint8_t expected[3] = {kNodeA, kNodeB, kNodeC};
    uint64_t t = 11'000;
    for (int i = 0; i < 3; ++i) {
        const Probe p = tracker.next_probe(0);
        REQUIRE(p.addr == expected[i]);
        REQUIRE(p.bit_rate == omgp::TRUNK_bit_rate);
        if (i < 2)
            REQUIRE(tracker.bus_fault()); // the restart did not shorten the pass
        tracker.on_result(p.addr, false, t += 1'000);
    }

    REQUIRE_FALSE(tracker.bus_fault()); // …nor lengthen it
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback);
    REQUIRE(tracker.state(kNodeC) == HealthState::ENROLLED);
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);
    REQUIRE(listener.count(Notice::RECOVERED) == 1); // the deferred transition, applied once
}

TEST_CASE("a pass whose only enrolled address is the last one in the rotation still reaches it",
          "[timing:bit_rate_fallback]") {
    // FR-026 "every enrolled address once, in address order": the pass cursor starts AT
    // ADDR_backplane_min and scans the whole backplane range. Every other pass test above
    // enrols ADDR_backplane_min itself, which pass_probe() finds on its FIRST iteration — so
    // none of them can tell a full scan from a one-address one, nor a cursor that starts one
    // address too low. This case puts the sole enrolled address LAST in the rotation, where
    // only a scan that runs the full kBackplaneCount iterations from ADDR_backplane_min
    // reaches it (deep-verify survivors on #530).
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    constexpr uint8_t kNodeTop = omgp::ADDR_backplane_max;
    enrol(tracker, kNodeTop, 0);
    fail_to_suspect(tracker, kNodeTop, 1'000);
    REQUIRE(tracker.bus_fault()); // one enrolled node is all nodes (ruling Q2)

    // A fallback-rate answer from an address that never enrolled starts the pass; kNodeTop is
    // the only enrolled address, so FR-026 makes the pass exactly one probe.
    probe_until(tracker, kNodeA, omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeA, true, 10'000);
    REQUIRE(tracker.bus_fault());
    REQUIRE(tracker.state(kNodeA) == HealthState::UNENROLLED); // a fallback answer does not enrol

    const Probe p = tracker.next_probe(0);
    REQUIRE(p.addr == kNodeTop); // not the ADDR_host sentinel: the scan neither started one
                                 // address low nor stopped short of the last address
    REQUIRE(p.bit_rate == omgp::TRUNK_bit_rate);

    // ...and that single probe drawing nothing ends the pass and clears at the fallback rate.
    tracker.on_result(kNodeTop, false, 11'000);
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback);
    REQUIRE(tracker.state(kNodeA) == HealthState::ENROLLED); // the deferred §6 transition
    REQUIRE(tracker.state(kNodeTop) == HealthState::SUSPECT);
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);
}

TEST_CASE("a pass over the first and last addresses of the rotation probes them in that order",
          "[timing:bit_rate_fallback]") {
    // The companion of the case above, and the other half of what pins the pass cursor's
    // start to exactly ADDR_backplane_min. That case enrols only the LAST address, so it
    // fails for a cursor that starts too LOW (the scan then spends one of its
    // kBackplaneCount iterations below the rotation and stops one address short). It cannot
    // fail for a cursor that starts too HIGH: a scan from anywhere inside the range still
    // reaches the sole enrolled address by wrapping. This case enrols the FIRST and the LAST
    // address and pins the order, so a cursor starting anywhere above ADDR_backplane_min
    // meets ADDR_backplane_max first and probes it out of order (deep-verify survivor
    // link/health.cpp:92 cxx_assign_const on #530, whose rewritten constant is NOT the
    // type's zero value — see the comment at that line).
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    constexpr uint8_t kNodeTop = omgp::ADDR_backplane_max;
    enrol(tracker, kNodeA, 0);
    enrol(tracker, kNodeTop, 0);
    fail_to_suspect(tracker, kNodeA, 1'000);
    fail_to_suspect(tracker, kNodeTop, 2'000);
    REQUIRE(tracker.bus_fault());

    // kNodeB never enrolled, so the pass is exactly the two enrolled addresses (FR-026).
    probe_until(tracker, kNodeB, omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeB, true, 10'000);
    REQUIRE(tracker.bus_fault());

    const Probe first = tracker.next_probe(0);
    REQUIRE(first.addr == kNodeA); // address order: the FIRST address of the rotation first
    REQUIRE(first.bit_rate == omgp::TRUNK_bit_rate);
    tracker.on_result(kNodeA, false, 11'000);
    REQUIRE(tracker.bus_fault()); // one probe of two: the pass has not ended

    const Probe second = tracker.next_probe(0);
    REQUIRE(second.addr == kNodeTop); // ...and the last address of the rotation second
    REQUIRE(second.bit_rate == omgp::TRUNK_bit_rate);

    // Both probes drew nothing, so the pass ends and the fallback rate is the one that works.
    tracker.on_result(kNodeTop, false, 12'000);
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback);
    REQUIRE(tracker.state(kNodeB) == HealthState::ENROLLED); // the deferred §6 transition
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);
}

TEST_CASE("a pass probe answered by a node last probed at the fallback rate still clears at the "
          "reference rate",
          "[timing:bit_rate_fallback]") {
    // The rate an outcome is read at is the rate of the last probe TO THAT ADDRESS, so a pass
    // probe must record its own reference rate against the address it goes to: A is probed at
    // the fallback rate first and does not answer, and only later answers a PASS probe. Without
    // the pass probe's record, A's reference-rate answer would be read at the rate of the
    // fallback probe it never answered — starting a second pass instead of clearing at once
    // (FR-026). The mid-pass clear cases above cannot see this: their answerer's last
    // alternating probe happened to be a reference-rate one.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    const Probe fb = probe_until(tracker, kNodeA, omgp::TRUNK_bit_rate_fallback);
    REQUIRE(fb.addr == kNodeA);
    tracker.on_result(kNodeA, false, 5'000); // A is silent at the fallback rate

    probe_until(tracker, kNodeC, omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeC, true, 10'000); // C answers there: the pass starts

    const Probe pass = tracker.next_probe(0);
    REQUIRE(pass.addr == kNodeA); // the pass runs in address order
    REQUIRE(pass.bit_rate == omgp::TRUNK_bit_rate);
    tracker.on_result(kNodeA, true, 11'000); // …and A answers it, at the REFERENCE rate

    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);
    REQUIRE(tracker.state(kNodeA) == HealthState::ENROLLED);
    REQUIRE(tracker.state(kNodeC) == HealthState::SUSPECT); // the recorded answerer, untouched
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);
}

TEST_CASE("a pass probe is re-yielded while its outcome is outstanding", "[link]") {
    // data-model.md §6 (round-13 red team on #472): a scheduler that calls next_probe() with
    // the pass probe still in flight (F3 obligation 2 violated) gets the SAME probe back and
    // the pass still completes. Fail-safe under a violated assumption, not a property of it.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    probe_until(tracker, kNodeB, omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeB, true, 10'000);

    const Probe first = tracker.next_probe(0);
    REQUIRE(first.addr == kNodeA);
    for (int i = 0; i < 3; ++i) { // mis-called three times: the pass does not walk past A
        const Probe again = tracker.next_probe(0);
        REQUIRE(again.addr == kNodeA);
        REQUIRE(again.bit_rate == omgp::TRUNK_bit_rate);
    }
    tracker.on_result(kNodeA, false, 11'000);
    REQUIRE(tracker.next_probe(0).addr == kNodeB); // only now does the pass advance
}

TEST_CASE("a tick between the last pass probe and its outcome does not clear the fault", "[link]") {
    // data-model.md §7 (round-8 red team on #472): ref_pass_left counts OUTCOMES, never
    // probes issued — a clear fired by a tick would recover a trunk nothing had answered.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    probe_until(tracker, kNodeB, omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeB, true, 10'000);

    uint64_t t = 11'000;
    for (int i = 0; i < 3; ++i) {
        const Probe p = tracker.next_probe(0);
        if (i == 2) {
            tracker.tick(t); // the last probe is on the wire, its outcome is not in yet
            REQUIRE(tracker.bus_fault());
            REQUIRE(listener.count(Notice::BUS_RECOVERED) == 0);
        }
        tracker.on_result(p.addr, false, t += 1'000);
    }
    REQUIRE_FALSE(tracker.bus_fault()); // the third OUTCOME is what ends the pass
}

TEST_CASE("a reference-rate answer during the pass clears at the reference rate and leaves the "
          "recorded answerer alone",
          "[timing:bit_rate_fallback]") {
    // data-model.md §7: the pass answer wins; the node that answered at the fallback rate
    // keeps the state it had — it cannot hear the reference rate and §6 will find it in its
    // own time.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    probe_until(tracker, kNodeC, omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeC, true, 10'000);
    REQUIRE(tracker.bus_fault());

    const Probe first = tracker.next_probe(0); // A
    REQUIRE(first.addr == kNodeA);
    tracker.on_result(first.addr, false, 11'000);
    const Probe second = tracker.next_probe(0); // B — and B answers at the reference rate
    REQUIRE(second.addr == kNodeB);
    tracker.on_result(second.addr, true, 12'000);

    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);
    REQUIRE(tracker.state(kNodeB) == HealthState::ENROLLED);
    REQUIRE(tracker.state(kNodeC) == HealthState::SUSPECT); // the recorded answerer, untouched

    // The pass state went with the clear: the next probe is a plain enrolment-rotation probe
    // at the rate in use, not a leftover pass probe.
    const Probe after = tracker.next_probe(0);
    REQUIRE(after.bit_rate == omgp::TRUNK_bit_rate);
    REQUIRE(after.addr != kNodeB); // B is ENROLLED again: not a rotation candidate
}

TEST_CASE("a pass that draws nothing clears the fault at the fallback rate and enrols the "
          "recorded answerer",
          "[timing:bit_rate_fallback]") {
    // FR-026 / data-model.md §7: no response in the whole pass -> clear at the fallback rate,
    // the fallback answerer becomes ENROLLED BEFORE the declare rule is next evaluated (so the
    // recovery is not re-declared as a fault in the same call), exactly one BUS_RECOVERED.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    probe_until(tracker, kNodeC, omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeC, true, 10'000);
    const uint32_t changes_before = tracker.bus_stats().rate_changes;

    uint64_t t = 11'000;
    for (int i = 0; i < 3; ++i) {
        const Probe p = tracker.next_probe(0);
        // Exactly two rate changes over the whole pass (§8, counted where each is decided):
        // the first pass probe moves the wire from the fallback rate that just answered to
        // the reference rate, and the pass does not alternate afterwards.
        REQUIRE(tracker.bus_stats().rate_changes == changes_before + 1);
        tracker.on_result(p.addr, false, t += 1'000);
    }

    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback);
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);
    REQUIRE(tracker.state(kNodeC) == HealthState::ENROLLED); // the deferred §6 transition
    REQUIRE(listener.count(Notice::BUS_FAULT) == 1);         // NOT re-declared on the same result
    REQUIRE(tracker.bus_stats().bus_faults == 1);

    // …and the clear pinning the fallback rate is the second: the rate in use moved back.
    REQUIRE(tracker.bus_stats().rate_changes == changes_before + 2);

    // The recovery notice order: the deferred enrolment first, then the bus-level notice.
    const size_t n = listener.entries.size();
    REQUIRE(listener.entries[n - 2].notice == Notice::RECOVERED);
    REQUIRE(listener.entries[n - 2].addr == kNodeC);
    REQUIRE(listener.entries[n - 1].notice == Notice::BUS_RECOVERED);
    REQUIRE(listener.entries[n - 1].addr == kBusAddr);

    // No automatic return (ruling 2026-09-13): the rotation keeps probing at the fallback
    // rate, with no reference-rate re-probe cadence of any kind.
    for (int i = 0; i < 4; ++i)
        REQUIRE(tracker.next_probe(0).bit_rate == omgp::TRUNK_bit_rate_fallback);
    REQUIRE(tracker.bus_stats().rate_changes == changes_before + 2);
}

TEST_CASE("after a fallback-rate clear a valid answer at the rate now in use enrols its node",
          "[timing:bit_rate_fallback]") {
    // data-model.md §7: "a valid answer at the fallback rate" is a FAULT-TIME classification.
    // Once the fault has cleared at the fallback rate, that rate IS the rate in use (no
    // automatic return, ruling 2026-09-13) and every probe goes out at it — so each answering
    // address keeps its BusState::probe_fallback bit set, and only on_result's `bus_.fault`
    // conjunct stops the next ordinary answer being read as a fallback answer. Without it the
    // answer is deferred to a reference pass that a cleared bus never runs: the node is never
    // enrolled, emits no ENROLLED notice, and F3 never polls it (review round 3 on #530,
    // finding 3(b) — the conjunct was load-bearing and pinned by nothing).
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    probe_until(tracker, kNodeC, omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeC, true, 10'000);
    uint64_t t = 11'000;
    for (int i = 0; i < 3; ++i) {
        const Probe p = tracker.next_probe(0);
        tracker.on_result(p.addr, false, t += 1'000);
    }
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback);
    const size_t notices_after_clear = listener.entries.size();

    // A fresh address from the healthy-rig rotation: A and B are SUSPECT (never candidates
    // once the fault is cleared) and C is ENROLLED, so this is an UNENROLLED one.
    const Probe fresh = tracker.next_probe(0);
    REQUIRE(fresh.bit_rate == omgp::TRUNK_bit_rate_fallback); // the rate now in use
    REQUIRE(tracker.state(fresh.addr) == HealthState::UNENROLLED);

    tracker.on_result(fresh.addr, true, t += 1'000);

    REQUIRE(tracker.state(fresh.addr) == HealthState::ENROLLED); // applied, NOT deferred
    REQUIRE(listener.entries.size() == notices_after_clear + 1);
    REQUIRE(listener.entries.back().notice == Notice::ENROLLED);
    REQUIRE(listener.entries.back().addr == fresh.addr);
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback); // unchanged by an enrolment
    REQUIRE(tracker.bus_stats().bus_faults == 1);
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);
}

TEST_CASE("the nodes that did not answer keep their own SUSPECT clocks across the episode",
          "[timing:offline_after_suspect]") {
    // FR-026 "their clocks were not paused by the fault" / data-model.md §6: each record's
    // suspect_since is its own, so A reaches OFFLINE at ITS 1 s mark after the recovery.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    probe_until(tracker, kNodeC, omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeC, true, 10'000);
    uint64_t t = 11'000;
    for (int i = 0; i < 3; ++i) {
        const Probe p = tracker.next_probe(0);
        tracker.on_result(p.addr, false, t += 1'000);
    }
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.state(kNodeA) == HealthState::SUSPECT);
    REQUIRE(tracker.state(kNodeB) == HealthState::SUSPECT);

    // A went SUSPECT at kSuspectA and B one millisecond later: A's mark falls first, and a
    // tick one microsecond short of it moves neither.
    tracker.tick(ThreeNodeRig::kSuspectA + kThresholdUs - 1);
    REQUIRE(tracker.state(kNodeA) == HealthState::SUSPECT);

    tracker.tick(ThreeNodeRig::kSuspectA + kThresholdUs);
    REQUIRE(tracker.state(kNodeA) == HealthState::OFFLINE);
    REQUIRE(tracker.state(kNodeB) == HealthState::SUSPECT); // its own mark is 1 ms later
    REQUIRE(listener.entries.back().notice == Notice::OFFLINE);
    REQUIRE(listener.entries.back().addr == kNodeA);

    // C is ENROLLED again, so the rig that just recovered is not re-declared faulty by A's
    // OFFLINE transition.
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(listener.count(Notice::BUS_FAULT) == 1);

    tracker.tick(ThreeNodeRig::kSuspectB + kThresholdUs);
    REQUIRE(tracker.state(kNodeB) == HealthState::OFFLINE);
    REQUIRE_FALSE(tracker.bus_fault());
}

// ------------------------------------------------------- a second episode (FR-024) ---------

TEST_CASE("a second bus fault after a recovery is declared again, exactly once", "[link]") {
    // FR-024 is "declared once per episode", not "declared once ever" — the flag is not a
    // latch.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker);
    probe_until(tracker, kNodeC, omgp::TRUNK_bit_rate_fallback);
    tracker.on_result(kNodeC, true, 10'000);
    uint64_t t = 11'000;
    for (int i = 0; i < 3; ++i) {
        const Probe p = tracker.next_probe(0);
        tracker.on_result(p.addr, false, t += 1'000);
    }
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.state(kNodeC) == HealthState::ENROLLED);
    REQUIRE(tracker.bus_stats().bus_faults == 1);

    // C is now the only node answering; when it stops, every enrolled node is SUSPECT or
    // worse again.
    fail_to_suspect(tracker, kNodeC, 20'000);

    REQUIRE(tracker.bus_fault());
    REQUIRE(tracker.bus_stats().bus_faults == 2);
    REQUIRE(listener.count(Notice::BUS_FAULT) == 2);
    REQUIRE(listener.count(Notice::ALERT) == 2);
    REQUIRE(listener.count(Notice::BUS_RECOVERED) == 1);

    // The second episode's alternation starts at the fallback rate too (FR-025), from
    // whatever rate was in use — here the fallback rate, so the first change is the
    // reference-rate probe.
    REQUIRE(tracker.next_probe(0).bit_rate == omgp::TRUNK_bit_rate_fallback);
    REQUIRE(tracker.next_probe(0).bit_rate == omgp::TRUNK_bit_rate);
}

TEST_CASE("a whole fault episode allocates nothing", "[link]") {
    // FR-034 / CLAUDE.md rule 5: the embedded path never allocates after init.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    ThreeNodeRig::build(tracker); // declares, so the listener's vector grows here…

    HEAP_FREE_SCOPE({ // …and the measured scope adds at most 8 more notices, well inside
                      // the reserve
        probe_until(tracker, kNodeC, omgp::TRUNK_bit_rate_fallback);
        tracker.on_result(kNodeC, true, 10'000);
        uint64_t t = 11'000;
        for (int i = 0; i < 3; ++i) {
            const Probe p = tracker.next_probe(0);
            tracker.on_result(p.addr, false, t += 1'000);
        }
        tracker.tick(t + kThresholdUs);
        (void)tracker.bus_fault();
        (void)tracker.bit_rate();
        (void)tracker.bus_stats();
    });

    REQUIRE_FALSE(tracker.bus_fault());
}
