#!/usr/bin/env bash
# OMGP pipeline — single definition used by CI, developers, and agents.
# Usage: ./pipeline.sh [stage...]   (default: all local stages)
# Stages: codegen quality build unit refimpl diffcheck scenarios esp32
set -euo pipefail
cd "$(dirname "$0")"
STAGES=("${@:-codegen quality build unit refimpl diffcheck scenarios}")
[ $# -eq 0 ] && STAGES=(codegen quality build unit refimpl diffcheck scenarios)
BIN=build/native
CXXFLAGS_BOOT="-std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Ibuild/gen"

stage_codegen()  { python3 tools/codegen.py; }

stage_quality() {
  # Code quality gates: formatting + static analysis. Runs on every merge.
  if command -v clang-format >/dev/null 2>&1; then
    find core link sim cli transport tools -name '*.cpp' -o -name '*.hpp' 2>/dev/null \
      | xargs -r clang-format --dry-run --Werror
  else
    echo "quality: clang-format not present (skipped in this env)"
  fi
  if command -v clang-tidy >/dev/null 2>&1 && [ -f build/native/compile_commands.json ]; then
    find core link -name '*.cpp' 2>/dev/null \
      | xargs -r clang-tidy -p build/native --warnings-as-errors='*'
  else
    echo "quality: clang-tidy skipped (needs compile_commands.json from cmake build)"
  fi
}

stage_build() {
  if command -v cmake >/dev/null 2>&1; then
    cmake --preset native && cmake --build --preset native
  else
    # Bootstrap mode: constrained environments (sandboxes, minimal agent hosts).
    # Same sources, same sanitizers; CMake presets remain the canonical build.
    echo "build: cmake not found -> bootstrap g++ build (sanitizers on)"
    mkdir -p "$BIN"
    g++ $CXXFLAGS_BOOT tests/unit/test_smoke.cpp -o "$BIN/test_smoke"
    g++ $CXXFLAGS_BOOT tools/crc_helper.cpp     -o "$BIN/crc_helper"
  fi
}

stage_unit() {
  if command -v ctest >/dev/null 2>&1 && [ -f build/native/CTestTestfile.cmake ]; then
    ctest --preset native --output-on-failure
  else
    "$BIN/test_smoke"
  fi
}

stage_refimpl()   { python3 tools/refimpl/omgp_crc.py; }
stage_diffcheck() { python3 tools/diffcheck.py; }
stage_scenarios() {
  if [ -x "$BIN/scenario_runner" ]; then "$BIN/scenario_runner" tests/scenarios/
  else python3 tools/scenario_lint.py; fi   # lint-only until F4 delivers the runner
}
stage_esp32() {
  docker run --rm -v "$PWD":/w -w /w/esp32-host "espressif/idf:$(cat IDF_VERSION)" \
    bash -c "idf.py set-target esp32s3 && idf.py build"
}

for s in ${STAGES[@]}; do
  echo "==> stage: $s"
  "stage_$s"
done
echo "==> pipeline green"
