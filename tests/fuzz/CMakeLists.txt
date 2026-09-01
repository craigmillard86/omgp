# libFuzzer targets — built only under the `fuzz` preset (clang, OMGP_FUZZ=ON). Each target
# is one decoder entry point under -fsanitize=fuzzer,address,undefined (spec 001 FR-026).
# tools/fuzz-smoke.sh builds these and runs them for the CI budget.
set(OMGP_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -O1)

function(omgp_add_fuzz_target name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ${ARGN})
  target_compile_options(${name} PRIVATE ${OMGP_FUZZ_FLAGS})
  target_link_options(${name} PRIVATE ${OMGP_FUZZ_FLAGS})
  # Binaries at build/fuzz/<name> (not under tests/fuzz/) — tools/fuzz-smoke.sh runs them there.
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endfunction()

omgp_add_fuzz_target(fuzz_header omgp_l3)
omgp_add_fuzz_target(fuzz_payload omgp_l3)
omgp_add_fuzz_target(fuzz_descriptor omgp_l3)
omgp_add_fuzz_target(fuzz_roundtrip omgp_canon)
omgp_add_fuzz_target(fuzz_frame omgp_link)
