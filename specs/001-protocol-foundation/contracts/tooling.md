# Contract: tooling, scripts and pipeline changes

Every fast/partial path states its blind spot on stdout (CLAUDE.md working agreement).

## `tools/fuzz-smoke.sh <seconds>`

```
./tools/fuzz-smoke.sh 600
```
1. Requires `clang++` with `-fsanitize=fuzzer`; if absent → prints
   `fuzz: clang with libFuzzer not found — cannot run (blind spot: no fuzzing in this environment)`
   and exits **1** (this script is only invoked where fuzzing is required).
2. `cmake --preset fuzz && cmake --build --preset fuzz` (targets in `tests/fuzz/`).
3. Builds seed corpora `build/fuzz/corpus/<target>/` from `tests/vectors/*.json` bytes
   (messages → header/payload/roundtrip; descriptor → descriptor).
4. Runs each of the 4 targets with `-max_total_time=$((seconds/4)) -timeout=5
   -rss_limit_mb=512 -artifact_prefix=build/fuzz/artifacts/<target>/`.
5. Prints per-target `fuzz: <target> runs=<n> cov=<n> findings=<n>`; exits non-zero if
   any target exits non-zero or any artifact was written; lists reproducer commands
   (`build/fuzz/<target> <artifact>`).

## `tools/mutate.sh [--diff <ref>] [--require] [--dry-run] [--trend-log <file>]`

```
./tools/mutate.sh --diff origin/main --require      # CI deep-verify
./tools/mutate.sh --diff HEAD~1                      # local
./tools/mutate.sh --trend-log metrics/mutation-trend.jsonl   # nightly: whole tree, trend only
```

**Gate (ruling 2026-08-29, docs/OPEN-QUESTIONS.md — supersedes the percentage threshold):**
every surviving mutant on a changed line is triaged into exactly one of (a) missing test —
the killing test is written; (b) equivalent — labelled on its source line
`// mutant-ok(equivalent): <one line>`; (c) accepted — error-path minutiae not worth a test,
`// mutant-ok(accepted): <one line>`. A label may name mutators
(`mutant-ok(accepted, cxx_gt_to_ge): …`) to cover only those on the line; a label on a
comment-only line governs the line below it (clang-format reflows long trailing comments).
Diff-scoped runs exit 1 on any unlabelled survivor (`max_unlabelled_survivors = 0`,
`label_categories`, both T3 in `tools/mutate.cfg`); whole-tree runs print
`mutation-trend: {json}` (appended to `--trend-log` when given) and never gate on the score.
A malformed label (unknown category, empty justification) fails every mode; a label that
covers no surviving mutant is reported as a *stale label* warning. The merge and gate live
in `tools/mutate_report.py` (tested on synthetic Elements reports in
`tools/refimpl/test_tooling.py`). `--threshold` is rejected with exit 2.
1. `--diff <ref>` must name a commit in the clone; an `origin/<branch>` ref missing from a
   shallow checkout is fetched with `--depth=1` first (a two-tree `git diff` needs no
   merge base), and only if that fails is it an error (exit 2 — never a silent empty
   scope). Scope = files under `l3/ link/ core/` changed relative to `<ref>`; no scope →
   `mutation: nothing in scope (<ref>)`, exit 0, < 60 s. `--dry-run` prints the scope and
   stops.
2. Tool check: `mull-runner-<N>` and `/usr/lib/mull-ir-frontend-<N>` for the clang major
   in use (`tools/mutate.cfg` pins the Mull version; `MULL_RUNNER`/`MULL_PLUGIN` override
   the paths, e.g. for an extracted package). Absent → with `--require` exit 1 and
   `mutation: mull <ver> required but not found`; without → exit 0 after
   `mutation: mull not present — skipped (blind spot: …)`.
3. `build/mutate/mull.yml` holds mutators + timeout only and is written **before** a fresh
   instrumented clang build (`-fpass-plugin=… -g -grecord-command-line -O0`, sanitizers
   off): the IR frontend reads it at compile time, every translation unit that is built is
   instrumented (any include/exclude path filter *at compile time* removes Catch2's `main()`
   TU and with it the run-time mutant dispatch — measured on 0.34.0), and Mull's `gitDiffRef`
   is not used because it drops all mutants in files the diff *adds*. The build is limited to
   the oracle targets (`cmake --build --target <oracle…>`, Ninja when present): nothing else
   runs under the runner. Before the runner starts the file is rewritten (`mutate.sh
   --print-phase2-config` shows it) with the same mutators, the timeout, `quiet: true` and
   `includePaths` for `scope_dirs` (`^<root>/<dir>/.*`), so only scope-dir mutants are
   *executed*; the runner captures no test output (`--no-output`; kills are exit-status);
   the merge in step 4 is unchanged and still the gate. (Amended 2026-09-06, gate-budget PR —
   pending a ruling, see `docs/OPEN-QUESTIONS.md` 2026-09-06 "Mull path filters are safe at
   RUN time"; `tools/mutate_diff_reports.py` compares two Elements reports mutant by mutant
   and is the evidence tool for it.)
