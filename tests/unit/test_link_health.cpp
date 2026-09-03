// trunk §6, §7: pins HealthTracker's per-address ENROLLED/SUSPECT/OFFLINE lifecycle —
// every transition and boundary non-transition of data-model.md §6 "Node health record" —
// with an explicit FakeClock and a recording HealthListener, written before
// link/health.hpp exists (CLAUDE.md rule 8; spec 002 T037, US4). Bus-fault behaviour
// (BUS_FAULT/ALERT/BUS_RECOVERED) is US5 and out of scope here.
// Contract: specs/002-trunk-link-layer/contracts/link-cpp.md "Health tracker"
#include "catch_amalgamated.hpp"
#include "fake_clock.hpp"
#include "heap_guard.hpp"
#include "link/health.hpp"
#include "link/link_types.hpp"
#include "omgp_protocol.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace omgp::link;
using omgp_test::FakeClock;

namespace {

// Records every (Notice, addr) delivered, in delivery order, so tests can assert both
// content and count (spec SC-006: exactly one notification per transition).
struct RecordingListener : HealthListener {
    struct Entry {
        Notice notice;
        uint8_t addr;
    };
    std::vector<Entry> entries;

    // Reserves up front so growth during a HEAP_FREE_SCOPE measures HealthTracker's own
    // allocations, not this recording harness's vector doubling (well above any scripted
    // sequence's transition count in this file).
    RecordingListener() {
        entries.reserve(16);
    }

    void on_notice(Notice notice, uint8_t addr) override {
        entries.push_back({notice, addr});
    }
};

constexpr uint8_t kAddr = omgp::ADDR_backplane_min; // 0x01: a representative peer address

// Enrols `addr` at `base` then drives exactly TRUNK_suspect_after_failures consecutive
// failures, landing on SUSPECT with suspect_since == the returned time (data-model.md §6
// row "ENROLLED | fail, failures+1 == 3 | SUSPECT (suspect_since = now)").
uint64_t drive_to_suspect(HealthTracker& tracker, uint8_t addr, uint64_t base = 0) {
    tracker.on_result(addr, true, base);
    for (uint32_t i = 1; i <= omgp::TRUNK_suspect_after_failures; ++i)
        tracker.on_result(addr, false, base + i);
    return base + omgp::TRUNK_suspect_after_failures;
}

// Drives `addr` to SUSPECT, then past the OFFLINE threshold via tick() alone (no
// intervening on_result), landing on OFFLINE. Returns suspect_since.
uint64_t drive_to_offline(HealthTracker& tracker, uint8_t addr, uint64_t base = 0) {
    uint64_t suspect_since = drive_to_suspect(tracker, addr, base);
    tracker.tick(suspect_since + omgp::TRUNK_offline_after_suspect_ms * 1000);
    return suspect_since;
}

} // namespace

TEST_CASE("UNENROLLED to ENROLLED on the first valid result", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    tracker.on_result(kAddr, true, 0);

    REQUIRE(tracker.state(kAddr) == HealthState::ENROLLED);
    REQUIRE(listener.entries.size() == 1);
    REQUIRE(listener.entries[0].notice == Notice::ENROLLED);
    REQUIRE(listener.entries[0].addr == kAddr);
}

TEST_CASE("UNENROLLED never counts failures, however many", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    HEAP_FREE_SCOPE({
        for (uint32_t i = 0; i < omgp::TRUNK_suspect_after_failures + 1; ++i)
            tracker.on_result(kAddr, false, i);
    });

    REQUIRE(tracker.state(kAddr) == HealthState::UNENROLLED);
    REQUIRE(listener.entries.empty());
}

