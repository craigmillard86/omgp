# ESP-IDF component wrapping the portable trunk L2 library in ../../../link (repo root is
# three levels up). SRC_DIRS is used because idf_component_register(SRCS ...) does not
# glob. build/gen/ must already exist: codegen runs on the host (pipeline.sh stage_esp32
# calls stage_codegen first) because this image has no Jinja2. link/ is header-only today
# (only crc16.hpp, specs/002-trunk-link-layer tasks.md T001); ESP-IDF registers a
# source-less component as INTERFACE automatically, same reasoning link/CMakeLists.txt
# documents for the native STATIC-vs-INTERFACE choice.
set(OMGP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
idf_component_register(
  SRC_DIRS     "${OMGP_ROOT}/link"
  INCLUDE_DIRS "${OMGP_ROOT}/link" "${OMGP_ROOT}/build/gen" "${OMGP_ROOT}"
)
# No source files yet (T008 adds link_status.cpp) so ESP-IDF registers COMPONENT_LIB as an
# INTERFACE library; target_compile_options only accepts PRIVATE on a library that compiles
# something. Guard until the component gains sources and becomes STATIC.
get_target_property(omgp_link_type ${COMPONENT_LIB} TYPE)
if(NOT omgp_link_type STREQUAL "INTERFACE_LIBRARY")
  target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions -fno-rtti)
endif()
