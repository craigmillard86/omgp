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

# UNIT_TEST_FLOOR's slack (~100 checks) is large enough to hide an entire
# small binary's test cases being gutted while the aggregate count stays
# green (red-team PR #128, second review round: deleting 96 of
# test_canonical_frame's 97 checks still cleared the floor by 6). Every
# binary's count is deterministic (property tests are fixed-seed,
# kSeed = 0xB0071E), so each floor below is exact, not slack. Same
# raise-only convention as UNIT_TEST_FLOOR: bump the entries you touch when
# you add checks, never lower one to get green. A binary with no entry here
# (just added, not yet wired in) is not floor-checked.
declare -A UNIT_PER_BINARY_FLOOR=(
  [test_smoke]=7
  [test_l3_types]=40
  [test_gen_constants]=77
  [test_l3_header]=29
  [test_l3_payload]=356
  [test_l3_descriptor]=299
  [test_l3_roundtrip]=133012
  [test_link_interfaces]=6
  [test_link_types]=85
  [test_link_frame]=731
  [test_canonical_frame]=97
  [test_l3_helper_dispatch]=20
  [test_link_stuffing]=47466
  [test_link_resync]=18039
  [test_mock_wire]=114
  [test_link_health]=304
)

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
    #
    # Match by *source path*, not by the registered ctest name: a name check
    # alone is blind to a registration whose name is right but whose source
    # argument was repointed at a different test_*.cpp (e.g. a copy-pasted
    # omgp_add_catch_test line) — the named ctest entry still exists and even
    # reports MORE checks (the duplicated file's), so neither the name check
    # nor the floor sees the orphaned file (red-team PR #128, second review
    # round, [HIGH]). CMakeLists.txt is the only place the name<->source
    # binding is made, so read the source argument directly from it instead
    # of ctest's derived name list.
    local declared f orphaned
    declared=$(grep -oE '(omgp_add_catch_test\([A-Za-z0-9_]+ |add_executable\(test_smoke )tests/(unit|property)/test_[A-Za-z0-9_]+\.cpp' CMakeLists.txt \
                 | grep -oE 'tests/(unit|property)/test_[A-Za-z0-9_]+\.cpp')
    orphaned=()
    for f in tests/unit/test_*.cpp tests/property/test_*.cpp; do
      [ -f "$f" ] || continue
      printf '%s\n' "$declared" | grep -qxF "$f" || orphaned+=("$f")
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
    # The aggregate floor's slack can hide one binary's checks being gutted
    # (comment on UNIT_PER_BINARY_FLOOR above); check each binary against its
    # own exact floor too.
    #
    # Pair each "Test: <name>" line with the "EXECUTED:" line that follows it
    # via awk, not a flat `paste - -` of two separate greps: paste assumes
    # strict alternation, so one binary logging a "Test:" line with no
    # matching "EXECUTED:" (a future non-Catch2 binary, a crash before the
    # counter prints) would silently shift every later pair's name<->count
    # binding instead of just that one binary going unmatched (review PR #128
    # @ eeea8ca, [LOW] pipeline.sh:169). awk resets on each "Test:" line, so a
    # missing "EXECUTED:" only drops that one binary's pairing.
    local low=() bname bcount bfloor
    local -A seen=()
    while read -r bname bcount; do
      [ "$bname" = smoke ] && bname=test_smoke   # ctest registers it as "smoke"; table/file key is test_smoke
      seen[$bname]=1
      bfloor=${UNIT_PER_BINARY_FLOOR[$bname]:-0}
      [ "${bcount:-0}" -lt "$bfloor" ] && low+=("$bname:$bcount<$bfloor")
    done < <(awk '
               /^[0-9]+\/[0-9]+ Test: [A-Za-z0-9_]+$/ { name=$NF; next }
               /^EXECUTED: [0-9]+$/ { if (name != "") { print name, $2; name="" } }
             ' "$log")
    if [ "${#low[@]}" -gt 0 ]; then
      echo "unit: per-binary check count below floor: ${low[*]}" >&2
      return 1
    fi
    # The comparison above is only reachable for binaries the log actually
    # names. A binary that never ran at all — `set_tests_properties(...
    # PROPERTIES DISABLED TRUE)`, or a CMakePresets.json test filter excluding
    # it — leaves no "Test:"/"EXECUTED:" pair, so its floor entry is silently
    # never compared and the aggregate floor's slack absorbs the loss (review
    # PR #128 @ eeea8ca, [MEDIUM]: "the per-binary floor only fires for
    # binaries that actually ran"). Assert every floor-tracked name was seen.
    local missing=() key
    for key in "${!UNIT_PER_BINARY_FLOOR[@]}"; do
      [ -n "${seen[$key]:-}" ] || missing+=("$key")
    done
    if [ "${#missing[@]}" -gt 0 ]; then
      echo "unit: per-binary floor entries never ran (disabled/excluded?): ${missing[*]}" >&2
      return 1
    fi
  else
    # Every test binary runs; its output is always printed (even on failure) and the
    # EXECUTED: lines are summed across binaries for the floor.
    local total=0 bin out rc n bname bfloor
    local -A seen=()
    for bin in "$BIN"/test_*; do
      [ -x "$bin" ] || continue
      bname=$(basename "$bin")
      seen[$bname]=1
      set +e
      out=$("$bin")
      rc=$?
      set -e
      echo "$out"
      if [ "$rc" -ne 0 ]; then
        echo "unit: $bname failed (exit $rc)" >&2
        return "$rc"
      fi
      n=$(printf '%s\n' "$out" | grep -o 'EXECUTED: [0-9]\+' | grep -o '[0-9]\+' | tail -1) || true
      bfloor=${UNIT_PER_BINARY_FLOOR[$bname]:-0}
      if [ "${n:-0}" -lt "$bfloor" ]; then
        echo "unit: $bname executed ${n:-0} check(s), below per-binary floor ($bfloor)" >&2
        return 1
      fi
      total=$((total + ${n:-0}))
    done
    # Same completeness gap as the ctest branch above: a floor-tracked binary
    # that was never built at all (e.g. dropped from the tests/*/test_*.cpp
    # glob some other way) would otherwise just be skipped by this loop with
    # no comparison ever made against its floor.
    local missing=() key
    for key in "${!UNIT_PER_BINARY_FLOOR[@]}"; do
      [ -n "${seen[$key]:-}" ] || missing+=("$key")
    done
    if [ "${#missing[@]}" -gt 0 ]; then
      echo "unit: per-binary floor entries never ran (not built?): ${missing[*]}" >&2
      return 1
    fi
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
  # workflow (red-team PR #128 finding 4). Each script rewrites pipeline.sh
  # or CMakeLists.txt in place and rebuilds to prove a gate fires, then
  # restores itself on its own EXIT trap.
  #
  # Run them against a throwaway copy of the tree, not the real working
  # directory: while a script's mutation is live, a concurrent `git add -A`
  # / `git commit -a` — both granted to the same agent workflows
  # (agent-dispatch.yml, review-fix.yml, ci-failure-router.yml) that also
  # invoke this stage — could commit the poisoned file (e.g. a temporarily
  # impossible UNIT_TEST_FLOOR), and an unconditional `rm -rf build/native`
  # afterward previously deleted the real build tree out from under every
  # documented follow-up command (`ctest --preset native`,
  # `./build/native/scenario_runner ...`) and let stage_scenarios' fallback
  # degrade to lint-only, silently, on the next run (red-team PR #128,
  # second review round, both [MEDIUM]). A copy means nothing tracked is
  # ever touched in the real tree, so neither hazard exists.
  #
  # build/ is deliberately NOT copied along: CMakeCache.txt records the
  # absolute source/build directory path it was configured with, so a copy
  # under a different path makes every cmake invocation fail outright
  # ("current CMakeCache.txt directory ... is different than ..."). Each
  # script's rebuild is a clean one as a result — the correctness this stage
  # exists for is worth that, and it only runs once per default pipeline
  # invocation, not per edit-test cycle.
  local scratch rc=0
  scratch=$(mktemp -d)
  trap 'rm -rf "$scratch"' RETURN
  if command -v rsync >/dev/null 2>&1; then
    rsync -a --exclude=.git --exclude=build ./ "$scratch"/
  else
    cp -a . "$scratch"/
    rm -rf "$scratch/.git" "$scratch/build"
  fi
  # Invoked via `bash` rather than run directly: the executable bit isn't
  # guaranteed to survive every checkout (test_pipeline_link_bootstrap.sh is
  # tracked as 100644), and this way it doesn't need to be. Each script's
  # own `cd "$(dirname "$0")/../.."` resolves against $scratch since we
  # invoke it by its path there.
  #
  # A script SKIPs when its scenario can't fire on this host
  # (test_pipeline_link_bootstrap.sh when cmake is reachable via a second
  # PATH entry; test_pipeline_registration.sh / test_pipeline_binary_presence.sh
  # on a cmake-less host) and exits 0 exactly like a real PASS, so the
  # stage's own exit code can't show that fewer gates were actually
  # exercised this run (review PR #128 @ eeea8ca, [LOW] pipeline.sh:258-260).
  # Track and echo each script's outcome explicitly instead of only the
  # aggregate exit code.
  local script status=()
  for script in test_pipeline_floor test_pipeline_link_bootstrap \
                test_pipeline_registration test_pipeline_binary_presence; do
    local out src
    set +e
    out=$(bash "$scratch/tests/unit/$script.sh" 2>&1)
    src=$?
    set -e
    echo "$out"
    if [ "$src" -ne 0 ]; then
      status+=("$script:FAIL"); rc=$src
    elif printf '%s\n' "$out" | grep -q ': SKIP'; then
      status+=("$script:SKIP")
    else
      status+=("$script:PASS")
    fi
  done
  echo "selftest: ${status[*]}"
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
