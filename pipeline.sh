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

# The unit test SOURCE set, one line per file: every test_*.cpp under tests/unit and
# tests/property, at any depth, sorted — symlinks not followed (find -P), and refused by name
# in unit_sources_plain() below. Shared by the bootstrap build and the bootstrap unit
# walk (and mirrored by tools/check_test_set.py) so all three agree on what "every source"
# means; a flat glob would leave tests/unit/<sub>/test_x.cpp silently unchecked (#172 red team
# @ceab86f, finding 4). find's -name matches the file name only, so sorting is by full path.
# Only the directories that exist are walked: find exits 1 for a missing one, which under
# pipefail made `srcs=$(unit_sources)` a silent `set -e` exit of the stage (red team
# @2f40596, lead c — met as a real failure in the fake trees, which have no tests/property).
unit_sources() {
  local d dirs=()
  for d in tests/unit tests/property; do [ -d "$d" ] && dirs+=("$d"); done
  [ "${#dirs[@]}" -gt 0 ] || return 0
  find "${dirs[@]}" -name 'test_*.cpp' | LC_ALL=C sort
}
# The bootstrap path has ONE binary per basename by construction ($BIN/$(basename $t .cpp)),
# so two sources sharing a basename would clobber each other's build and let the survivor
# vouch for both — twice into the floor sum (#172 red team @3dde163, finding 1). Refused by
# name before the bootstrap build and before the bootstrap unit walk. The cmake path is
# unaffected: targets are named in CMakeLists, and the tool matches by object, not basename.
unit_sources_unique() {
  local dup rc=0
  for dup in $(unit_sources | xargs -rn1 basename | LC_ALL=C sort | uniq -d); do
    echo "unit: $(unit_sources | grep "/$dup\$" | tr '\n' ' ')— same basename $dup: the bootstrap path builds one binary per basename, so these would share (and clobber) one" >&2
    rc=1
  done
  return "$rc"
}
# No symlink under the test source directories, at any depth, directory or file (red team
# @8b0e4f4 finding 2): `find` without -L does not descend a symlinked directory, so a source
# behind one was outside the set and never named. The set is plain files and directories
# only; a link is refused by name (the same rule as tools/check_test_set.py's walk()).
unit_sources_plain() {
  local d dirs=() l rc=0
  for d in tests/unit tests/property; do [ -d "$d" ] && dirs+=("$d"); done
  [ "${#dirs[@]}" -gt 0 ] || return 0
  while IFS= read -r l; do
    [ -n "$l" ] || continue
    echo "unit: $l: a symlink (-> $(readlink "$l")) under the test source directories: the source set is plain files and directories only, so whatever the link reaches is outside the set — replace the link with the files themselves" >&2
    rc=1
  done < <(find "${dirs[@]}" -type l | LC_ALL=C sort)
  return "$rc"
}

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
# The floor is the COUNT gate. The SET gate — every tests/{unit,property}/test_*.cpp compiled,
# registered and executed, by name — is tools/check_test_set.py in stage_unit (#133).
UNIT_TEST_FLOOR=509360

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
    # One binary per unit_sources() entry, named by basename — so basenames must be unique.
    # test_smoke keeps its own main (linking it with Catch2's main would be a duplicate symbol).
    unit_sources_plain || return 1
    unit_sources_unique || return 1
    while IFS= read -r t; do
      name=$(basename "$t" .cpp)
      if [ "$name" = test_smoke ]; then
        g++ $CXXFLAGS_BOOT "$t" -o "$BIN/test_smoke"
      else
        g++ $CXXFLAGS_BOOT -I. -Il3 -Itools -Ithird_party/catch2 -Itests/support \
          "$t" $support $l3srcs $linksrcs "$BIN/catch2.o" $WRAP_LDFLAGS -o "$BIN/$name"
      fi
    done < <(unit_sources)
    g++ $CXXFLAGS_BOOT tools/crc_helper.cpp -o "$BIN/crc_helper"
    g++ $CXXFLAGS_BOOT -I. -Il3 -Itools tools/l3_helper.cpp tools/l3_helper_dispatch.cpp \
      tools/canonical.cpp $l3srcs $linksrcs -o "$BIN/l3_helper"
  fi
}

