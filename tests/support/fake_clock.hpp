// Test-support: deterministic clock for link/ engine tests (spec 002 T010).
// contracts/byte-wire-and-clock.md "omgp::Clock"; CLAUDE.md rule 3 (all time flows
// through the injected Clock; tests advance it explicitly, nothing sleeps).
#pragma once

#include "link/clock.hpp"

#include <cstdint>

namespace omgp_test {

struct FakeClock : omgp::Clock {
    uint64_t t = 0;

    uint64_t now_us() override {
        return t;
    }
    void advance(uint64_t us) {
        t += us;
    }
    void set(uint64_t us) {
        t = us;
    }
};

} // namespace omgp_test
