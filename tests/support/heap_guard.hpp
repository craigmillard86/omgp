// Test-support: counting heap guard (spec 001 research R-10, CLAUDE.md rule 5).
// Every Catch2 test links with -Wl,--wrap=malloc,calloc,realloc,_Znwm,_Znam and
// tests/support/heap_guard.cpp, whose wrappers forward to the real allocators and bump
// omgp_test::heap_calls. Wrap a codec call in HEAP_FREE_SCOPE to assert it allocated
// nothing. A wrapper that aborted instead would kill Catch2's own allocations.
#pragma once

namespace omgp_test {
extern volatile unsigned long heap_calls;
} // namespace omgp_test

// Usage: HEAP_FREE_SCOPE({ st = omgp::l3::encode_header(h, buf, sizeof buf, n); });
#define HEAP_FREE_SCOPE(...)                                                                       \
    do {                                                                                           \
        const unsigned long omgp_heap_before_ = omgp_test::heap_calls;                             \
        __VA_ARGS__;                                                                               \
        REQUIRE(omgp_test::heap_calls == omgp_heap_before_);                                       \
    } while (0)