4. Runs the three unit binaries (`test_l3_header/payload/descriptor` — the property
   binaries are too slow per mutant at -O0) under the runner with `--workers $(nproc)`
   and the `IDE` + `Elements` reporters (+ `GitHubAnnotations` under CI); merges the
   Elements JSON reports by mutant identity (killed if any binary kills it) and keeps
   only mutants under `scope_dirs` on lines `git diff -U0 <ref>` added/changed (whole
   file for new files; everything in scope when `--diff` is omitted).
5. Prints the Mutation Report (data-model §7) and writes `build/mutate/report.json`:
   summary line `mutation: mode=diff|trend diff_ref=… mutants=N killed=K survived=S
   not_covered=C kill_rate=…% labelled[equivalent=… accepted=…] unlabelled=U
   max_unlabelled=0`, one `UNLABELLED survivor: file:line:col mutator` line each, one
   `labelled (<category>): … — <justification>` line each, stale-label warnings.
6. Exit 1 if any runner failed, if no report was produced, if the scope is non-empty but
   zero mutants were generated, if any label is malformed, or — with `--diff` only — if
   `unlabelled > max_unlabelled_survivors`.

## `tools/l3_helper` (built by CMake and by the bootstrap g++ path)

Line protocol in `canonical-text.md`. Host-only (`tools/`), may use the full language
(reads stdin with `std::getline`); links `omgp_l3`.

## `tools/diffcheck.py`

Keeps the existing CRC differential (200 cases via `crc_helper`) and adds, through one
long-lived `l3_helper` process: valid-message corpus (≥10k), descriptor corpus (≥1k),
invalid corpus. Prints `diffcheck: <n> cases, C++ and Python agree` or exits 1 with the
first mismatch: `(seed, index)`, request line, C++ line, Python line. Fixed seed
`0xB0071E`; `--seed` and `--index` replay one case; `--count` scales the corpus.

## `tools/check_embedded.py`

```
python3 tools/check_embedded.py [--dirs l3 link core] [--yaml protocol/omgp-protocol.yaml]
```
Exit 1 listing `file:line: <finding>` for forbidden constructs and protocol-literal
duplicates (research R-10). Escape hatch: `// literal-ok: <reason>` on the same line.
Runs in the `quality` stage on every path (pure Python, no build needed).

## `pipeline.sh` changes

- `quality`: add `python3 tools/check_embedded.py`.
- `build` (bootstrap): compile `third_party/catch2/catch_amalgamated.cpp` once to an
  object, then each `tests/unit/*.cpp` + `tests/property/*.cpp` with `l3/*.cpp`,
  `l3_helper`, `crc_helper`; disclosure line unchanged. (Amended by #133: the source
  list is `unit_sources()` — every `test_*.cpp` under those two directories at any depth,
  sorted, symlinks not followed and refused by name (red team @8b0e4f4), a directory the
  walk cannot enter refused by name (red team @3713ab0) — shared with the bootstrap `unit`
  walk.)