TEST_CASE("ENROLLED tolerates two consecutive failures and resets the count on success", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    tracker.on_result(kAddr, true, 0);
    tracker.on_result(kAddr, false, 1);
    tracker.on_result(kAddr, false, 2);
    REQUIRE(tracker.state(kAddr) == HealthState::ENROLLED);
    REQUIRE(listener.entries.size() == 1); // only the initial ENROLLED notice

    tracker.on_result(kAddr, true, 3); // 3rd call succeeds: resets failures, stays ENROLLED
    REQUIRE(tracker.state(kAddr) == HealthState::ENROLLED);
    REQUIRE(listener.entries.size() == 1);

    // Prove the count actually reset to 0: it takes a fresh run of
    // TRUNK_suspect_after_failures failures (not fewer) to reach SUSPECT.
    for (uint32_t i = 0; i < omgp::TRUNK_suspect_after_failures - 1; ++i) {
        tracker.on_result(kAddr, false, 4 + i);
        REQUIRE(tracker.state(kAddr) == HealthState::ENROLLED);
    }
    tracker.on_result(kAddr, false, 3 + omgp::TRUNK_suspect_after_failures);
    REQUIRE(tracker.state(kAddr) == HealthState::SUSPECT);
    REQUIRE(listener.entries.size() == 2);
}

TEST_CASE("ENROLLED to SUSPECT on the Nth consecutive failure", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    drive_to_suspect(tracker, kAddr);

    REQUIRE(tracker.state(kAddr) == HealthState::SUSPECT);
    REQUIRE(listener.entries.size() == 2);
    REQUIRE(listener.entries[0].notice == Notice::ENROLLED);
    REQUIRE(listener.entries[1].notice == Notice::SUSPECT);
    REQUIRE(listener.entries[1].addr == kAddr);
}

TEST_CASE("SUSPECT to ENROLLED (RECOVERED) on a valid result", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    uint64_t suspect_since = drive_to_suspect(tracker, kAddr);
    tracker.on_result(kAddr, true, suspect_since + 1);

    REQUIRE(tracker.state(kAddr) == HealthState::ENROLLED);
    REQUIRE(listener.entries.size() == 3);
    REQUIRE(listener.entries[2].notice == Notice::RECOVERED);
    REQUIRE(listener.entries[2].addr == kAddr);
}

TEST_CASE("SUSPECT stays SUSPECT via tick just short of the OFFLINE boundary", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    uint64_t suspect_since = drive_to_suspect(tracker, kAddr);
    size_t before = listener.entries.size();
    tracker.tick(suspect_since + (omgp::TRUNK_offline_after_suspect_ms - 1) * 1000);

    REQUIRE(tracker.state(kAddr) == HealthState::SUSPECT);
    REQUIRE(listener.entries.size() == before);
}

TEST_CASE("SUSPECT stays SUSPECT via a failing on_result just short of the OFFLINE boundary",
          "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    uint64_t suspect_since = drive_to_suspect(tracker, kAddr);
    size_t before = listener.entries.size();
    tracker.on_result(kAddr, false,
                      suspect_since + (omgp::TRUNK_offline_after_suspect_ms - 1) * 1000);

    REQUIRE(tracker.state(kAddr) == HealthState::SUSPECT);
    REQUIRE(listener.entries.size() == before);
}

TEST_CASE("SUSPECT to OFFLINE via tick alone at the boundary, no on_result involved", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    uint64_t suspect_since = drive_to_suspect(tracker, kAddr);
    size_t before = listener.entries.size();
    tracker.tick(suspect_since + omgp::TRUNK_offline_after_suspect_ms * 1000);

    REQUIRE(tracker.state(kAddr) == HealthState::OFFLINE);
    REQUIRE(listener.entries.size() == before + 1);
    REQUIRE(listener.entries.back().notice == Notice::OFFLINE);
    REQUIRE(listener.entries.back().addr == kAddr);
}

TEST_CASE("SUSPECT to OFFLINE via a failing on_result at the boundary", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    uint64_t suspect_since = drive_to_suspect(tracker, kAddr);
    size_t before = listener.entries.size();
    tracker.on_result(kAddr, false, suspect_since + omgp::TRUNK_offline_after_suspect_ms * 1000);

    REQUIRE(tracker.state(kAddr) == HealthState::OFFLINE);
    REQUIRE(listener.entries.size() == before + 1);
    REQUIRE(listener.entries.back().notice == Notice::OFFLINE);
    REQUIRE(listener.entries.back().addr == kAddr);
}

TEST_CASE("OFFLINE to ENROLLED (RECOVERED) on a valid result", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    uint64_t suspect_since = drive_to_offline(tracker, kAddr);
    size_t before = listener.entries.size();
    tracker.on_result(kAddr, true, suspect_since + omgp::TRUNK_offline_after_suspect_ms * 1000 + 1);

    REQUIRE(tracker.state(kAddr) == HealthState::ENROLLED);
    REQUIRE(listener.entries.size() == before + 1);
    REQUIRE(listener.entries.back().notice == Notice::RECOVERED);
    REQUIRE(listener.entries.back().addr == kAddr);
}

