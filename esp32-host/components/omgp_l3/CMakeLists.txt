# ESP-IDF component wrapping the portable L3 codec library in ../../../l3 (repo root is
# three levels up). SRC_DIRS is used because idf_component_register(SRCS ...) does not
# glob. build/gen/ must already exist: codegen runs on the host (pipeline.sh stage_esp32
# calls stage_codegen first) because this image has no Jinja2.
set(OMGP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
idf_component_register(
  SRC_DIRS     "${OMGP_ROOT}/l3"
  INCLUDE_DIRS "${OMGP_ROOT}/l3" "${OMGP_ROOT}/build/gen" "${OMGP_ROOT}"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions -fno-rtti)
