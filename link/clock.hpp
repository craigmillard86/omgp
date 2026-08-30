// trunk §3: media access and the timing model are measured against a single
// injected monotonic clock (CLAUDE.md rule 3) — no wall clock, no sleep.
#pragma once
#include <cstdint>

namespace omgp {

struct Clock {
    virtual uint64_t now_us() = 0; // monotonic microseconds; never goes backwards
  protected:
    ~Clock() = default;
};

} // namespace omgp
