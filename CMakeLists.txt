cmake_minimum_required(VERSION 3.22)
project(omgp CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # clang-tidy in the pipeline quality stage

# Codegen before anything compiles
execute_process(COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/codegen.py
                RESULT_VARIABLE CODEGEN_RC)
if(NOT CODEGEN_RC EQUAL 0)
  message(FATAL_ERROR "protocol codegen failed")
endif()
include_directories(${CMAKE_SOURCE_DIR}/build/gen)

add_compile_options(-Wall -Wextra -Werror)
if(OMGP_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g)
  add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()

# --- portable libraries (embedded path: CLAUDE.md rule 5 enforced per target) ---
add_subdirectory(l3)
add_subdirectory(link)
# core/ is STATIC since T015 (core_engine.cpp). Rule 5's no-exceptions half is compiler-enforced
# there (-fno-exceptions on omgp_core); its no-RTTI half is NOT — it is a tools/check_embedded.py
# source-level control, for the reason core/CMakeLists.txt states on its target_compile_options
# line (docs/OPEN-QUESTIONS.md, 2026-09-27, PENDING).
add_subdirectory(core)

# --- test framework: vendored Catch2 v3 (third_party/catch2/VERSION records tag + sha256) ---
add_library(catch2_amalgamated STATIC third_party/catch2/catch_amalgamated.cpp)
target_include_directories(catch2_amalgamated PUBLIC third_party/catch2)
target_compile_options(catch2_amalgamated PRIVATE -Wno-error) # third-party warnings never fail the build

# tests/support: linked into every Catch2 test, never a test itself. OBJECT library so the
# listener's static registrar and the --wrap symbols are always pulled into the link.
# mock_wire.cpp needs omgp_link (Deframer/encode_frame/link_types) — T010; mock_l3_node.cpp also
# needs omgp_l3 (it answers with typed L3 responses through the real payload/header codecs) —
# spec 003 T012, the one target that legitimately links both (L2 stays opaque to L3: the
# dependency is this host-only object library's, never omgp_link's).
add_library(omgp_test_support OBJECT tests/support/catch_listener.cpp tests/support/heap_guard.cpp
                                      tests/support/mock_wire.cpp tests/support/mock_l3_node.cpp)
target_include_directories(omgp_test_support PUBLIC tests/support)
target_link_libraries(omgp_test_support PUBLIC catch2_amalgamated omgp_link omgp_l3)

# Counting heap guard (tests/support/heap_guard.hpp): wrap the allocators our objects call.
set(OMGP_WRAP_LDFLAGS -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc
                      -Wl,--wrap=_Znwm -Wl,--wrap=_Znam)

# omgp_core is on this line (T001) and on no other, which is spec 003 T016's audited result:
# the only sources in the tree that include a core/ header are tests/unit/test_core_*.cpp, and
# every Catch2 binary is built here. omgp_test_support, omgp_canon, omgp_l3_helper, the fuzz
# targets, test_smoke, crc_helper and l3_helper were each checked and need no change — T016
# forbids adding omgp_core to a target that does not reference it. tools/refimpl/
# test_core_link_sets.py re-runs that audit in both directions on every `refimpl` stage.
function(omgp_add_catch_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE omgp_l3 omgp_link omgp_core omgp_test_support
                                        catch2_amalgamated)
  target_link_options(${name} PRIVATE ${OMGP_WRAP_LDFLAGS})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# --- host-only canonical-text codec (C++ side of the differential test) ---
add_library(omgp_canon STATIC tools/canonical.cpp)
target_include_directories(omgp_canon PUBLIC tools)
target_link_libraries(omgp_canon PUBLIC omgp_l3 omgp_link)

# --- l3_helper's verb table, split out so its dispatch is unit testable (test_l3_helper_dispatch)
#     without spawning the built binary ---
add_library(omgp_l3_helper STATIC tools/l3_helper_dispatch.cpp)
target_include_directories(omgp_l3_helper PUBLIC tools)
target_link_libraries(omgp_l3_helper PUBLIC omgp_canon omgp_link)

# --- fuzz targets (clang only; `fuzz` preset) — the rest of the tree is skipped there so a
#     fuzz build stays fast and never needs the Catch2 object ---
if(OMGP_FUZZ)
  add_subdirectory(tests/fuzz)
  return()
endif()

# --- tests ---
add_executable(test_smoke tests/unit/test_smoke.cpp) # standalone main; kept as the floor's baseline
add_test(NAME smoke COMMAND test_smoke)
omgp_add_catch_test(test_l3_types tests/unit/test_l3_types.cpp)
omgp_add_catch_test(test_gen_constants tests/unit/test_gen_constants.cpp)
omgp_add_catch_test(test_l3_header tests/unit/test_l3_header.cpp)
omgp_add_catch_test(test_l3_payload tests/unit/test_l3_payload.cpp)
target_link_libraries(test_l3_payload PRIVATE omgp_canon)
omgp_add_catch_test(test_l3_descriptor tests/unit/test_l3_descriptor.cpp)
target_link_libraries(test_l3_descriptor PRIVATE omgp_canon)
omgp_add_catch_test(test_l3_bp_slot_map tests/unit/test_l3_bp_slot_map.cpp)
omgp_add_catch_test(test_l3_roundtrip tests/property/test_l3_roundtrip.cpp)
omgp_add_catch_test(test_link_interfaces tests/unit/test_link_interfaces.cpp)
omgp_add_catch_test(test_link_types tests/unit/test_link_types.cpp)
omgp_add_catch_test(test_link_frame tests/unit/test_link_frame.cpp)
omgp_add_catch_test(test_canonical_frame tests/unit/test_canonical_frame.cpp)
target_link_libraries(test_canonical_frame PRIVATE omgp_canon)
omgp_add_catch_test(test_l3_helper_dispatch tests/unit/test_l3_helper_dispatch.cpp)
target_link_libraries(test_l3_helper_dispatch PRIVATE omgp_l3_helper)
omgp_add_catch_test(test_link_stuffing tests/property/test_link_stuffing.cpp)
omgp_add_catch_test(test_link_resync tests/property/test_link_resync.cpp)
omgp_add_catch_test(test_mock_wire tests/unit/test_mock_wire.cpp)
omgp_add_catch_test(test_mock_l3_node tests/unit/test_mock_l3_node.cpp)
omgp_add_catch_test(test_link_health tests/unit/test_link_health.cpp)
omgp_add_catch_test(test_link_busfault tests/unit/test_link_busfault.cpp)
omgp_add_catch_test(test_link_master tests/unit/test_link_master.cpp)
omgp_add_catch_test(test_link_responder tests/unit/test_link_responder.cpp)
omgp_add_catch_test(test_link_loop tests/unit/test_link_loop.cpp)
# core/ (spec 003). test_core_types is also core/'s mutation oracle: tools/mutate.cfg lists `core`
# in scope_dirs, and the harness fails closed on a scope dir with sources and no test_<dir>_*
# binary (tests/refimpl/test_tooling.py) — core/core_types.hpp is core/'s first source file.
omgp_add_catch_test(test_core_types tests/unit/test_core_types.cpp)
# CoreEngine's §8a pending-delivery rings and drain_callbacks() (spec 003 T014/T015).
omgp_add_catch_test(test_core_callback_queue tests/unit/test_core_callback_queue.cpp)
omgp_add_catch_test(test_core_discovery tests/unit/test_core_discovery.cpp)
# set_param()/get_param()'s enqueue side and R-10's request-id pool (spec 003 T026/T027).
omgp_add_catch_test(test_core_params tests/unit/test_core_params.cpp)
omgp_add_catch_test(test_core_scheduler tests/unit/test_core_scheduler.cpp)

# --- host-only tools ---
add_executable(crc_helper tools/crc_helper.cpp)
add_executable(l3_helper tools/l3_helper.cpp)
target_link_libraries(l3_helper PRIVATE omgp_l3_helper)