stage_unit() {
  if command -v ctest >/dev/null 2>&1 && [ -f build/native/CTestTestfile.cmake ]; then
    # Two paths, one predicate each. This one runs whatever cmake registered (ctest sees
    # add_test lines only, so check_test_set.py has to prove every source is in that set);
    # the bootstrap path below runs one binary per SOURCE, which its own stage_build made
    # one-for-one by construction. A cmake build tree with no CTestTestfile takes the
    # bootstrap path and fails by name at the first source with no $BIN binary.
    # The JUnit record is this run's execution evidence (#172 red team: LastTest.log
    # interleaves the tests' stdout with ctest's framing, so a test could forge it; the
    # record's framing is XML the tests cannot write). Deleted first so a stale record can
    # never stand in for this run's. --test-output-size-passed: ctest keeps only the first
    # 1024 bytes of a PASSING test's stdout in the record by default, and EXECUTED: is the
    # LAST line the listener prints, so a chatty green binary would lose its evidence
    # (red team @3880d35). Raised to 10 MB per test; the tool names a truncated record as
    # such. (`--test-output-truncation head` is accepted and ignored by CMake 3.22 — measured.)
    # Pre-run snapshot (red team @f8bce32; the source set @2f40596): the test source set,
    # compile_commands.json, every source's object, the registration set and every registered
    # binary are fingerprinted BEFORE ctest and must be found unchanged after it — a test that
    # writes any of them while running (a forged compile entry + object for a source no target
    # built made escape 2 green; deleting an unregistered source mid-run made escape 1 green,
    # since the tool walks the sources after ctest) is refused.
    # Held in this shell's memory and handed over on stdin, not written under build/, so a
    # running test cannot rewrite the snapshot as well. (A test that reaches this shell's
    # memory, or rewrites tools/check_test_set.py or this script while running, is outside
    # what artefact reading can establish — stated, not covered.)
    rm -f build/native/Testing/junit.xml   # first: a failed snapshot must not leave a stale record either
    local pre
    pre=$(python3 tools/check_test_set.py --snapshot) \
      || { echo "unit: pre-run snapshot of the build artefacts failed (nothing to compare the run against)" >&2; return 1; }
    ctest --preset native --output-on-failure --output-junit Testing/junit.xml \
      --test-output-size-passed 10000000
    # ctest only prints test-binary stdout inline on failure, so on a green
    # run the "EXECUTED: <n>" line lives in ctest's per-test log instead.
    local log="build/native/Testing/Temporary/LastTest.log"
    local n=0
    if [ -f "$log" ]; then
      n=$(grep -o 'EXECUTED: [0-9]\+' "$log" | awk -F': ' '{s+=$2} END{print s+0}') || true
    fi
    n=${n:-0}
    echo "unit: executed $n check(s) (ctest path)"
    # The floor sees a mass loss of checks; it cannot see ONE tests/{unit,property}/test_*.cpp
    # that is built but never add_test-ed, compiled by no target, or DISABLED, while the rest
    # keep the sum above it (#133). check_test_set.py reads compile_commands.json, the objects,
    # `ctest --show-only` and this run's JUnit record, and names every escaped source (or
    # fails when there are no sources at all). The two checks are independent: each reports
    # its own failure, neither masks the other. It runs AFTER the sum because
    # `ctest --show-only` rewrites LastTest.log (the tool restores it, belt and braces).
    local rc=0
    python3 tools/check_test_set.py --ctest --pre - <<<"$pre" || rc=1
    if [ "$n" -lt "$UNIT_TEST_FLOOR" ]; then
      echo "unit: executed check count ($n) below floor ($UNIT_TEST_FLOOR) - test filter may be broken" >&2
      rc=1
    fi
    return "$rc"
  else
    # One binary per SOURCE (the same unit_sources() list stage_build compiles): a source
    # whose binary is missing fails by name (#133) instead of dropping out of a walk over
    # build/native/test_*. Every binary runs; its output is always printed (even on failure)
    # and the EXECUTED: lines are summed across binaries for the floor. The same predicate as
    # the ctest path (red team @ceab86f, finding 1): exit 0 alone is not execution — the
    # binary must print `EXECUTED: n` with n > 0, else it is named and the stage fails. A
    # source newer than its binary is named too (@3dde163 finding 2; the ctest path's
    # source-vs-object rule in kind) — an mtime comparison is a control, not a guarantee.
    # Three passes, not one (red team @2f40596 finding 2): the evidence for every source —
    # the source list itself, its binary's existence and freshness, and the binary's identity
    # (dev, ino, size, mtime, ctime; what `stat` shows, as the ctest path's snapshot) — is
    # gathered BEFORE any binary runs, because a binary is arbitrary code that can mint a
    # later source's binary or overwrite it in place. After the run the source list and every
    # identity must be unchanged. Identity by stat is a control, not a guarantee (ctime is
    # what utime cannot put back; root or a moved clock can).
    local total=0 count=0 t bin out rc n srcs
    local -A pre_id
    unit_sources_plain || return 1
    unit_sources_unique || return 1
    srcs=$(unit_sources)   # taken ONCE, before the run; the loops below walk this list, not a fresh find
    while IFS= read -r t; do
      [ -n "$t" ] || continue
      bin="$BIN/$(basename "$t" .cpp)"
      if [ ! -x "$bin" ]; then
        echo "unit: $t: no binary at $bin (not built, or built under another name)" >&2
        return 1
      fi
      if [ "$t" -nt "$bin" ]; then
        echo "unit: $t: source is newer than its binary $bin; rebuild before verifying" >&2
        return 1
      fi
      pre_id[$t]=$(stat -c '%d %i %s %.9Y %.9Z' "$bin")
    done <<<"$srcs"
    while IFS= read -r t; do
      [ -n "$t" ] || continue
      bin="$BIN/$(basename "$t" .cpp)"
      set +e
      out=$("$bin" </dev/null)   # not the loop's source list: a binary reading stdin must not eat it (review @3dde163)
      rc=$?
      set -e
      echo "$out"
      if [ "$rc" -ne 0 ]; then
        echo "unit: $(basename "$bin") failed (exit $rc)" >&2
        return "$rc"
      fi
      n=$(printf '%s\n' "$out" | grep -o '^EXECUTED: [0-9]\+$' | grep -o '[0-9]\+' | tail -1) || true
      if [ -z "$n" ]; then
        echo "unit: $t: $(basename "$bin") exited 0 but printed no EXECUTED line — that is not execution" >&2
        return 1
      fi
      if [ "$n" -eq 0 ]; then
        echo "unit: $t: $(basename "$bin") reported EXECUTED: 0 — no test case executed" >&2
        return 1
      fi
      total=$((total + n))
      count=$((count + 1))
    done <<<"$srcs"
    if [ "$(unit_sources)" != "$srcs" ]; then
      echo "unit: test source set changed during the run (before: $(tr '\n' ' ' <<<"$srcs")— after: $(unit_sources | tr '\n' ' ')) — a test deleted or added a test source? rerun" >&2
      return 1
    fi
    while IFS= read -r t; do
      [ -n "$t" ] || continue
      bin="$BIN/$(basename "$t" .cpp)"
      if [ "$(stat -c '%d %i %s %.9Y %.9Z' "$bin")" != "${pre_id[$t]}" ]; then
        echo "unit: $t: $bin changed during the run (the file that ran is not the file stage_build produced — overwritten by an earlier test?); rebuild and rerun" >&2
        return 1
      fi
    done <<<"$srcs"
    if [ "$count" -eq 0 ]; then
      echo "unit: no test sources under tests/unit, tests/property (nothing to verify is a failure, not a pass)" >&2
      return 1
    fi
    echo "unit: verified $count test binaries (built and executed; bootstrap path)"   # one per source: basenames are unique (checked above)
    echo "unit: executed $total check(s) (bootstrap path)"
    if [ "$total" -lt "$UNIT_TEST_FLOOR" ]; then
      echo "unit: executed check count ($total) below floor ($UNIT_TEST_FLOOR) - test filter may be broken" >&2
      return 1
    fi
  fi
}

stage_refimpl() {
  # Ordering dependency (red team @ceab86f, finding 2): tools/refimpl/test_test_set_gate.py's
  # two real-tree controls read build/native's artefacts AND the JUnit record that THIS
  # pipeline's `unit` stage wrote — so `refimpl` must follow `build unit` in the same
  # invocation, as the default list and ci.yml's explicit list both do. Without them the
  # controls skip locally and FAIL on CI (GITHUB_ACTIONS): a bare `./pipeline.sh refimpl`
  # on a runner is a failure, not a pass.
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
