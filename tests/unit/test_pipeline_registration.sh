#!/usr/bin/env bash
# Regression test for the ctest-registration check in pipeline.sh (stage_unit).
# Proves that a tests/unit|property/test_*.cpp file the ctest path cannot see
# fails the stage loudly instead of the ctest path silently never running it
# while the stage still reports green. Three ways a file goes dark:
#   1. its CMakeLists.txt registration is removed outright (red-team PR #128
#      finding 1: dropping a whole binary's registration can hide behind the
#      check-count floor's slack);
#   2. it is a new file, never registered in the first place (finding 2);
#   3. a registration line's *name* is right but its *source* argument was
#      repointed at a different test_*.cpp — e.g. a copy-pasted
#      omgp_add_catch_test line — so the named ctest entry still exists
#      (and even reports MORE checks, the duplicated file's), leaving the
#      true source file registered nowhere (red-team PR #128, second review
#      round, [HIGH]). This is the case a name-only check cannot see; the
#      fix matches by source path instead, checked here against a
#      tests/property/ file to cover that glob arm too (a name-only check
#      previously exercised tests/unit/ alone).
#
# Usage: ./pipeline.sh selftest   (invokes this from a scratch copy of the tree)
set -euo pipefail
cd "$(dirname "$0")/../.."

# The guard this test proves lives entirely in stage_unit's ctest branch
# (`command -v ctest && [ -f build/native/CTestTestfile.cmake ]`); the
# bootstrap g++ branch has no registration concept at all — it just globs
# every tests/*/test_*.cpp file and builds it, so there is nothing for a
# missing-registration scenario to catch there. On a cmake-less host all
# three scenarios below take the bootstrap branch, `./pipeline.sh codegen
# build unit` exits 0 exactly as CLAUDE.md says it should, and this test
# would report FAIL for a gate that cannot fire on that host (review PR #128
# @ eeea8ca, [MEDIUM]: "no cmake guard, so a default ./pipeline.sh fails on
# the documented cmake-less host"). Mirrors test_pipeline_link_bootstrap.sh's
# SKIP for the same reason, in the opposite direction (that script needs
# cmake absent; this one needs it present).
if ! command -v cmake >/dev/null 2>&1 || ! command -v ctest >/dev/null 2>&1; then
  echo "test_pipeline_registration: SKIP — no cmake/ctest on this host; the ctest-path registration guard this test exercises only applies there"
  exit 0
fi

CMAKELISTS=CMakeLists.txt
BACKUP=$(mktemp)
cp "$CMAKELISTS" "$BACKUP"
PROBE=tests/unit/test_pipeline_registration_probe.cpp
restore() { cp "$BACKUP" "$CMAKELISTS"; rm -f "$BACKUP" "$PROBE"; }
trap restore EXIT

run_expect_orphaned() {
  local label="$1" out rc
  set +e
  out=$(./pipeline.sh codegen build unit 2>&1)
  rc=$?
  set -e
  if [ "$rc" -eq 0 ]; then
    echo "test_pipeline_registration: FAIL ($label) — stage_unit passed with an orphaned test file"
    echo "$out"
    return 1
  fi
  if ! printf '%s\n' "$out" | grep -q "not registered as a ctest test"; then
    echo "test_pipeline_registration: FAIL ($label) — stage_unit failed, but not with the expected message"
    echo "$out"
    return 1
  fi
  echo "test_pipeline_registration: PASS ($label) — stage_unit correctly failed"
}

echo "test_pipeline_registration: dropping test_link_health's CMakeLists.txt registration (source file stays)"
python3 - <<'PY'
s = open('CMakeLists.txt').read()
needle = 'omgp_add_catch_test(test_link_health tests/unit/test_link_health.cpp)\n'
assert needle in s, "expected CMakeLists.txt line not found; has it moved?"
open('CMakeLists.txt', 'w').write(s.replace(needle, ''))
PY
run_expect_orphaned "dropped registration"

cp "$BACKUP" "$CMAKELISTS"

echo "test_pipeline_registration: adding a new test_*.cpp file with no CMakeLists.txt registration"
cat >"$PROBE" <<'EOF'
// Temporary probe, deleted by this test's cleanup trap. Proves the ctest
// path's registration check catches a test file nothing ever registered.
#include "catch_amalgamated.hpp"
TEST_CASE("orphaned probe test, never registered") { REQUIRE(true); }
EOF
run_expect_orphaned "never registered"

cp "$BACKUP" "$CMAKELISTS"

echo "test_pipeline_registration: repointing test_link_stuffing's registration at a different source file (name stays, tests/property/ arm)"
python3 - <<'PY'
s = open('CMakeLists.txt').read()
needle = 'omgp_add_catch_test(test_link_stuffing tests/property/test_link_stuffing.cpp)\n'
replacement = 'omgp_add_catch_test(test_link_stuffing tests/property/test_link_resync.cpp)\n'
assert needle in s, "expected CMakeLists.txt line not found; has it moved?"
open('CMakeLists.txt', 'w').write(s.replace(needle, replacement))
PY
run_expect_orphaned "name right, source repointed"

echo "test_pipeline_registration: PASS — all three orphaned-test scenarios fail the stage loudly"
