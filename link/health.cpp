// OMGP trunk L2 — node health tracker implementation: trunk §6, §7; transition table
// data-model.md §6. Makes tests/unit/test_link_health.cpp (T037) pass.
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
    return now_us >= since_us ? now_us - since_us : 0;
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
constexpr bool is_node_addr(uint8_t addr) {
    return addr != omgp::ADDR_host && addr < kAddrCount;
}
} // namespace

HealthTracker::HealthTracker(Clock& clock, HealthListener& listener)
    : clock_(clock), listener_(listener), records_{}, next_probe_addr_(omgp::ADDR_backplane_max) {
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
    for (size_t addr = 0; addr < kAddrCount; ++addr) {
        HealthRecord& r = records_[addr];
        if (r.state == HealthState::SUSPECT &&
            elapsed_us(now_us, r.suspect_since_us) >= kOfflineThresholdUs) {
            r.state = HealthState::OFFLINE;
            notify(Notice::OFFLINE, static_cast<uint8_t>(addr));
        }
    }
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

Probe HealthTracker::next_probe(uint64_t /*now_us*/) {
    // data-model.md §6: round-robin over UNENROLLED/OFFLINE addresses in
    // [ADDR_backplane_min, ADDR_backplane_max] (0x00 is the host and is never a candidate).
    // Bounded by the full backplane range so an all-ENROLLED/SUSPECT table can never spin.
    constexpr size_t kBackplaneCount =
        static_cast<size_t>(omgp::ADDR_backplane_max) - omgp::ADDR_backplane_min + 1;
    for (size_t i = 0; i < kBackplaneCount; ++i) {
        next_probe_addr_ = next_backplane_addr(next_probe_addr_);
        const HealthState s = records_[next_probe_addr_].state;
        if (s == HealthState::UNENROLLED || s == HealthState::OFFLINE)
            return Probe{next_probe_addr_, bit_rate()};
    }
    // No eligible address (e.g. every backplane node is ENROLLED/SUSPECT — the steady
    // state of a healthy rig): ADDR_host (0x00) is outside the rotation range and is
    // asserted by tests/unit/test_link_health.cpp to never be a real probe target, so it
    // signals "nothing to probe" without adding a field the contract doesn't declare.
    // T043 revisits this under bus fault.
    return Probe{omgp::ADDR_host, bit_rate()};
}

bool HealthTracker::bus_fault() const {
    return false; // T043 (US5) implements declare/clear; stubbed per tasks.md T038.
}

uint32_t HealthTracker::bit_rate() const {
    return omgp::TRUNK_bit_rate; // T043 (US5) pins the recovered rate; stubbed per T038.
}

} // namespace link
} // namespace omgp
