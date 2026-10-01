# omgp_core — portable host-core (core/). STATIC since T015 (core_engine.cpp is its first source
# file; specs/003-host-core-engine/tasks.md T001/T015) — the same INTERFACE-then-STATIC path
# link/CMakeLists.txt took (spec 002's T001/T008). The -fno-exceptions below makes CLAUDE.md rule
# 5's "no exceptions" compiler-enforced for core/; its "no RTTI" half is NOT — see the long note on
# the target_compile_options line for why, and what does enforce it.
# core/'s dependency surface (specs/003-host-core-engine/research.md R-02, a stated divergence
# from CLAUDE.md's literal OMGPTransport wording; contracts/core-cpp.md):
# link::Master / link::HealthTracker / Clock via link/master.hpp, link/health.hpp,
# link/clock.hpp, link/link_types.hpp, plus l3/ and the generated protocol header — never
# link/responder.hpp, link/frame.hpp, link/byte_wire.hpp, sim/, cli/ or a platform header.
add_library(omgp_core STATIC
  core_engine.cpp
)
# Both, since T020-T022 (#691-#693): core_engine.cpp needs link::Master and link::HealthTracker,
# and now also l3/'s header, payload and descriptor codecs for IDENTIFY, READ_DESC, BP_SLOT_MAP
# and the descriptor CRC — and core/core_engine.hpp includes l3/l3_types.hpp for
# reconcile_slot_map()'s parameter, so omgp_l3 is PUBLIC rather than PRIVATE. Both are permitted
# by contracts/core-cpp.md's surface; core/ may depend on l3/ and link/ alike because it is the
# layer above both, and that is not a channel between them (CLAUDE.md "Architecture invariants":
# L2 stays opaque to L3 because omgp_link itself never links omgp_l3 — see link/CMakeLists.txt).
target_link_libraries(omgp_core PUBLIC omgp_link omgp_l3)
target_include_directories(omgp_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/build/gen
)
target_compile_features(omgp_core PUBLIC cxx_std_17)
# -fno-exceptions only, NOT the -fno-rtti this file's T001 note predicted. Stated plainly because
# it is a gap, not a preference: on the native preset, `-fno-rtti` on this target and
# `-fsanitize=vptr` (part of -fsanitize=undefined, OMGP_SANITIZERS=ON) in the RTTI-enabled test
# TUs are mutually exclusive. CoreEngine is core/'s first polymorphic class (research.md R-03: it
# IS the link::HealthListener), its key function on_notice() is defined here, and with -fno-rtti
# this TU emits no typeinfo for it — so every test TU that constructs one fails to link:
#
#   /usr/bin/ld: CMakeFiles/test_core_callback_queue.dir/tests/unit/test_core_callback_queue.cpp.o:
#     (.data.rel+0x1f8): undefined reference to `typeinfo for omgp::core::CoreEngine'
#
# DEMONSTRATED attribution, not inferred: with `-fno-rtti` kept here, adding `-fno-sanitize=vptr`
# to test_core_callback_queue alone made the same build link (measured, then reverted — that flag
# is not in the tree). Two ways out, and the one rejected is the one that would have kept the flag:
# disabling UBSan's vptr check for every test binary that touches core/ trades a real sanitizer
# check on the exact class it exists to check, and the exception would spread to every later core/
# test. So the flag is dropped here instead, and CLAUDE.md rule 5's no-RTTI clause is enforced for
# core/ by (a) tools/check_embedded.py, which fails the quality stage on `dynamic_cast`/`typeid`
# anywhere under core/ (a source-level check, run on every path including the bootstrap one), and
# (b) the ESP-IDF component's own -fno-exceptions -fno-rtti on the target build — which does NOT
# exist yet: esp32-host/components/omgp_core/CMakeLists.txt is T003 (#674), still open, so today
# (a) is the whole of it. Labelled per CLAUDE.md rule 11: no-RTTI in core/ is currently a
# grep-level control over the repo's contents, NOT a compiler guarantee.
# Recorded for a human ruling in docs/OPEN-QUESTIONS.md, 2026-09-27 entry.
target_compile_options(omgp_core PRIVATE -fno-exceptions)
