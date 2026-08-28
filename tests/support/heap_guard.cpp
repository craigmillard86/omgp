// Test-support: counting allocator wrappers (see heap_guard.hpp). Host-only test code;
// requires GNU ld --wrap, which the native CMake target and pipeline.sh bootstrap apply.
#include "heap_guard.hpp"

#include <cstddef>
#include <new>

namespace omgp_test {
volatile unsigned long heap_calls = 0;
} // namespace omgp_test

extern "C" {
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);

void* __wrap_malloc(size_t n) {
    ++omgp_test::heap_calls;
    return __real_malloc(n);
}
void* __wrap_calloc(size_t a, size_t b) {
    ++omgp_test::heap_calls;
    return __real_calloc(a, b);
}
void* __wrap_realloc(void* p, size_t n) {
    ++omgp_test::heap_calls;
    return __real_realloc(p, n);
}
}

// operator new(size_t) and operator new[](size_t) — Itanium ABI mangled names, so a
// codec that says `new` is counted even though libstdc++'s own malloc call is not
// visible to --wrap (it lives inside the shared library). All four names must be
// extern "C": they are linker-level symbols, not C++ functions to be mangled again.
extern "C" {
void* __real__Znwm(size_t);
void* __real__Znam(size_t);
void* __wrap__Znwm(size_t n) {
    ++omgp_test::heap_calls;
    return __real__Znwm(n);
}
void* __wrap__Znam(size_t n) {
    ++omgp_test::heap_calls;
    return __real__Znam(n);
}
}
