#!/usr/bin/env bash
# OMGP pipeline — single definition used by CI, developers, and agents.
# Usage: ./pipeline.sh [stage...]   (default: all local stages)
# Stages: codegen quality build unit refimpl diffcheck scenarios esp32 | fuzz (optional, clang)
set -euo pipefail
cd "$(dirname "$0")"
STAGES=("${@:-codegen quality build unit refimpl diffcheck scenarios}")
[ $# -eq 0 ] && STAGES=(codegen quality build unit refimpl diffcheck scenarios)
BIN=build/native
CXXFLAGS_BOOT="-std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Ibuild/gen"
# Counting heap guard for Catch2 tests (tests/support/heap_guard.hpp) — mirrors CMakeLists.txt.
WRAP_LDFLAGS="-Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=_Znwm -Wl,--wrap=_Znam"

# raise when tests are added; NEVER lower to get green (that change is itself T3)
# 133865 = 133870 executed (9 binaries; the seeded property tests dominate) minus 5 slack —
# T008/T009 follow-up, raised 2026-08-30 with test_link_types.cpp (was 133821)
# 197000 = 197100 executed (12 binaries) minus ~100 slack — T014-T017+T022, raised
# 2026-08-31 with test_link_frame/test_link_stuffing/test_link_resync.cpp (was 133865)
# 200580 = 200682 executed (16 binaries) minus ~102 slack — T023-T026 added
# test_canonical_frame/test_l3_helper_dispatch/test_mock_wire/test_link_health.cpp;
# raised 2026-09-05 (T027/#45, was 197000). NEVER lowered.
# 509360 = 509460 executed (17 binaries) minus 100 slack AT THE RAISING COMMIT — T029/T031
# added test_link_master.cpp (then 308753 checks: its microsecond-cadence timing loops REQUIRE
# per poll); raised 2026-09-06 (#137 review, MEDIUM: at 200580 the gate could not notice this
# binary being dropped — 236588 executed at 40355cf was 36008 above it). The suite grows and
# the floor is not chased upward, so the CURRENT total is the `unit: executed N check(s)`
# line of the latest run, not this comment (it went stale twice: #137 reviews @050f397 and
# @c0bf71c). What the floor protects is the invariant, restated with the figures measured
# at 1a55116 (515373 = test_link_master 310744 + the other 16 binaries 204629): the other
# 16 together stay BELOW the floor, so dropping test_link_master fires the gate —
# demonstrated by that arithmetic, assuming those 16 have not grown by 304731 (slack over
# the floor is 6013; over the 16-binary sum it is 304731). Was 200580.
UNIT_TEST_FLOOR=509360

stage_codegen() {
  # Constants + vectors header from the YAML, then prove the human-authored docs tables
  # still match it (the docs drift guard; spec 001 FR-001, SC-002).
  python3 tools/codegen.py --vectors tests/vectors
  python3 tools/codegen.py --check-docs
}

stage_quality() {
  # Code quality gates: formatting + embedded-path rules. A CI gate since #135 (ci.yml's
  # native job names this stage; before that nothing in CI ran it).
  #
  # The formatter is PINNED in tools/requirements.txt (clang-format==N, a pip wheel) because
  # releases format differently: an unpinned gate would red every PR the day the runner image
  # moved. The stage checks `clang-format --version` against that pin. On a CI runner
  # (GITHUB_ACTIONS/CI set) a missing or mismatched formatter FAILS the stage — the silent
  # `command -v` skip this replaced is exactly the hole #135 closed. Locally it is a disclosed
  # skip with the fix named, and check_embedded still runs. OMGP_CLANG_FORMAT overrides the
  # binary (tests use it to simulate absence/mismatch, as OMGP_FUZZ_CXX does for fuzz-smoke).
  local want have cf="${OMGP_CLANG_FORMAT:-clang-format}"
  want=$(sed -n 's/^clang-format==\([0-9][0-9.]*\).*/\1/p' tools/requirements.txt)
  [ -n "$want" ] || { echo "quality: tools/requirements.txt does not pin clang-format==N" >&2; return 1; }
  have=$({ "$cf" --version 2>/dev/null || true; } | sed -n 's/.*clang-format version \([0-9][0-9.]*\).*/\1/p')   # `|| true`: an absent binary is a finding, not a crash (set -eo pipefail)
  if [ "$have" = "$want" ]; then
    local files
    files=$(find core link l3 sim cli transport tools tests -name '*.cpp' -o -name '*.hpp' 2>/dev/null)
    echo "$files" | xargs -r "$cf" --dry-run --Werror
    echo "quality: clang-format $want clean ($(echo "$files" | grep -c .) files)"
  elif [ -n "${GITHUB_ACTIONS:-}${CI:-}" ]; then
    echo "quality: clang-format $want is required on CI, found '${have:-none}' — pip install -r tools/requirements.txt" >&2
    return 1
  else
    echo "quality: clang-format $want not on PATH (found '${have:-none}') — formatting NOT checked; pip install -r tools/requirements.txt"
  fi
  # clang-tidy is opt-in (OMGP_CLANG_TIDY=1): it is not pinned, and it needs
  # build/native/compile_commands.json, which in CI exists only on a warm cache — an automatic
  # run would make the gate's verdict depend on cache state and on the runner's analyser
  # version. Pinning it is a separate decision (#135 PR body); nothing in CI ran it before.
  if [ -n "${OMGP_CLANG_TIDY:-}" ]; then
    { command -v clang-tidy >/dev/null 2>&1 && [ -f build/native/compile_commands.json ]; } \
      || { echo "quality: OMGP_CLANG_TIDY is set but clang-tidy or build/native/compile_commands.json is missing" >&2; return 1; }
    find core link l3 -name '*.cpp' 2>/dev/null \
      | xargs -r clang-tidy -p build/native --warnings-as-errors='*'
  else
    echo "quality: clang-tidy not run (opt-in: OMGP_CLANG_TIDY=1 with clang-tidy on PATH and a cmake build tree; unpinned)"
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
  python3 -m pytest -q -rs tools/refimpl   # reference implementation + tool tests (jinja2/pytest: tools/requirements.txt); -rs names every skip, so a CI log says WHICH blind spot a skip is (#141 review: "3 skipped" was unreadable)
}
stage_diffcheck() { python3 tools/diffcheck.py; }
stage_scenarios() {
  if [ -x "$BIN/scenario_runner" ]; then "$BIN/scenario_runner" tests/scenarios/
  else python3 tools/scenario_lint.py; fi   # lint-only until F4 delivers the runner
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
