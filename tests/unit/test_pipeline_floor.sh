#!/usr/bin/env bash
# Regression test for the UNIT_TEST_FLOOR gate in pipeline.sh (stage_unit).
# Proves a broken test filter (fewer checks executed than expected) fails
# the stage loudly, instead of silently reporting a smaller-than-expected
# green run. Temporarily raises the floor above the actual check count,
# asserts the stage fails, then restores the original file.
#
# Usage: tests/unit/test_pipeline_floor.sh   (run from repo root or anywhere)
set -euo pipefail
cd "$(dirname "$0")/../.."

PIPELINE=pipeline.sh
BACKUP=$(mktemp)
cp "$PIPELINE" "$BACKUP"
restore() { cp "$BACKUP" "$PIPELINE"; rm -f "$BACKUP"; }
trap restore EXIT

CURRENT_FLOOR=$(grep -oE 'UNIT_TEST_FLOOR=[0-9]+' "$PIPELINE" | grep -oE '[0-9]+')
IMPOSSIBLE_FLOOR=$((CURRENT_FLOOR + 1000))

echo "test_pipeline_floor: raising UNIT_TEST_FLOOR $CURRENT_FLOOR -> $IMPOSSIBLE_FLOOR to prove the gate fires"
sed -i "s/UNIT_TEST_FLOOR=${CURRENT_FLOOR}/UNIT_TEST_FLOOR=${IMPOSSIBLE_FLOOR}/" "$PIPELINE"

set +e
./pipeline.sh codegen build unit >/tmp/omgp_floor_test_out.$$ 2>&1
rc=$?
set -e

if [ "$rc" -eq 0 ]; then
  echo "test_pipeline_floor: FAIL — stage_unit passed with an impossible floor ($IMPOSSIBLE_FLOOR); the gate is not enforcing"
  cat /tmp/omgp_floor_test_out.$$
  rm -f /tmp/omgp_floor_test_out.$$
  exit 1
fi

if ! grep -q "below floor" /tmp/omgp_floor_test_out.$$; then
  echo "test_pipeline_floor: FAIL — stage_unit failed, but not with the expected floor message"
  cat /tmp/omgp_floor_test_out.$$
  rm -f /tmp/omgp_floor_test_out.$$
  exit 1
fi

rm -f /tmp/omgp_floor_test_out.$$
echo "test_pipeline_floor: PASS — stage_unit correctly failed with floor $IMPOSSIBLE_FLOOR (restoring $CURRENT_FLOOR)"
