#!/usr/bin/env bash
# Fuzz harness: builds the libFuzzer targets (CMake preset `fuzz`, clang) and runs each for
# an equal share of the budget, seeded from the golden vectors. Exit non-zero on any crash,
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
for t in "${TARGETS[@]}"; do
  art="$BUILD/artifacts/$t/"
  rm -rf "$art" && mkdir -p "$art"   # findings are from THIS run only; stale artefacts never count
  log="$BUILD/$t.log"
  set +e
  "${NOASLR[@]}" "$BUILD/$t" "$BUILD/corpus/$t" -max_total_time="$per" -timeout=5 -rss_limit_mb=512 \
    -artifact_prefix="$art" -print_final_stats=1 > "$log" 2>&1
  trc=$?
  set -e
  runs=$(grep -oE 'stat::number_of_executed_units: *[0-9]+' "$log" | grep -oE '[0-9]+$' || echo 0)
  cov=$(grep -oE 'cov: *[0-9]+' "$log" | tail -1 | grep -oE '[0-9]+' || echo 0)
  findings=$(find "$art" -type f | wc -l)
  echo "fuzz: $t runs=${runs:-0} cov=${cov:-0} findings=$findings exit=$trc"
  if [ "$trc" -ne 0 ] || [ "$findings" -ne 0 ]; then
    rc=1
    grep -E 'ERROR: |SUMMARY: |==.*==' "$log" | head -5
    for a in "$art"*; do [ -f "$a" ] && echo "  reproduce: $BUILD/$t $a"; done
  fi
done
[ "$rc" -eq 0 ] && echo "fuzz: clean rejections only across ${#TARGETS[@]} targets" || echo "fuzz: FINDINGS — see above" >&2
exit $rc
