cmake_minimum_required(VERSION 3.16)
# Protocol constants are generated on the host (pipeline.sh stage_esp32 runs codegen
# before this container starts; the espressif/idf image has no Jinja2). Fail early and
# say so rather than producing a cryptic missing-header error deep in the build.
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/../build/gen/omgp_protocol.h")
  message(FATAL_ERROR "build/gen/omgp_protocol.h missing: run ./pipeline.sh codegen first (codegen runs on the host, not in the IDF container)")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(omgp-host)
