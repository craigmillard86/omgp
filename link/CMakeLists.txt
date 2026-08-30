# omgp_link — trunk L2 (link/). INTERFACE until T008 adds link_status.cpp and converts
# this to STATIC: CMake refuses `add_library(... STATIC)` with no sources, and link/
# holds only the header-only crc16.hpp at this point (specs/002-trunk-link-layer/tasks.md
# T001; contracts/tooling.md "CMake").
add_library(omgp_link INTERFACE)
# omgp_link must never target_link_libraries(... omgp_l3): L2 is opaque to L3 (CLAUDE.md
# "Architecture invariants"; spec 002 FR-013 — the engine hands payload up as opaque bytes
# and must not depend on L3 content). Targets that legitimately need both (test binaries,
# omgp_canon, l3_helper) link omgp_l3 and omgp_link independently at the root CMakeLists.txt,
# not through this target. tools/refimpl/test_check_embedded.py::test_link_never_includes_l3
# is the companion source-level guard (specs/002-trunk-link-layer/tasks.md T005).
target_include_directories(omgp_link INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/build/gen
)
target_compile_features(omgp_link INTERFACE cxx_std_17)