- `unit`: run every test binary; the `EXECUTED: <n>` lines are summed (ctest path via
  `LastTest.log`, bootstrap via stdout); `UNIT_TEST_FLOOR` raised to the new total −
  small slack (documented in the commit that raises it: "raise when tests are added;
  NEVER lower"). (Amended by #133: the floor is the COUNT gate only. The ctest path also
  runs `tools/check_test_set.py`, which proves from `compile_commands.json`, the object
  files, `ctest --show-only` and the run's `ctest --output-junit` record
  (`build/native/Testing/junit.xml`, deleted before ctest runs and refused if older than
  any registered binary) that every `test_*.cpp` under `tests/unit` and `tests/property`
  at any depth (red team @ceab86f) — reached without following symlinks, and a symlink
  anywhere under those directories, file or directory, is refused by name, since `rglob`
  and `find` do not descend one and a source behind one was silently outside the set (red
  team @8b0e4f4), and so is a directory the walk cannot enter — `os.walk` otherwise swallows
  the error and omits it, so a mode-000 `tests/unit/deep` hid its source before and after
  the run alike (red team @3713ab0), and a mode-400 one — listable, not searchable — was a
  traceback rather than a name (red team @4b78242); with links and walk errors refused the
  walked set is the whole tree by construction — the `EXECUTED: <n>` line must stand alone (anchored
  both ends; a line merely containing the marker is not the run's count) —
  was compiled, registered and executed, naming every one
  that was not — compiled means exactly ONE `compile_commands.json` entry (two entries
  under two targets are refused naming both: keeping the last one let a single appended
  entry re-credit a source to any registered target and made the verdict depend on entry
  order — red team @8b0e4f4) whose object exists, is
  no older than the source (an edited-after-build source is named; an mtime comparison is
  a control, not a guarantee) and is on the target's link line
  (`CMakeFiles/<target>.dir/link.txt`, the Makefiles generator's, read as a build artefact;
  absent, the check fails closed — an entry plus a stub object is not a compilation into
  the binary, red team @8b0e4f4), registered means an `add_test` whose command is exactly
  the target's own binary — compared as an absolute path without resolving symlinks, so a
  link to another target's binary is not a registration, and the file a registration runs
  must be registered once (a link registered by its own path is refused, both ways; red
  team @3dde163; the same path registered twice likewise — red team @2f40596), one ctest test NAME must register one binary (execution evidence is
  keyed by name; CMake allows a duplicate across `add_subdirectory` — both refused; red
  team @aed9693), and the registered set must equal the record's `<testcase>` names
  (`ctest --show-only` runs after the tests; a test that rewrote `CTestTestfile.cmake`
  is refused as "registration set differs from the run's record" — a control, not a
  guarantee) — with no arguments (a filtered Catch2 run, or a same-named binary
  elsewhere, is refused); and all of that evidence must be UNCHANGED by the run: the
  stage takes `check_test_set.py --snapshot` before ctest (the source set with each
  source's dev/ino/size/mtime/ctime; sha256 of `compile_commands.json`; the same identity
  of every source's object(s), link line(s) and every registered binary; the registration list) and hands
  it to `--ctest --pre` on stdin — never via a file under `build/` — so a test that
  writes a compile entry, an object, a link line, a registration or a binary while running is refused
  as "changed during the ctest run", and one that deletes or adds a test source while
  running as "source set changed during the ctest run" — the check walks the sources
  after ctest, so without the set in the snapshot a built-but-unregistered source was
  verified away by deleting it mid-run (red team @2f40596)
  (red team @f8bce32; the identity is a control, not a guarantee — ctime is what a
  size-and-mtime-preserving rewrite cannot restore without root or a moved clock; a test
  that reaches the shell's memory or rewrites the tool or `pipeline.sh` is outside what
  reading artefacts can establish, stated). Executed means an `EXECUTED: <n>` line with n > 0 (red team
  @94f2462) for the BINARY that contains the source's object — per-binary evidence, not
  proof that any one source's cases ran; one source per target in `CMakeLists.txt` makes
  the two coincide, a control on the build files, not a guarantee — ctest is run with
  `--test-output-size-passed 10000000` because its default keeps only the first 1024 bytes
  of a passing test's stdout, dropping the trailing `EXECUTED:` line, and the tool names a
  record truncated that way (red team @3880d35); the bootstrap path walks the same
  `unit_sources()` list, so a source with no binary fails by name, and applies the same
  execution predicate: a binary that exits 0 with no `EXECUTED:` line, or `EXECUTED: 0`,
  is named and fails the stage (red team @ceab86f); a source newer than its binary is
  named (`rebuild before verifying`; a control, not a guarantee); the source list, every
  binary's existence and freshness and its dev/ino/size/mtime/ctime are gathered BEFORE
  any binary runs, and the list and identities must be unchanged after the run — a
  binary is arbitrary code, so gathering source n+1's evidence after source n's binary
  ran let an earlier test mint or overwrite a later source's binary (red team @2f40596;
  identity by stat is the same control as the ctest path's); and because that path
  builds one binary per BASENAME, two sources sharing a basename are refused by name
  before the bootstrap build and before the walk (red team @3dde163 — otherwise one
  binary would vouch for both and count twice), and a symlink under the source
  directories is refused there too (red team @8b0e4f4), as is a directory `find` cannot
  enter (red team @3713ab0; `find -P` already exited 1 on it, but named by find, not the
  stage). Both paths print a `unit: verified N
  test binaries` line (N = distinct binaries on the ctest path; sources, which the
  uniqueness check makes one-per-binary, on the bootstrap path), and both fail when
  there are no sources at all. The record, not
  `LastTest.log`, is the execution evidence: the log interleaves the tests' own stdout
  with ctest's framing (PR #172 red team). The two paths have different predicates by
  design — ctest runs what cmake registered and the tool proves the sources are all in
  that set; bootstrap runs one binary per source that its own `stage_build` made — and
  a pipeline run never mixes them: a cmake tree without `CTestTestfile.cmake` takes the
  bootstrap path and fails at the first source with no bootstrap binary.)
- `refimpl`: `python3 -m pytest -q -rs tools/refimpl`. Ordering dependency (#133):
  `tools/refimpl/test_test_set_gate.py`'s two real-tree controls read `build/native`'s
  artefacts and the JUnit record the same invocation's `unit` stage wrote, so `refimpl`
  must follow `build unit` (the default list and `ci.yml`'s explicit list both do);
  without them the controls skip locally and fail on CI (`GITHUB_ACTIONS`).
- `codegen`: `python3 tools/codegen.py --vectors tests/vectors && python3 tools/codegen.py --check-docs`.
- `esp32`: calls `stage_codegen` first so `build/gen/` exists on the host before the
  Docker build (the IDF image has no Jinja2).
- New optional stage `fuzz` (`./pipeline.sh fuzz` → `tools/fuzz-smoke.sh 60`) — not in
  the default stage list.

## CMake

- `add_library(omgp_l3 STATIC l3/*.cpp)`; `target_compile_options(omgp_l3 PRIVATE
  -fno-exceptions -fno-rtti)`; `target_include_directories(build/gen, .)`.
- `add_library(catch2_amalgamated STATIC third_party/catch2/catch_amalgamated.cpp)`.
- Test executables: `test_smoke` (kept), `test_l3_header`, `test_l3_payload`,
  `test_l3_descriptor`, `test_l3_roundtrip` each `add_test`, all linking
  `tests/support/*.cpp` (Catch2 listener, counting `__wrap_malloc` heap guard) with
  `-Wl,--wrap=malloc`.
- `CMakePresets.json`: add `fuzz` configure/build presets (clang, `OMGP_FUZZ=ON`).
- `esp32-host/CMakeLists.txt`: `message(FATAL_ERROR "run ./pipeline.sh codegen first")`
  if `../build/gen/omgp_protocol.h` is missing (codegen runs on the host, never inside
  the IDF container); component `components/omgp_l3` uses
  `idf_component_register(SRC_DIRS ../../../l3 INCLUDE_DIRS ../../../l3 ../../../build/gen)`
  (three levels up is the repo root; `SRCS` does not glob) with
  `-fno-exceptions -fno-rtti`; `main/l3_smoke.cpp` exercises encode/decode.

## CI (`.github/workflows/ci.yml`, T3)

Three separate human PRs, each landing only after the tool it enables exists (a
workflow edit inside an agent PR makes the whole PR T3 — see tasks.md PR boundaries):
1. Phase 1 (T006): `esp32` job gains `actions/setup-python`, `pip install -r
   tools/requirements.txt` and `./pipeline.sh codegen` before `./pipeline.sh esp32`.
2. After US1 merges (T027 follow-up): codegen drift-guard step runs
   `python3 tools/codegen.py --check-docs` after codegen.
3. After T053–T061 merge (T062): `deep-verify` installs pinned Mull for the runner's
   clang and switches to `./tools/mutate.sh --diff origin/main --require`
   (`fuzz-smoke.sh 600` is already called).
`native` job unchanged (pipeline does the rest). `risk-score.yml`: add `l3\/` to the T2
regex. `CLAUDE.md`: layout + rule 5 mention `l3/`.

## Evidence the gate bites (quickstart proves both)

- Fuzz: remove the `len < 5` check in `decode_header`, run `fuzz-smoke.sh 60` →
  ASan report + non-zero exit. If the output were the same as with the check present,
  the harness would not be reaching the decoder — that is the discriminating test.
- Mutation: change `value > 4095` to `value >= 4095` in `encode_set_param` without a
  test for 4095 → survivor listed, exit 1; add the boundary test → killed, exit 0.
