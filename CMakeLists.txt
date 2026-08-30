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

# --- test framework: vendored Catch2 v3 (third_party/catch2/VERSION records tag + sha256) ---
add_library(catch2_amalgamated STATIC third_party/catch2/catch_amalgamated.cpp)
target_include_directories(catch2_amalgamated PUBLIC third_party/catch2)
target_compile_options(catch2_amalgamated PRIVATE -Wno-error) # third-party warnings never fail the build

# tests/support: linked into every Catch2 test, never a test itself. OBJECT library so the
# listener's static registrar and the --wrap symbols are always pulled into the link.
add_library(omgp_test_support OBJECT tests/support/catch_listener.cpp tests/support/heap_guard.cpp)
target_include_directories(omgp_test_support PUBLIC tests/support)
target_link_libraries(omgp_test_support PUBLIC catch2_amalgamated)

# Counting heap guard (tests/support/heap_guard.hpp): wrap the allocators our objects call.
set(OMGP_WRAP_LDFLAGS -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc
                      -Wl,--wrap=_Znwm -Wl,--wrap=_Znam)

function(omgp_add_catch_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE omgp_l3 omgp_link omgp_test_support catch2_amalgamated)
  target_link_options(${name} PRIVATE ${OMGP_WRAP_LDFLAGS})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# --- host-only canonical-text codec (C++ side of the differential test) ---
add_library(omgp_canon STATIC tools/canonical.cpp)
target_include_directories(omgp_canon PUBLIC tools)
target_link_libraries(omgp_canon PUBLIC omgp_l3 omgp_link)

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
omgp_add_catch_test(test_l3_roundtrip tests/property/test_l3_roundtrip.cpp)
omgp_add_catch_test(test_link_interfaces tests/unit/test_link_interfaces.cpp)

# --- host-only tools ---
add_executable(crc_helper tools/crc_helper.cpp)
add_executable(l3_helper tools/l3_helper.cpp)
target_link_libraries(l3_helper PRIVATE omgp_canon omgp_link)
