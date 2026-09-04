#!/usr/bin/env bash
# OMGP pipeline — single definition used by CI, developers, and agents.
# Usage: ./pipeline.sh [stage...]   (default: all local stages)
# Stages: codegen quality build unit refimpl diffcheck scenarios selftest esp32 | fuzz (optional, clang)
set -euo pipefail
cd "$(dirname "$0")"
STAGES=("${@:-codegen quality build unit refimpl diffcheck scenarios selftest}")
[ $# -eq 0 ] && STAGES=(codegen quality build unit refimpl diffcheck scenarios selftest)
BIN=build/native
CXXFLAGS_BOOT="-std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Ibuild/gen"
# Counting heap guard for Catch2 tests (tests/support/heap_guard.hpp) — mirrors CMakeLists.txt.
WRAP_LDFLAGS="-Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=_Znwm -Wl,--wrap=_Znam"

# raise when tests are added; NEVER lower to get green (that change is itself T3)
# 133865 = 133870 executed (9 binaries; the seeded property tests dominate) minus 5 slack —
# T008/T009 follow-up, raised 2026-08-30 with test_link_types.cpp (was 133821)
# 197000 = 197100 executed (12 binaries) minus ~100 slack — T014-T017+T022, raised
# 2026-08-31 with test_link_frame/test_link_stuffing/test_link_resync.cpp (was 133865)
# 200580 = 200682 executed (16 binaries) minus ~100 slack — T023-T026 (canonical frame
# verbs, torture, mock_wire, health) landed since the last raise; T027 checkpoint,
# raised 2026-09-04 (was 197000)
UNIT_TEST_FLOOR=200580

stage_codegen() {
  # Constants + vectors header from the YAML, then prove the human-authored docs tables
  # still match it (the docs drift guard; spec 001 FR-001, SC-002).
  python3 tools/codegen.py --vectors tests/vectors
  python3 tools/codegen.py --check-docs
}

stage_quality() {
  # Code quality gates: formatting + static analysis. Runs on every merge.
  if command -v clang-format >/dev/null 2>&1; then
    find core link l3 sim cli transport tools tests -name '*.cpp' -o -name '*.hpp' 2>/dev/null \
      | xargs -r clang-format --dry-run --Werror
  else
    echo "quality: clang-format not present (skipped in this env)"
  fi
  if command -v clang-tidy >/dev/null 2>&1 && [ -f build/native/compile_commands.json ]; then
    find core link l3 -name '*.cpp' 2>/dev/null \
      | xargs -r clang-tidy -p build/native --warnings-as-errors='*'
  else
    echo "quality: clang-tidy skipped (needs compile_commands.json from cmake build)"
  fi
  # CLAUDE.md rules 1/4/5 for embedded-path code: no heap/exceptions/RTTI, no protocol
  # literals that the YAML defines, spec citations in l3/. Pure Python; runs on every path.
  python3 tools/check_embedded.py
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
    # Vendored Catch2 (third_party/catch2/VERSION), compiled once and reused while unchanged;
    # third-party warnings never fail the build, so -Werror is dropped for this object only.
    if [ ! -f "$BIN/catch2.o" ] || [ third_party/catch2/catch_amalgamated.cpp -nt "$BIN/catch2.o" ]; then
      g++ ${CXXFLAGS_BOOT/-Werror/} -Ithird_party/catch2 -c third_party/catch2/catch_amalgamated.cpp -o "$BIN/catch2.o"
    fi
    local support l3srcs linksrcs t name
    # canonical.cpp/l3_helper_dispatch.cpp: host-only, used by tests + l3_helper
    support="$(ls tests/support/*.cpp) tools/canonical.cpp tools/l3_helper_dispatch.cpp"
    l3srcs=$(ls l3/*.cpp 2>/dev/null || true)
    linksrcs=$(ls link/*.cpp 2>/dev/null || true)
    # One binary per tests/unit/test_*.cpp and tests/property/test_*.cpp. test_smoke keeps its
    # own main (linking it with Catch2's main would be a duplicate symbol).
    for t in tests/unit/test_*.cpp tests/property/test_*.cpp; do
      [ -f "$t" ] || continue
      name=$(basename "$t" .cpp)
      if [ "$name" = test_smoke ]; then
        g++ $CXXFLAGS_BOOT "$t" -o "$BIN/test_smoke"
      else
        g++ $CXXFLAGS_BOOT -I. -Il3 -Itools -Ithird_party/catch2 -Itests/support \
          "$t" $support $l3srcs $linksrcs "$BIN/catch2.o" $WRAP_LDFLAGS -o "$BIN/$name"
      fi
    done
    g++ $CXXFLAGS_BOOT tools/crc_helper.cpp -o "$BIN/crc_helper"
    g++ $CXXFLAGS_BOOT -I. -Il3 -Itools tools/l3_helper.cpp tools/l3_helper_dispatch.cpp \
      tools/canonical.cpp $l3srcs $linksrcs -o "$BIN/l3_helper"
  fi
}

stage_unit() {
  if command -v ctest >/dev/null 2>&1 && [ -f build/native/CTestTestfile.cmake ]; then
    # The ctest path discovers tests from CMakeLists.txt, not the filesystem
    # (unlike the bootstrap glob below), so a tests/unit|property/test_*.cpp
    # file whose registration is missing or was dropped runs on no path at
    # all while this stage still reports green — the check-count floor alone
    # can't see it when the file's checks are smaller than its slack
    # (red-team PR #128, findings 1 + 2). Fail loudly instead.
    local registered f name want orphaned
    registered=$(grep -oE 'add_test\(\[=\[[A-Za-z0-9_]+\]=\]' build/native/CTestTestfile.cmake \
                   | sed -E 's/^add_test\(\[=\[//; s/\]=\]$//')
    orphaned=()
    for f in tests/unit/test_*.cpp tests/property/test_*.cpp; do
      [ -f "$f" ] || continue
      name=$(basename "$f" .cpp)
      want="$name"
      [ "$name" = test_smoke ] && want=smoke   # test_smoke keeps its own main; registered as "smoke"
      printf '%s\n' "$registered" | grep -qx "$want" || orphaned+=("$f")
    done
    if [ "${#orphaned[@]}" -gt 0 ]; then
      echo "unit: test source file(s) not registered as a ctest test: ${orphaned[*]}" >&2
      return 1
    fi

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
    # Every test binary runs; its output is always printed (even on failure) and the
    # EXECUTED: lines are summed across binaries for the floor.
    local total=0 bin out rc n
    for bin in "$BIN"/test_*; do
      [ -x "$bin" ] || continue
      set +e
      out=$("$bin")
      rc=$?
      set -e
      echo "$out"
      if [ "$rc" -ne 0 ]; then
        echo "unit: $(basename "$bin") failed (exit $rc)" >&2
        return "$rc"
      fi
      n=$(printf '%s\n' "$out" | grep -o 'EXECUTED: [0-9]\+' | grep -o '[0-9]\+' | tail -1) || true
      total=$((total + ${n:-0}))
    done
    echo "unit: executed $total check(s) (bootstrap path)"
    if [ "$total" -lt "$UNIT_TEST_FLOOR" ]; then
      echo "unit: executed check count ($total) below floor ($UNIT_TEST_FLOOR) - test filter may be broken" >&2
      return 1
    fi
  fi
}

stage_refimpl() {
  python3 tools/refimpl/omgp_crc.py
  python3 tools/refimpl/genvectors.py --check   # committed vectors byte-identical to the generator (rule 9; red-team finding 3 on PR #107)
  python3 -m pytest -q tools/refimpl   # reference implementation + tool tests (jinja2/pytest: tools/requirements.txt)
}
stage_diffcheck() { python3 tools/diffcheck.py; }
stage_scenarios() {
  if [ -x "$BIN/scenario_runner" ]; then "$BIN/scenario_runner" tests/scenarios/
  else python3 tools/scenario_lint.py; fi   # lint-only until F4 delivers the runner
}
stage_selftest() {
  # Regression tests for pipeline.sh's own gates. These existed
  # (test_pipeline_floor.sh, test_pipeline_link_bootstrap.sh) but were
  # executed by nothing — not CMakeLists.txt, not the bootstrap glob, not any
  # workflow (red-team PR #128 finding 4). Each script mutates pipeline.sh or
  # CMakeLists.txt and rebuilds to prove a gate fires, then restores itself;
  # capture failures rather than let `set -e` abort before the build/native
  # cleanup below, since a mid-test rebuild can leave it desynced from the
  # restored files (stale object timestamps vs. the file cmake now sees).
  # Invoked via `bash` rather than run directly: the executable bit isn't
  # guaranteed to survive every checkout (test_pipeline_link_bootstrap.sh is
  # tracked as 100644), and this way it doesn't need to be.
  local rc=0
  bash tests/unit/test_pipeline_floor.sh || rc=$?
  bash tests/unit/test_pipeline_link_bootstrap.sh || rc=$?
  bash tests/unit/test_pipeline_registration.sh || rc=$?
  rm -rf build/native
  return "$rc"
}
stage_fuzz() {
  # Optional (not in the default list): libFuzzer smoke over every decoder, clang only.
  # CI deep-verify runs tools/fuzz-smoke.sh 600 directly; FUZZ_SECONDS=… for a longer soak.
  tools/fuzz-smoke.sh "${FUZZ_SECONDS:-60}"
}
stage_esp32() {
  # Codegen runs on the host: the IDF image has no Jinja2, and build/gen/ is inside the mount.
  stage_codegen
  docker run --rm -v "$PWD":/w -w /w/esp32-host "espressif/idf:$(cat IDF_VERSION)" \
    bash -c "idf.py set-target esp32s3 && idf.py build"
}

for s in ${STAGES[@]}; do
  echo "==> stage: $s"
  "stage_$s"
done
echo "==> pipeline green"
