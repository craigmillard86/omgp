#!/usr/bin/env bash
# Diff-scoped mutation testing with Mull (spec 001 FR-027, research R-04; config in
# tools/mutate.cfg). Only lines changed relative to --diff <ref> under the embedded-path
# directories are gated, so the CI deep-verify job runs in bounded time.
#
# Gate (ruling docs/OPEN-QUESTIONS.md 2026-08-29): triage, not percentage. With --diff, every
# surviving mutant on a changed line must be killed by a test or carry a label on its source
# line — `// mutant-ok(equivalent|accepted[, mutator...]): <justification>` — and the run
# fails on any unlabelled survivor. Without --diff the whole-tree kill rate is reported as a
# trend (tools/mutate_report.py) and never gates.
#
# A changed file with no function body at all (e.g. a pure abstract interface) can never
# produce a mutant; that would otherwise look identical to the "instrumentation isn't
# reaching the code" blind spot. Opt out per file with `// mutation-exempt(no-body): <why>`
# (docs/OPEN-QUESTIONS.md 2026-08-30) — reviewed like mutant-ok, just at file granularity.
#
# Oracle: for every scope dir with changed files, the tests/unit binaries named test_<dir>_*
# (CMakeLists.txt omgp_add_catch_test) run under the runner. It was a hard-coded test_l3_*
# list until PR #94 (2026-08-30), the first link/ source, produced zero executed mutants and
# tripped the blind-spot rule. A changed dir with no unit binary fails before any build.
#
#   ./tools/mutate.sh --diff origin/main --require     # CI: fail if Mull is missing
#   ./tools/mutate.sh --diff HEAD~1                    # local: disclosed skip if Mull is missing
#   ./tools/mutate.sh --diff <ref> --dry-run           # print the scope and stop
#   ./tools/mutate.sh --trend-log metrics/mutation-trend.jsonl   # whole tree, append trend
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
MAX_UNLABELLED=$(cfg max_unlabelled_survivors)
CATEGORIES=$(cfg label_categories)
SCOPE_DIRS=$(cfg scope_dirs)
TIMEOUT_MS=$(cfg timeout_ms)
GROUPS_=$(cfg groups)

REF=""; REQUIRE=0; DRY=0; TREND_LOG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --diff) REF="$2"; shift 2 ;;
    --require) REQUIRE=1; shift ;;
    --dry-run) DRY=1; shift ;;
    --trend-log) TREND_LOG="$2"; shift 2 ;;
    --threshold) echo "mutate: --threshold was removed — the gate is the survivor triage (tools/mutate.cfg [policy])" >&2; exit 2 ;;
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

# --- oracle: the unit binaries of every changed scope dir --------------------------------------
# Unit tests are the oracle: the seeded property tests (tests/property) are too slow per
# mutant at -O0 and assert invariants, not specific values. Names come from CMakeLists.txt so
# a new test_<dir>_* binary joins the oracle without touching this script; a changed dir with
# no unit binary can never kill a mutant, so that is a blind spot and fails here, unbuilt.
CHANGED_DIRS=$(echo "$SCOPE" | cut -d/ -f1 | sort -u)
UNIT_BINS=$(grep -oE 'omgp_add_catch_test\(test_[A-Za-z0-9_]+ +tests/unit/' CMakeLists.txt | sed -E 's/omgp_add_catch_test\(//; s/ .*//')
ORACLE=""
for d in $CHANGED_DIRS; do
  bins=$(echo "$UNIT_BINS" | grep -E "^test_${d}_" || true)
  if [ -z "$bins" ]; then
    echo "mutation: no unit-test oracle for changed dir '$d/' (no test_${d}_* under tests/unit in CMakeLists.txt) — failing (blind spot: its mutants could never be executed)" >&2
    exit 1
  fi
  ORACLE="$ORACLE $bins"
done
ORACLE=$(echo $ORACLE | tr ' ' '\n' | sort -u | tr '\n' ' ')
echo "mutation: oracle: $ORACLE"
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
if [ -n "$REF" ]; then
  echo "mutation: mull $VERSION via $RUNNER (clang $CLANG_MAJOR); gate: max ${MAX_UNLABELLED} unlabelled survivor(s) on changed lines"
else
  echo "mutation: mull $VERSION via $RUNNER (clang $CLANG_MAJOR); whole tree — trend only, no gate"
fi

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
# Library objects only: header-only code (constexpr helpers) is instrumented inside the test
# TUs and shows up in the report, not in this count — the count is a sanity signal, the gate
# is mutate_report.py's blind-spot rule.
embedded=""
for d in $CHANGED_DIRS; do
  n=$(nm "$BUILD/$d"/CMakeFiles/*.dir/*.o 2>/dev/null | grep -c ' t mull_' || true)
  embedded="$embedded $d=${n:-0}"
done
echo "mutation: instrumented build done (mutated functions in library objects:$embedded)"

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

# --- run the oracle binaries under the runner ----------------------------------------------------
# Reporters (mull-runner --help, 0.34.0): IDE, SQLite, GitHubAnnotations, Patches, Elements
# (Mutation Testing Elements JSON), Sarif.
rc=0
REPORTS="$BUILD/reports"
rm -rf "$REPORTS" && mkdir -p "$REPORTS"
EXTRA=()
[ -n "${GITHUB_ACTIONS:-}" ] && EXTRA=(--reporters GitHubAnnotations)
for name in $ORACLE; do
  bin="$BUILD/$name"
  if [ ! -x "$bin" ]; then
    echo "mutation: oracle binary $name was not built — failing (blind spot: CMakeLists.txt names it but the instrumented build did not produce it)" >&2
    rc=1; continue
  fi
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

# --- merge the Elements reports and apply the triage gate (tools/mutate_report.py) ---------------
TREND=()
[ -n "$TREND_LOG" ] && TREND=(--trend-log "$TREND_LOG")
python3 tools/mutate_report.py --reports "$REPORTS" --root "$ROOT" --scope-dirs "$SCOPE_DIRS" \
  --ranges "$BUILD/scope_ranges.json" --ref "$REF" --out "$BUILD/report.json" \
  --max-unlabelled "$MAX_UNLABELLED" --categories "$CATEGORIES" "${TREND[@]}" || rc=1
[ "$rc" -eq 0 ] && echo "mutation: PASS" || echo "mutation: FAIL" >&2
exit $rc