TEST_CASE("OFFLINE stays OFFLINE on further failures", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    uint64_t suspect_since = drive_to_offline(tracker, kAddr);
    size_t before = listener.entries.size();
    tracker.on_result(kAddr, false,
                      suspect_since + omgp::TRUNK_offline_after_suspect_ms * 1000 + 1);

    REQUIRE(tracker.state(kAddr) == HealthState::OFFLINE);
    REQUIRE(listener.entries.size() == before);
}

TEST_CASE("notice count equals transition count across a full scripted lifecycle", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    uint64_t t = 0;
    tracker.on_result(kAddr, true, t);    // UNENROLLED -> ENROLLED           : ENROLLED
    tracker.on_result(kAddr, false, ++t); // ENROLLED, failures=1             : —
    tracker.on_result(kAddr, false, ++t); // ENROLLED, failures=2             : —
    tracker.on_result(kAddr, false, ++t); // ENROLLED -> SUSPECT              : SUSPECT
    tracker.on_result(kAddr, true, ++t);  // SUSPECT -> ENROLLED              : RECOVERED
    tracker.on_result(kAddr, false, ++t); // ENROLLED, failures=1             : —
    tracker.on_result(kAddr, false, ++t); // ENROLLED, failures=2             : —
    tracker.on_result(kAddr, false, ++t); // ENROLLED -> SUSPECT              : SUSPECT
    uint64_t suspect_since = t;
    tracker.tick(suspect_since +
                 omgp::TRUNK_offline_after_suspect_ms * 1000); // -> OFFLINE : OFFLINE
    t = suspect_since + omgp::TRUNK_offline_after_suspect_ms * 1000;
    tracker.on_result(kAddr, false, ++t); // OFFLINE, stays                  : —
    tracker.on_result(kAddr, true, ++t);  // OFFLINE -> ENROLLED             : RECOVERED

    REQUIRE(tracker.state(kAddr) == HealthState::ENROLLED);
    REQUIRE(listener.entries.size() == 6);
    Notice expected[6] = {Notice::ENROLLED, Notice::SUSPECT, Notice::RECOVERED,
                          Notice::SUSPECT,  Notice::OFFLINE, Notice::RECOVERED};
    for (size_t i = 0; i < 6; ++i) {
        REQUIRE(listener.entries[i].notice == expected[i]);
        REQUIRE(listener.entries[i].addr == kAddr);
    }
}

TEST_CASE("poll_due for a SUSPECT node follows the 10x T_poll reduced-rate rule",
          "[timing:T_poll]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    drive_to_suspect(tracker, kAddr);
    uint64_t last_poll = 1'000'000;
    tracker.mark_polled(kAddr, last_poll);

    REQUIRE_FALSE(tracker.poll_due(kAddr, last_poll + 9 * omgp::TRUNK_T_poll_us));
    REQUIRE(tracker.poll_due(kAddr, last_poll + 10 * omgp::TRUNK_T_poll_us));
}

TEST_CASE("poll_due is true for ENROLLED regardless of last poll time", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    tracker.on_result(kAddr, true, 0);
    uint64_t last_poll = 500;
    tracker.mark_polled(kAddr, last_poll);

    REQUIRE(tracker.poll_due(kAddr, last_poll));
    REQUIRE(tracker.poll_due(kAddr, last_poll + 1));
    REQUIRE(tracker.poll_due(kAddr, last_poll + 10 * omgp::TRUNK_T_poll_us));
}

TEST_CASE("poll_due is false for OFFLINE and UNENROLLED", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    REQUIRE_FALSE(tracker.poll_due(kAddr, 0)); // UNENROLLED: never touched

    uint8_t offline_addr = omgp::ADDR_backplane_min + 1;
    uint64_t suspect_since = drive_to_offline(tracker, offline_addr);
    uint64_t offline_at = suspect_since + omgp::TRUNK_offline_after_suspect_ms * 1000;
    tracker.mark_polled(offline_addr, offline_at);

    REQUIRE_FALSE(tracker.poll_due(offline_addr, offline_at + 10 * omgp::TRUNK_T_poll_us));
}

