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

# raise when tests are added; NEVER lower to get green (that change is itself T3)
UNIT_TEST_FLOOR=7

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
    echo "bootstrap build: same sources+sanitizers as CMake preset; ctest/coverage paths not exercised"
    mkdir -p "$BIN"
    g++ $CXXFLAGS_BOOT tests/unit/test_smoke.cpp -o "$BIN/test_smoke"
    g++ $CXXFLAGS_BOOT tools/crc_helper.cpp     -o "$BIN/crc_helper"
  fi
}

stage_unit() {
  if command -v ctest >/dev/null 2>&1 && [ -f build/native/CTestTestfile.cmake ]; then
    ctest --preset native --output-on-failure
    # ctest only prints test-binary stdout inline on failure, so on a green
    # run the "EXECUTED: <n>" line lives in ctest's per-test log instead.
    local log="build/native/Testing/Temporary/LastTest.log"
    local n=0
    if [ -f "$log" ]; then
      n=$(grep -o 'EXECUTED: [0-9]\+' "$log" | awk -F': ' '{s+=$2} END{print s+0}') || true
    fi
    n=${n:-0}
    echo "unit: executed $n check(s) (ctest path)"
    if [ "$n" -lt "$UNIT_TEST_FLOOR" ]; then
      echo "unit: executed check count ($n) below floor ($UNIT_TEST_FLOOR) - test filter may be broken" >&2
      return 1
    fi
  else
    local out
    out=$("$BIN/test_smoke")
    echo "$out"
    local n
    n=$(printf '%s\n' "$out" | grep -o 'EXECUTED: [0-9]\+' | grep -o '[0-9]\+') || true
    n=${n:-0}
    echo "unit: executed $n check(s) (bootstrap path)"
    if [ "$n" -lt "$UNIT_TEST_FLOOR" ]; then
      echo "unit: executed check count ($n) below floor ($UNIT_TEST_FLOOR) - test filter may be broken" >&2
      return 1
    fi
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
