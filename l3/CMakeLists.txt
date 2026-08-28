# omgp_l3 — L3 message + descriptor codecs. Embedded path (CLAUDE.md rule 5): the
# portable-subset constraints are compiler-enforced here, not just documented.
# Sources are listed explicitly.
add_library(omgp_l3 STATIC
  l3_status.cpp
  l3_header.cpp
  l3_payload.cpp
  l3_descriptor.cpp
)
target_include_directories(omgp_l3 PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/build/gen
)
target_compile_features(omgp_l3 PUBLIC cxx_std_17)
target_compile_options(omgp_l3 PRIVATE -fno-exceptions -fno-rtti)