TEST_CASE("next_probe rotates round-robin over UNENROLLED/OFFLINE addresses only", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    const uint8_t enrolled_addr = omgp::ADDR_backplane_min;    // 0x01, must never be returned
    const uint8_t suspect_addr = omgp::ADDR_backplane_min + 1; // 0x02, must never be returned
    const uint8_t offline_addr = omgp::ADDR_backplane_min + 2; // 0x03, eligible

    // offline_addr is driven through its whole SUSPECT->OFFLINE cycle at t ~ 0..1e6 first;
    // suspect_addr only starts failing well after that tick has already landed, so the
    // same global tick() that carries offline_addr past its threshold cannot also carry
    // suspect_addr past its own (data-model.md §6 OFFLINE is per-record suspect_since, but
    // tick(now) is evaluated against every SUSPECT record at once — colliding timestamps
    // would make both go OFFLINE together, which is not what this test is scripting).
    tracker.on_result(enrolled_addr, true, 0);
    drive_to_offline(tracker, offline_addr);
    drive_to_suspect(tracker, suspect_addr, 2'000'000);

    std::vector<uint8_t> eligible;
    for (uint8_t a = omgp::ADDR_backplane_min; a <= omgp::ADDR_backplane_max; ++a)
        if (a != enrolled_addr && a != suspect_addr)
            eligible.push_back(a); // UNENROLLED (never touched) or the driven-OFFLINE address

    std::vector<uint8_t> seen;
    for (size_t i = 0; i < eligible.size(); ++i) {
        Probe probe = tracker.next_probe(0);
        REQUIRE(probe.addr != omgp::ADDR_host);
        REQUIRE(probe.addr != enrolled_addr);
        REQUIRE(probe.addr != suspect_addr);
        REQUIRE(std::find(eligible.begin(), eligible.end(), probe.addr) != eligible.end());
        seen.push_back(probe.addr);
    }

    // Round-robin: one full pass visits every eligible address exactly once.
    std::vector<uint8_t> sorted_seen = seen;
    std::sort(sorted_seen.begin(), sorted_seen.end());
    std::vector<uint8_t> sorted_eligible = eligible;
    std::sort(sorted_eligible.begin(), sorted_eligible.end());
    REQUIRE(sorted_seen == sorted_eligible);

    // bus_fault()/bit_rate() are unconditional T038 stubs (bus_fault() always returns
    // false, bit_rate() always TRUNK_bit_rate) — this pins the stub, not a property of
    // `enrolled_addr` staying ENROLLED, which the stub never reads.
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.next_probe(0).bit_rate == omgp::TRUNK_bit_rate);
}

TEST_CASE("next_probe returns ADDR_host when no UNENROLLED/OFFLINE address exists", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    // Every backplane address ENROLLED: the steady state of a healthy rig, and the one
    // case data-model.md §6's "round-robin over addresses whose state is UNENROLLED or
    // OFFLINE" has no candidate for. next_probe must signal that rather than handing back
    // an ENROLLED/SUSPECT address the caller would mistake for a real probe target.
    for (uint8_t a = omgp::ADDR_backplane_min; a <= omgp::ADDR_backplane_max; ++a)
        tracker.on_result(a, true, 0);

    Probe probe = tracker.next_probe(0);
    REQUIRE(probe.addr == omgp::ADDR_host);
    // The sentinel still carries the real bit rate (deep-verify on #118: the bit_rate()
    // call on the no-candidate return was asserted nowhere).
    REQUIRE(probe.bit_rate == omgp::TRUNK_bit_rate);

    // Still no candidate on a second call: the "no eligible address" signal is stable,
    // not a one-shot artefact of where the rotation cursor happened to land.
    probe = tracker.next_probe(0);
    REQUIRE(probe.addr == omgp::ADDR_host);
    REQUIRE(probe.bit_rate == omgp::TRUNK_bit_rate);
}

