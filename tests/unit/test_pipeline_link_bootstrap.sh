#!/usr/bin/env bash
# Regression test for pipeline.sh's bootstrap g++ path (stage_build, the
# `command -v cmake` else-branch): proves link/*.cpp sources are compiled into
# every Catch2 test binary and into l3_helper, mirroring the existing l3/*.cpp
# handling (specs/002-trunk-link-layer tasks.md T002). Also proves the glob
# tolerates link/ holding zero .cpp files (today's state).
#
# Usage: tests/unit/test_pipeline_link_bootstrap.sh   (run from repo root or anywhere)
set -euo pipefail
cd "$(dirname "$0")/../.."

PROBE=link/_pipeline_test_probe.cpp
SYMBOL=omgp_link_pipeline_test_probe
cleanup() { rm -f "$PROBE"; }
trap cleanup EXIT

cat >"$PROBE" <<EOF
// Temporary probe, deleted by this test's cleanup trap. Its presence in
// build/native binaries proves the bootstrap stage_build linked link/*.cpp in.
extern "C" void ${SYMBOL}() {}
EOF

# Force the bootstrap g++ branch of stage_build regardless of the host
# environment, by dropping cmake's directory from PATH (cmake lives in
# /usr/local/bin on CI/agent hosts; g++ lives in /usr/bin and stays reachable).
BOOT_PATH=$PATH
if command -v cmake >/dev/null 2>&1; then
  cmake_dir=$(dirname "$(command -v cmake)")
  BOOT_PATH=$(IFS=:; new=(); for d in $PATH; do [ "$d" = "$cmake_dir" ] || new+=("$d"); done; IFS=:; echo "${new[*]}")
fi
if PATH="$BOOT_PATH" command -v cmake >/dev/null 2>&1; then
  echo "test_pipeline_link_bootstrap: SKIP — could not remove cmake from PATH on this host"
  exit 0
fi

echo "test_pipeline_link_bootstrap: forcing bootstrap build with a probe link/*.cpp file"
out=$(PATH="$BOOT_PATH" ./pipeline.sh build 2>&1) || {
  echo "test_pipeline_link_bootstrap: FAIL — bootstrap build did not succeed with a link/*.cpp file present"
  echo "$out"
  exit 1
}
if ! echo "$out" | grep -q "bootstrap g++ build"; then
  echo "test_pipeline_link_bootstrap: FAIL — expected bootstrap branch did not run"
  echo "$out"
  exit 1
fi

fail=0
one_catch_binary=""
for bin in build/native/test_*; do
  [ -x "$bin" ] || continue
  [ "$(basename "$bin")" = "test_smoke" ] && continue   # test_smoke has its own main; never links link/*.cpp
  one_catch_binary="$bin"
  if ! nm "$bin" 2>/dev/null | grep -q "$SYMBOL"; then
    echo "test_pipeline_link_bootstrap: FAIL — $bin does not contain probe symbol $SYMBOL"
    fail=1
  fi
done
if [ -z "$one_catch_binary" ]; then
  echo "test_pipeline_link_bootstrap: FAIL — no Catch2 test binary was built to check"
  fail=1
fi

if ! nm build/native/l3_helper 2>/dev/null | grep -q "$SYMBOL"; then
  echo "test_pipeline_link_bootstrap: FAIL — build/native/l3_helper does not contain probe symbol $SYMBOL"
  fail=1
fi

rm -f "$PROBE"
echo "test_pipeline_link_bootstrap: re-running bootstrap build with zero link/*.cpp files (today's real state)"
out2=$(PATH="$BOOT_PATH" ./pipeline.sh build 2>&1) || {
  echo "test_pipeline_link_bootstrap: FAIL — bootstrap build failed with an empty link/*.cpp glob"
  echo "$out2"
  exit 1
}

if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "test_pipeline_link_bootstrap: PASS"
