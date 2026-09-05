#!/usr/bin/env bash
# Regression test for the per-binary floor's completeness in pipeline.sh
# (stage_unit, ctest path). UNIT_PER_BINARY_FLOOR is only ever compared for
# binary names that appear in build/native/Testing/Temporary/LastTest.log —
# a binary that is registered (so the source-registration guard sees it) but
# never actually executed (CTest PROPERTIES DISABLED TRUE, or a preset
# exclude filter) simply never appears in that log, so its floor entry is
# never checked and the aggregate UNIT_TEST_FLOOR's ~100-check slack absorbs
# the loss silently (review PR #128 @ eeea8ca, [MEDIUM]: "the per-binary
# floor only fires for binaries that actually ran"). Proves stage_unit now
# fails when a floor-tracked binary goes dark this way, instead of reporting
# green with that binary's checks simply missing from the count.
#
# Usage: ./pipeline.sh selftest   (invokes this from a scratch copy of the tree)
set -euo pipefail
cd "$(dirname "$0")/../.."

# This scenario only exercises the ctest path: CTest PROPERTIES DISABLED is a
# ctest concept with no bootstrap-glob equivalent (the bootstrap path has its
# own, separately-guarded presence check for "binary never built"). Mirrors
# test_pipeline_registration.sh's guard for the same reason.
if ! command -v cmake >/dev/null 2>&1 || ! command -v ctest >/dev/null 2>&1; then
  echo "test_pipeline_binary_presence: SKIP — no cmake/ctest on this host; this scenario only applies to the ctest path"
  exit 0
fi

CMAKELISTS=CMakeLists.txt
BACKUP=$(mktemp)
cp "$CMAKELISTS" "$BACKUP"
restore() { cp "$BACKUP" "$CMAKELISTS"; rm -f "$BACKUP"; }
trap restore EXIT

echo "test_pipeline_binary_presence: disabling test_canonical_frame (registration and source untouched, ctest just never runs it)"
printf '\nset_tests_properties(test_canonical_frame PROPERTIES DISABLED TRUE)\n' >>"$CMAKELISTS"

set +e
out=$(./pipeline.sh codegen build unit 2>&1)
rc=$?
set -e

if [ "$rc" -eq 0 ]; then
  echo "test_pipeline_binary_presence: FAIL — stage_unit passed with test_canonical_frame disabled and never executed"
  echo "$out"
  exit 1
fi
if ! printf '%s\n' "$out" | grep -q "never ran"; then
  echo "test_pipeline_binary_presence: FAIL — stage_unit failed, but not with the expected message"
  echo "$out"
  exit 1
fi

echo "test_pipeline_binary_presence: PASS — stage_unit correctly failed when a floor-tracked binary never ran"