TEST_CASE("a failed sentinel pass leaves the cursor exactly one full cycle on", "[link]") {
    // deep-verify on #118: the loop bound's off-by-one advanced the cursor one EXTRA step
    // on the no-candidate path. One sentinel pass here, then the two lowest backplane
    // addresses become eligible: a correct cursor (back at its start) scans min first;
    // an overshot cursor sits ON min and would scan min+1 first. Exactly one sentinel
    // call, deliberately — a second pass wraps the drift past the discriminating slot.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    for (uint8_t a = omgp::ADDR_backplane_min; a <= omgp::ADDR_backplane_max; ++a)
        tracker.on_result(a, true, 0);
    REQUIRE(tracker.next_probe(0).addr == omgp::ADDR_host); // the single sentinel pass

    drive_to_offline(tracker, omgp::ADDR_backplane_min, 5000000);
    drive_to_offline(tracker, static_cast<uint8_t>(omgp::ADDR_backplane_min + 1), 9000000);
    REQUIRE(tracker.next_probe(0).addr == omgp::ADDR_backplane_min);
}

TEST_CASE("recovery from SUSPECT resets the failure count — three fresh failures needed again",
          "[link]") {
    // deep-verify on #118: the `consecutive_failures = 0` on the SUSPECT->ENROLLED
    // transition was asserted nowhere — a constant-assign mutant survived. Two failures
    // after a recovery must NOT re-suspect; the third must.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    drive_to_suspect(tracker, kAddr);
    tracker.on_result(kAddr, true, 100); // RECOVERED
    REQUIRE(tracker.state(kAddr) == HealthState::ENROLLED);
    for (uint32_t i = 1; i < omgp::TRUNK_suspect_after_failures; ++i) {
        tracker.on_result(kAddr, false, 100 + i);
        REQUIRE(tracker.state(kAddr) == HealthState::ENROLLED);
    }
    tracker.on_result(kAddr, false, 200);
    REQUIRE(tracker.state(kAddr) == HealthState::SUSPECT);
}

TEST_CASE("recovery from OFFLINE resets the failure count — three fresh failures needed again",
          "[link]") {
    // deep-verify on #118: same shape as the SUSPECT-recovery reset, on the
    // OFFLINE->ENROLLED transition.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    drive_to_offline(tracker, kAddr);
    tracker.on_result(kAddr, true, 2000000); // RECOVERED
    REQUIRE(tracker.state(kAddr) == HealthState::ENROLLED);
    for (uint32_t i = 1; i < omgp::TRUNK_suspect_after_failures; ++i) {
        tracker.on_result(kAddr, false, 2000000 + i);
        REQUIRE(tracker.state(kAddr) == HealthState::ENROLLED);
    }
    tracker.on_result(kAddr, false, 2000100);
    REQUIRE(tracker.state(kAddr) == HealthState::SUSPECT);
}

TEST_CASE("out-of-range addresses are not nodes: no record, no notice, no effect", "[link]") {
    // red-team on #118 M1/M14: `addr` is wire-derived (link/frame.cpp validates only
    // dst == 0xFF; src is copied through), so the bounds guard is the sole memory-safety
    // control on records_[kAddrCount] — and reverting it, or "fixing" it with a modulo
    // that aliases 0x11 onto backplane 0x01's record, must fail THIS test.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    tracker.on_result(kAddr, true, 0); // one real record the invalid addresses must not touch
    REQUIRE(listener.entries.size() == 1);

    for (uint8_t bad : {uint8_t{0x10}, uint8_t{0x42}, uint8_t{0xFF}}) {
        tracker.on_result(bad, true, 1);
        tracker.on_result(bad, false, 2);
        tracker.mark_polled(bad, 3);
        REQUIRE(tracker.state(bad) == HealthState::UNENROLLED);
        REQUIRE_FALSE(tracker.poll_due(bad, 4));
    }
    // 0x10 % kAddrCount == 0x00 and 0x11 % kAddrCount == 0x01: an aliasing "guard" would
    // have disturbed a real record. The in-range record is untouched and nothing notified.
    tracker.on_result(uint8_t{0x11}, false, 5);
    REQUIRE(tracker.state(kAddr) == HealthState::ENROLLED);
    REQUIRE(listener.entries.size() == 1);
}

