# omgp_link — trunk L2 (link/). INTERFACE until T008 adds link_status.cpp and converts
# this to STATIC: CMake refuses `add_library(... STATIC)` with no sources, and link/
# holds only the header-only crc16.hpp at this point (specs/002-trunk-link-layer/tasks.md
# T001; contracts/tooling.md "CMake").
add_library(omgp_link INTERFACE)
target_include_directories(omgp_link INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/build/gen
)
target_compile_features(omgp_link INTERFACE cxx_std_17)
