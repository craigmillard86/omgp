// trunk §3: media access and the timing model are measured against a single
// injected monotonic clock (CLAUDE.md rule 3) — no wall clock, no sleep.
// mutation-exempt(no-body): pure abstract interface, one `= 0` declaration — no function
// body for Mull to mutate (tools/mutate_report.py; docs/OPEN-QUESTIONS.md 2026-08-30).
#pragma once
#include <cstdint>

namespace omgp {

struct Clock {
    virtual uint64_t now_us() = 0; // monotonic microseconds; never goes backwards
  protected:
    ~Clock() = default;
};

} // namespace omgp
