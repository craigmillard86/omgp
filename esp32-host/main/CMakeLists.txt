# omgp_core has no smoke file of its own yet, so it appears only in REQUIRES: that is what puts
# the component in the build graph and gets core/ compiled under its -fno-exceptions -fno-rtti
# (T003, #674). Compiled, not linked — core_smoke.cpp is T045 (#716); see the component's own
# CMakeLists.txt and tools/refimpl/test_esp32_link_smoke.py for why the distinction matters.
idf_component_register(SRCS "main.c" "l3_smoke.cpp" "link_smoke.cpp" INCLUDE_DIRS "." REQUIRES omgp_l3 omgp_link omgp_core)
