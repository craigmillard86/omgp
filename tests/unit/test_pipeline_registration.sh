#!/usr/bin/env bash
# Regression test for the ctest-registration check in pipeline.sh (stage_unit).
# Proves that a tests/unit|property/test_*.cpp file the ctest path cannot see
# — either because its CMakeLists.txt registration was removed (red-team
# PR #128 finding 1: dropping a whole binary's registration can hide behind
# the check-count floor's slack) or because it was never registered in the
# first place (finding 2) — fails the stage loudly instead of the ctest path
# silently never running it while the stage still reports green.
#
# Usage: tests/unit/test_pipeline_registration.sh   (run from repo root or anywhere)
set -euo pipefail
cd "$(dirname "$0")/../.."

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

echo "test_pipeline_registration: PASS — both orphaned-test scenarios fail the stage loudly"