TEST_CASE("ADDR_host is not a node: on_result and a hostile lifecycle leave it untracked",
          "[link]") {
    // red-team on #118 finding 1: 0x00 is the host's own address (trunk §5). next_probe
    // already excludes it and uses it as the no-candidate sentinel; on_result must agree,
    // or an echoed/hostile src=0x00 frame enrols the host, poll_due tells F3 to poll it
    // forever, and every notice carries addr 0 — which data-model §9 reserves for
    // bus-level notices.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    tracker.on_result(omgp::ADDR_host, true, 0);
    REQUIRE(tracker.state(omgp::ADDR_host) == HealthState::UNENROLLED);
    REQUIRE_FALSE(tracker.poll_due(omgp::ADDR_host, 1));
    REQUIRE(listener.entries.empty());

    // Full hostile lifecycle: enrol, drive to SUSPECT, tick past the OFFLINE threshold.
    tracker.on_result(omgp::ADDR_host, true, 0);
    for (uint32_t i = 1; i <= omgp::TRUNK_suspect_after_failures; ++i)
        tracker.on_result(omgp::ADDR_host, false, i);
    tracker.tick(omgp::TRUNK_suspect_after_failures +
                 uint64_t{omgp::TRUNK_offline_after_suspect_ms} * 1000);
    REQUIRE(tracker.state(omgp::ADDR_host) == HealthState::UNENROLLED);
    REQUIRE(listener.entries.empty());
}

TEST_CASE("an earlier now_us than a stored stamp reads as zero elapsed, never a wraparound",
          "[link]") {
    // red-team on #118 finding 2: now_us is a caller-supplied parameter with no
    // monotonicity contract across the four entry points. One stamp earlier than
    // suspect_since wrapped the uint64_t delta to ~1.8e19 µs — an instant, silent
    // OFFLINE (and, in poll_due, a SUSPECT node silently restored to full poll rate).
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    const uint64_t suspect_since = drive_to_suspect(tracker, kAddr, 1000);
    const auto notices_after_suspect = listener.entries.size();

    tracker.tick(suspect_since - 1);
    REQUIRE(tracker.state(kAddr) == HealthState::SUSPECT);

    tracker.on_result(kAddr, false, suspect_since - 1);
    REQUIRE(tracker.state(kAddr) == HealthState::SUSPECT);
    REQUIRE(listener.entries.size() == notices_after_suspect);

    tracker.mark_polled(kAddr, 1000000);
    REQUIRE_FALSE(tracker.poll_due(kAddr, 999999)); // 0 µs since last poll, not ~1.8e19
}

TEST_CASE("next_probe's cursor stays inside the backplane range from every state", "[link]") {
    // red-team on #118 M12: an off-by-one in the wrap comparison walked the cursor to
    // 0x10 and read records_[16] — an intra-object overflow ASan's default config does
    // not flag. Two full passes over an all-eligible table must yield exactly the
    // backplane sequence, twice, with every address in range.
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    constexpr size_t kBackplaneCount =
        static_cast<size_t>(omgp::ADDR_backplane_max) - omgp::ADDR_backplane_min + 1;
    std::vector<uint8_t> seq;
    for (size_t i = 0; i < 2 * kBackplaneCount; ++i) {
        const Probe probe = tracker.next_probe(0);
        REQUIRE(probe.addr >= omgp::ADDR_backplane_min);
        REQUIRE(probe.addr <= omgp::ADDR_backplane_max);
        seq.push_back(probe.addr);
    }
    for (size_t i = 0; i < seq.size(); ++i)
        REQUIRE(seq[i] == omgp::ADDR_backplane_min + static_cast<uint8_t>(i % kBackplaneCount));
}

TEST_CASE("a full on_result/tick/poll_due/next_probe run allocates nothing", "[link]") {
    FakeClock clock;
    RecordingListener listener;
    HealthTracker tracker(clock, listener);

    HEAP_FREE_SCOPE({
        uint64_t suspect_since = drive_to_suspect(tracker, kAddr);
        tracker.mark_polled(kAddr, suspect_since);
        (void)tracker.poll_due(kAddr, suspect_since + 10 * omgp::TRUNK_T_poll_us);
        tracker.tick(suspect_since + omgp::TRUNK_offline_after_suspect_ms * 1000);
        (void)tracker.next_probe(suspect_since);
        (void)tracker.state(kAddr);
        (void)tracker.bus_fault();
        (void)tracker.bit_rate();
    });
}
