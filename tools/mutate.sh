#!/usr/bin/env bash
# Diff-scoped mutation testing with Mull (spec 001 FR-027, research R-04; config in
# tools/mutate.cfg). Only lines changed relative to --diff <ref> under the embedded-path
# directories are mutated, so the CI deep-verify job runs in bounded time.
#
#   ./tools/mutate.sh --diff origin/main --require     # CI: fail if Mull is missing
#   ./tools/mutate.sh --diff HEAD~1                    # local: disclosed skip if Mull is missing
#   ./tools/mutate.sh --diff <ref> --dry-run           # print the scope and stop
#
# Overrides: OMGP_CLANG_MAJOR, MULL_RUNNER (path), MULL_PLUGIN (path) — used to run an
# extracted (not installed) Mull package.
# Every partial path states its blind spot (CLAUDE.md working agreements).
set -euo pipefail
cd "$(dirname "$0")/.."
CFG=tools/mutate.cfg
BUILD=build/mutate

cfg() { sed -n "s/^$1 *= *//p" "$CFG" | head -1 | sed 's/ *#.*//; s/ *$//'; }
VERSION=$(cfg version)
THRESHOLD=$(cfg threshold_pct)
SCOPE_DIRS=$(cfg scope_dirs)
TIMEOUT_MS=$(cfg timeout_ms)
GROUPS_=$(cfg groups)

REF=""; REQUIRE=0; DRY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --diff) REF="$2"; shift 2 ;;
    --require) REQUIRE=1; shift ;;
    --threshold) THRESHOLD="$2"; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    *) echo "mutate: unknown argument $1" >&2; exit 2 ;;
  esac
done

