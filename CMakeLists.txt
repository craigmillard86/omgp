cmake_minimum_required(VERSION 3.22)
project(omgp CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
add_executable(test_smoke tests/unit/test_smoke.cpp)
add_test(NAME smoke COMMAND test_smoke)
add_executable(crc_helper tools/crc_helper.cpp)
# F1 onward: add omgp_protocol / omgp_link / omgp_core libraries here,
# Catch2 via FetchContent (or vendored for offline builds), fuzz targets.
