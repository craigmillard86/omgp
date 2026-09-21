# omgp_core — portable host-core (core/). INTERFACE, not STATIC: core/ has no sources yet
# and CMake rejects a source-less STATIC library. specs/003-host-core-engine/tasks.md T015
# converts this target to STATIC when core_engine.cpp lands and attaches the
# -fno-exceptions -fno-rtti options CLAUDE.md rule 5 requires (compiler-enforced only from
# then on) — the same INTERFACE-then-STATIC path link/CMakeLists.txt took (002's T001/T008).
# core/'s dependency surface (specs/003-host-core-engine/research.md R-02, a stated
# divergence from CLAUDE.md's literal OMGPTransport wording; contracts/core-cpp.md):
# link::Master / link::HealthTracker / Clock via link/master.hpp, link/health.hpp,
# link/clock.hpp, link/link_types.hpp, plus l3/ and the generated protocol header — never
# link/responder.hpp, link/frame.hpp, link/byte_wire.hpp, sim/, cli/ or a platform header.
# The target_link_libraries(omgp_core ... omgp_link omgp_l3) that expresses that belongs to
# T015's STATIC conversion (plan.md "Project Structure"), not here: an INTERFACE library with
# no headers and no sources needs no link dependencies.
add_library(omgp_core INTERFACE)
target_include_directories(omgp_core INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/build/gen
)
target_compile_features(omgp_core INTERFACE cxx_std_17)
