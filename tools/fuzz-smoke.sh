#!/usr/bin/env bash
# Fuzz harness: builds the libFuzzer targets (CMake preset `fuzz`, clang) and runs each for
# an equal share of the budget (serially by default; OMGP_FUZZ_JOBS=N runs N at a time),
# seeded from the golden vectors. Exit non-zero on any crash,
# timeout, leak or sanitizer report (spec 001 FR-026, SC-004). CI deep-verify calls it with
# 600; locally use 60 for a smoke run or more for a soak.
#   ./tools/fuzz-smoke.sh <seconds>
# Every partial path states its blind spot (CLAUDE.md working agreements).
set -euo pipefail
cd "$(dirname "$0")/.."
BUDGET=${1:-60}
TARGETS=(fuzz_header fuzz_payload fuzz_descriptor fuzz_roundtrip fuzz_frame)
BUILD=build/fuzz
CXX=${OMGP_FUZZ_CXX:-clang++}

if ! command -v "$CXX" >/dev/null 2>&1 || ! echo 'extern "C" int LLVMFuzzerTestOneInput(const unsigned char*,unsigned long){return 0;}' \
     | "$CXX" -x c++ -fsanitize=fuzzer - -o /dev/null >/dev/null 2>&1; then
  echo "fuzz: clang with libFuzzer not found — cannot run (blind spot: no fuzzing in this environment)" >&2
  exit 1
fi

echo "fuzz: budget ${BUDGET}s across ${#TARGETS[@]} targets ($((BUDGET / ${#TARGETS[@]}))s each) using $CXX"
python3 tools/codegen.py --vectors tests/vectors >/dev/null
CXX="$CXX" cmake --preset fuzz >/dev/null
cmake --build --preset fuzz >/dev/null

# Seed corpora from the golden vectors: messages feed header/roundtrip as-is, payload as
# [opcode, dir, payload...]; descriptors feed the descriptor target.
python3 - "$BUILD/corpus" <<'EOF'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
for t in ("fuzz_header", "fuzz_payload", "fuzz_descriptor", "fuzz_roundtrip", "fuzz_frame"):
    (root / t).mkdir(parents=True, exist_ok=True)
for f in sorted(pathlib.Path("tests/vectors").glob("*.json")):
    v = json.loads(f.read_text())
    raw = bytes.fromhex(v["bytes"].replace(" ", ""))
    if v["kind"] == "message":
        (root / "fuzz_header" / f.stem).write_bytes(raw)
        (root / "fuzz_roundtrip" / f.stem).write_bytes(raw)
        (root / "fuzz_payload" / f.stem).write_bytes(bytes([raw[0], raw[3] & 1]) + raw[5:])
    elif v["kind"] == "descriptor":
        (root / "fuzz_descriptor" / f.stem).write_bytes(raw)
    elif v["kind"] == "frame":
        (root / "fuzz_frame" / f.stem).write_bytes(raw)
EOF

# ASan-instrumented binaries intermittently segfault at startup on WSL2 when ASLR places a
# mapping inside the shadow region (measured here: 5/10 launches with ASLR, 0/10 without).
# Disabling ASLR for the target process removes that noise; harmless elsewhere.
NOASLR=()
if command -v setarch >/dev/null 2>&1; then NOASLR=(setarch "$(uname -m)" -R); fi

rc=0
per=$((BUDGET / ${#TARGETS[@]}))
[ "$per" -lt 1 ] && per=1
# Targets run in waves of OMGP_FUZZ_JOBS at a time — default 1, i.e. serial, and CI uses the
# default. -max_total_time is WALL time, so the budget buys executions only while a fuzzer
# has a core to itself; measured 2026-09-06 (gate-budget PR) on a 12-thread host, five
# fuzzers at once each reached 55–75 % of their solo executions (fuzz_frame 216 550 → 115 044
# in the same 12 s; SMT siblings and all-core clocks, not oversubscription) — a smaller run
# wearing the same number. Parallel is therefore opt-in for a quick local smoke
# (OMGP_FUZZ_JOBS=5 ./tools/fuzz-smoke.sh 60), never the evidence run.
jobs=${OMGP_FUZZ_JOBS:-1}
[ "$jobs" -gt "${#TARGETS[@]}" ] && jobs=${#TARGETS[@]}
[ "$jobs" -lt 1 ] && jobs=1
echo "fuzz: ${jobs} target(s) at a time"
declare -A trc
i=0
while [ "$i" -lt "${#TARGETS[@]}" ]; do
  wave=("${TARGETS[@]:$i:$jobs}")
  pids=()
  for t in "${wave[@]}"; do
    art="$BUILD/artifacts/$t/"
    rm -rf "$art" && mkdir -p "$art"   # findings are from THIS run only; stale artefacts never count
    "${NOASLR[@]}" "$BUILD/$t" "$BUILD/corpus/$t" -max_total_time="$per" -timeout=5 -rss_limit_mb=512 \
      -artifact_prefix="$art" -print_final_stats=1 > "$BUILD/$t.log" 2>&1 &
    pids+=($!)
  done
  for k in "${!wave[@]}"; do
    set +e; wait "${pids[$k]}"; trc[${wave[$k]}]=$?; set -e   # each target's own exit status
  done
  i=$((i + jobs))
done
for t in "${TARGETS[@]}"; do
  art="$BUILD/artifacts/$t/"
  log="$BUILD/$t.log"
  runs=$(grep -oE 'stat::number_of_executed_units: *[0-9]+' "$log" | grep -oE '[0-9]+$' || echo 0)
  cov=$(grep -oE 'cov: *[0-9]+' "$log" | tail -1 | grep -oE '[0-9]+' || echo 0)
  findings=$(find "$art" -type f | wc -l)
  echo "fuzz: $t runs=${runs:-0} cov=${cov:-0} findings=$findings exit=${trc[$t]}"
  if [ "${trc[$t]}" -ne 0 ] || [ "$findings" -ne 0 ]; then
    rc=1
    # `|| true`: a target that died without a sanitizer/libFuzzer line (OOM-killed, a bad
    # binary) left grep with no match, and under set -eo pipefail that aborted the script
    # here — later targets unreported, no FINDINGS line (found by injecting an `exit 77`
    # shim as fuzz_header, gate-budget PR 2026-09-06). The exit status was already 1.
    { grep -E 'ERROR: |SUMMARY: |==.*==' "$log" || true; } | head -5
    for a in "$art"*; do [ -f "$a" ] && echo "  reproduce: $BUILD/$t $a"; done
  fi
done
[ "$rc" -eq 0 ] && echo "fuzz: clean rejections only across ${#TARGETS[@]} targets" || echo "fuzz: FINDINGS — see above" >&2
exit $rc
