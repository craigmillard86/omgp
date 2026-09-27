# ESP-IDF component wrapping the portable host-core library in ../../../core (repo root is
# three levels up). SRC_DIRS is used because idf_component_register(SRCS ...) does not glob.
# build/gen/ must already exist: codegen runs on the host (pipeline.sh stage_esp32 calls
# stage_codegen first) because this image has no Jinja2. Unlike the two sibling components,
# this one is STATIC from its first commit — core/ already carries core_engine.cpp (T015,
# #686), so there is no source-less INTERFACE phase here and no reason to repeat
# omgp_link/CMakeLists.txt's get_target_property guard: the condition it exists for
# (idf_component_register producing an INTERFACE library, on which target_compile_options
# PRIVATE is a configure error) can never be true for this file. omgp_l3/CMakeLists.txt has
# the same unconditional shape for the same reason.
set(OMGP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
# REQUIRES omgp_link, mirroring core/CMakeLists.txt's `target_link_libraries(omgp_core PUBLIC
# omgp_link)`: core_engine.cpp uses link::Master and link::HealthTracker, and core/'s public
# header includes link/master.hpp and link/health.hpp, so the dependency is public, not PRIV_.
# Not omgp_l3 — no core/ source includes an l3/ header yet; it joins this line with the first
# codec user (T020-T022), and declaring it now would be a dependency nothing here uses.
# tools/refimpl/test_esp32_core_component.py derives this set from core/'s own #include lines
# and fails on either a missing or an extra entry, so the comment cannot drift from the code.
idf_component_register(
  SRC_DIRS     "${OMGP_ROOT}/core"
  INCLUDE_DIRS "${OMGP_ROOT}/core" "${OMGP_ROOT}/build/gen" "${OMGP_ROOT}"
  REQUIRES     omgp_link
)
# This is the only toolchain in the tree that COMPILES core/ -fno-rtti. Not the native preset:
# CoreEngine is core/'s first polymorphic class and its key function on_notice() is defined in
# core_engine.cpp, so -fno-rtti there emits no typeinfo and every RTTI-enabled, -fsanitize=vptr
# test TU that constructs one fails to link (docs/OPEN-QUESTIONS.md, 2026-09-27, PENDING human
# ruling; core/CMakeLists.txt carries the same finding, with the measurement). That entry's
# recommended option C is this file: under the pinned espressif/idf toolchain nothing is
# RTTI-enabled and no sanitizer runs, so the flag holds with nothing to trade for it.
#
# What this LINE contributes, stated exactly (CLAUDE.md rule 11), because it is less than it
# looks: espressif/idf v5.3 already puts -fno-exceptions -fno-rtti in the global C++ flags
# (CONFIG_COMPILER_CXX_EXCEPTIONS / _CXX_RTTI default off), so at this sdkconfig the flags would
# reach core/ without it and CMake deduplicates these two copies away entirely — measured in
# esp32-host/build/build.ninja, where the core component's FLAGS carry exactly one of each, in
# the global position. What the line does buy is that core/ keeps them if that project-wide
# default ever changes: target options are appended AFTER the global block, so the last flag
# wins. DEMONSTRATED for the ordering, not assumed — a probe -fno-threadsafe-statics added here
# appeared after -std=gnu++2b -fno-exceptions -fno-rtti in that same FLAGS line (measured, then
# reverted; it is not in the tree). INFERRED, not measured: that a future -fexceptions global
# would therefore be overridden here — nobody has built that sdkconfig.
#
# And what the whole component buys: core/ is now COMPILED by the toolchain that ships, which is
# where -fno-rtti acts. It is NOT LINKED into the firmware image — nothing in app_main()
# references a core/ symbol, so every member of libomgp_core.a is dropped from the link, the
# state tools/refimpl/test_esp32_link_smoke.py's docstring measured for omgp_link's own T003
# stub. T045 (#716)'s esp32-host/main/core_smoke.cpp is what closes that half.
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions -fno-rtti)