# --- scope: changed embedded-path sources -------------------------------------------------------
# A depth-1 CI checkout has no origin/<branch> refs. `git diff <ref>` only needs the ref's
# tree (no merge base), so a shallow fetch of that one ref is enough to recover.
if [ -n "$REF" ] && ! git rev-parse --verify --quiet "$REF^{commit}" >/dev/null; then
  case "$REF" in
    origin/*)
      b=${REF#origin/}
      echo "mutate: '$REF' not in this clone — fetching it shallowly"
      # Explicit refspec: a --depth clone is --single-branch, so its configured refspec
      # would not map the fetched branch onto refs/remotes/origin/<branch>.
      git fetch --quiet --depth=1 origin "+refs/heads/$b:refs/remotes/origin/$b" 2>/dev/null || true ;;
  esac
  if ! git rev-parse --verify --quiet "$REF^{commit}" >/dev/null; then
    echo "mutate: --diff ref '$REF' is not a commit in this clone and could not be fetched" >&2
    exit 2
  fi
fi
if [ -n "$REF" ]; then
  # shellcheck disable=SC2086
  SCOPE=$(git diff --name-only "$REF" -- $SCOPE_DIRS | grep -E '\.(cpp|hpp|h|cc)$' || true)
else
  # shellcheck disable=SC2086
  SCOPE=$(find $SCOPE_DIRS -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) 2>/dev/null | sort)
fi
if [ -z "$SCOPE" ]; then
  echo "mutation: nothing in scope (${REF:-full tree}) — no changed sources under: $SCOPE_DIRS"
  exit 0
fi
echo "mutation: scope: $(echo "$SCOPE" | tr '\n' ' ')"
[ "$DRY" -eq 1 ] && exit 0

# --- tool presence -------------------------------------------------------------------------------
CLANG_MAJOR=${OMGP_CLANG_MAJOR:-$(clang++ --version 2>/dev/null | grep -oE 'version [0-9]+' | head -1 | grep -oE '[0-9]+' || true)}
CLANG_MAJOR=${CLANG_MAJOR:-$(cfg clang_major)}
RUNNER=${MULL_RUNNER:-$(cfg runner | sed "s/{clang_major}/$CLANG_MAJOR/")}
PLUGIN=${MULL_PLUGIN:-$(cfg plugin | sed "s/{clang_major}/$CLANG_MAJOR/")}
if ! command -v "$RUNNER" >/dev/null 2>&1 || [ ! -f "$PLUGIN" ] || ! command -v clang++ >/dev/null 2>&1; then
  if [ "$REQUIRE" -eq 1 ]; then
    echo "mutation: mull $VERSION required but not found (runner: $RUNNER, plugin: $PLUGIN, clang major: ${CLANG_MAJOR:-?})" >&2
    exit 1
  fi
  echo "mutation: mull not present — skipped (blind spot: no mutation coverage in this environment; CI deep-verify runs it with --require)"
  exit 0
fi
echo "mutation: mull $VERSION via $RUNNER (clang $CLANG_MAJOR), threshold ${THRESHOLD}%"

# --- configuration is TWO-PHASE ----------------------------------------------------------------
# The IR frontend plugin reads mull.yml at COMPILE time (mutators, include/exclude paths) and
# the runner reads it again at RUN time (git diff filter, timeout). Both use the build
# directory as cwd, so the file lives there. Measured on Mull 0.34.0: gitDiffRef present at
# compile time makes the plugin embed NO mutants (its git lookup runs from a build
# subdirectory and matches nothing), while the same key at run time filters correctly. So
# phase 1 instruments everything under the scope dirs; phase 2 appends the git filter.
# The build is always fresh: objects compiled without the config contain no mutants.
python3 tools/codegen.py --vectors tests/vectors >/dev/null
ROOT=$(pwd)
rm -rf "$BUILD" && mkdir -p "$BUILD"
# Compile-time config: mutators only — every translation unit is instrumented. Measured on
# 0.34.0: any includePaths/excludePaths here leaves the runner with "No mutants found",
# because excluding the TU that holds main() (Catch2's amalgamated source) removes the
# run-time mutant dispatch the plugin injects there. Scoping is therefore done by the git
# diff filter at run time plus a post-filter on the report (only scope_dirs are counted).
{
  echo "mutators:"; for g in $GROUPS_; do echo "  - $g"; done   # names must be known to Mull (see mutate.cfg)
  echo "timeout: $TIMEOUT_MS"
  echo "quiet: true"
} > "$BUILD/mull.yml"

# --- instrumented build -------------------------------------------------------------------------
cmake -S . -B "$BUILD" -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug -DOMGP_SANITIZERS=OFF \
  -DCMAKE_CXX_FLAGS="-fpass-plugin=$PLUGIN -g -grecord-command-line -O0" >/dev/null
cmake --build "$BUILD" --parallel >/dev/null
embedded=$(nm "$BUILD"/l3/CMakeFiles/omgp_l3.dir/*.o 2>/dev/null | grep -c ' t mull_' || true)
echo "mutation: instrumented build done (${embedded:-0} mutated functions in l3/)"

# Phase 2 (run time): mutators + timeout only. Diff scoping is NOT delegated to Mull's
# gitDiffRef: measured on 0.34.0, its filter keeps mutants in modified files but drops
# every mutant in a file the diff ADDS (new-file hunks) — useless for a feature whose PRs
# mostly add files. Instead every mutant runs (cheap with --workers) and the merge below
# keeps only those on lines `git diff -U0 <ref>` marks as added/changed under scope_dirs.
{
  echo "mutators:"; for g in $GROUPS_; do echo "  - $g"; done
  echo "timeout: $TIMEOUT_MS"
  echo "quiet: true"
} > "$BUILD/mull.yml"
# Changed-line ranges per in-scope file (new files are whole-file ranges), as JSON.
if [ -n "$REF" ]; then
  # shellcheck disable=SC2086
  git diff -U0 "$REF" -- $SCOPE_DIRS | python3 -c '
import json, re, sys
ranges, cur = {}, None
for line in sys.stdin:
    if line.startswith("+++ "):
        p = line[4:].strip()
        cur = None if p == "/dev/null" else (p[2:] if p.startswith("b/") else p)
        if cur: ranges.setdefault(cur, [])
    elif line.startswith("@@") and cur:
        m = re.match(r"@@ -\S+ \+(\d+)(?:,(\d+))? @@", line)
        start, count = int(m.group(1)), int(m.group(2)) if m.group(2) is not None else 1
        if count: ranges[cur].append([start, start + count - 1])
json.dump(ranges, sys.stdout)' > "$BUILD/scope_ranges.json"
else
  echo '{}' > "$BUILD/scope_ranges.json"
fi

# --- run the unit binaries under the runner and merge the Elements JSON reports ---------------------
# Unit tests are the oracle: the seeded property tests are too slow per mutant at -O0 and
# assert invariants, not specific values. Reporters (mull-runner --help, 0.34.0): IDE,
# SQLite, GitHubAnnotations, Patches, Elements (Mutation Testing Elements JSON), Sarif.
rc=0
REPORTS="$BUILD/reports"
rm -rf "$REPORTS" && mkdir -p "$REPORTS"
EXTRA=()
[ -n "${GITHUB_ACTIONS:-}" ] && EXTRA=(--reporters GitHubAnnotations)
for bin in "$BUILD"/test_l3_header "$BUILD"/test_l3_payload "$BUILD"/test_l3_descriptor; do
  [ -x "$bin" ] || continue
  name=$(basename "$bin")
  # mull-runner exits non-zero whenever survivors exist, so its exit code is not a failure
  # signal; a missing report is. (All mutants run here; the diff scoping happens in the
  # merge below, so per-binary survivor counts are NOT the gate's numbers.)
  ( cd "$BUILD" && "$RUNNER" --reporters IDE --reporters Elements "${EXTRA[@]}" --report-dir reports \
      --report-name "$name" --ide-reporter-show-killed --timeout "$TIMEOUT_MS" \
      --workers "${OMGP_MUTATE_WORKERS:-$(nproc)}" "./$name" > "$name.mull.log" 2>&1 ) || true
  if [ ! -f "$REPORTS/$name.json" ] && ! grep -q 'No mutants found' "$BUILD/$name.mull.log"; then
    echo "mutation: $name: runner produced no report — failing (see $BUILD/$name.mull.log):" >&2
    { grep -iE 'error' "$BUILD/$name.mull.log" || true; } | head -3 >&2
    rc=1
  fi
  echo "mutation: $name: $(grep -oE 'Surviving mutants: [0-9]+|No mutants found' "$BUILD/$name.mull.log" | head -1 || echo 'ran')"
done

python3 - "$REPORTS" "$THRESHOLD" "${REF:-full tree}" "$ROOT" "$SCOPE_DIRS" "$BUILD/scope_ranges.json" <<'EOF' || rc=1
import json, pathlib, sys
build, threshold, ref = pathlib.Path(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
root, scope_dirs = sys.argv[4], sys.argv[5].split()
ranges = json.load(open(sys.argv[6]))            # {rel_path: [[start, end], ...]} — empty when no --diff
def rel_of(path: str) -> str:
    return path[len(root) + 1:] if path.startswith(root + "/") else path
def in_scope(path: str, line) -> bool:
    """Embedded-path source (tests/tools carry mutants too but never count), and — with
    --diff — on a line the diff added or changed (new files are whole-file ranges)."""
    rel = rel_of(path)
    if not any(rel.startswith(d + "/") for d in scope_dirs):
        return False
    if not ranges:
        return True
    return any(a <= (line or -1) <= b for a, b in ranges.get(rel, []))
# Mutation Testing Elements report: {"files": {path: {"mutants": [{"mutatorName", "location":
# {"start": {"line", "column"}}, "status"}]}}}. The same mutant appears in every binary's
# report; it counts as killed if ANY binary kills it (Killed/Timeout/RuntimeError), survived
# only if every binary that reached it let it live, not-covered if no binary reached it.
RANK = {"Killed": 3, "Timeout": 3, "RuntimeError": 3, "CompileError": 3, "Survived": 2,
        "NoCoverage": 1, "Ignored": 0, "Pending": 0}
best: dict[tuple, tuple[int, str]] = {}
reports = sorted(build.glob("*.json"))
for r in reports:
    try:
        doc = json.loads(r.read_text())
    except json.JSONDecodeError:
        print(f"mutation: could not parse {r.name}")
        continue
    for path, f in (doc.get("files") or {}).items():
        for m in f.get("mutants", []):
            loc = (m.get("location") or {}).get("start") or {}
            if not in_scope(path, loc.get("line")):
                continue
            key = (path, loc.get("line"), loc.get("column"), m.get("mutatorName"))
            rank = RANK.get(m.get("status"), 0)
            if rank > best.get(key, (-1, ""))[0]:
                best[key] = (rank, m.get("status", "?"))
killed = sum(1 for r, _ in best.values() if r == 3)
survived = sum(1 for r, _ in best.values() if r == 2)
not_covered = sum(1 for r, _ in best.values() if r == 1)
survivors = [f"{k[0]}:{k[1]}:{k[2]} {k[3]}" for k, (r, _) in sorted(best.items(), key=lambda kv: (kv[0][0], kv[0][1] or 0)) if r == 2]
total = killed + survived
rate = (100.0 * killed / total) if total else 0.0
print(f"mutation: diff_ref={ref} reports={len(reports)} mutants={total + not_covered} killed={killed} survived={survived} not_covered={not_covered} kill_rate={rate:.1f}% threshold={threshold}%")
for s in survivors[:50]: print(f"  survivor: {s}")
(build.parent / "report.json").write_text(json.dumps({"diff_ref": ref, "mutants_total": total + not_covered, "killed": killed, "survived": survived, "not_covered": not_covered, "kill_rate": rate, "threshold": threshold, "survivors": survivors}, indent=2) + "\n")
if not reports:
    print("mutation: no Mull reports produced — failing (blind spot: the runner did not execute)")
    sys.exit(1)
if total + not_covered == 0:
    print("mutation: scope is non-empty but Mull generated no mutants — failing (blind spot: instrumentation is not reaching the changed code)")
    sys.exit(1)
sys.exit(0 if rate >= threshold else 1)
EOF
[ "$rc" -eq 0 ] && echo "mutation: PASS" || echo "mutation: FAIL" >&2
exit $rc
