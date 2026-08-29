// Test-support: prints "EXECUTED: <n>" (total assertions evaluated) when a Catch2 run
// ends, so pipeline.sh's UNIT_TEST_FLOOR can count checks across every test binary
// (spec 001 FR-033). Linked into every Catch2 test; never built as a test itself.
#include "catch_amalgamated.hpp"

#include <cstdio>

namespace {

class ExecutedCountListener : public Catch::EventListenerBase {
  public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunEnded(Catch::TestRunStats const& stats) override {
        std::printf("EXECUTED: %llu\n",
                    static_cast<unsigned long long>(stats.totals.assertions.total()));
        std::fflush(stdout);
    }
};

} // namespace

CATCH_REGISTER_LISTENER(ExecutedCountListener)
