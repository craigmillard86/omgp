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

## `tools/mutate.sh [--diff <ref>] [--require] [--threshold <pct>] [--dry-run]`

```
./tools/mutate.sh --diff origin/main --require      # CI deep-verify
./tools/mutate.sh --diff HEAD~1                      # local
```
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
   off): the IR frontend reads it at compile time, every translation unit is instrumented
   (any include/exclude path filter removes Catch2's `main()` TU and with it the run-time
   mutant dispatch — measured on 0.34.0), and Mull's `gitDiffRef` is not used because it
   drops all mutants in files the diff *adds*.
4. Runs the three unit binaries (`test_l3_header/payload/descriptor` — the property
   binaries are too slow per mutant at -O0) under the runner with `--workers $(nproc)`
   and the `IDE` + `Elements` reporters (+ `GitHubAnnotations` under CI); merges the
   Elements JSON reports by mutant identity (killed if any binary kills it) and keeps
   only mutants under `scope_dirs` on lines `git diff -U0 <ref>` added/changed (whole
   file for new files; everything in scope when `--diff` is omitted).
5. Prints the Mutation Report (data-model §7) and writes `build/mutate/report.json`.
6. Exit 1 if any runner failed, if no report was produced, if the scope is non-empty but
   zero mutants were generated, or if `kill_rate < threshold` (`tools/mutate.cfg`, 80).

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
  `l3_helper`, `crc_helper`; disclosure line unchanged.
- `unit`: run every test binary; the `EXECUTED: <n>` lines are summed (ctest path via
  `LastTest.log`, bootstrap via stdout); `UNIT_TEST_FLOOR` raised to the new total −
  small slack (documented in the commit that raises it: "raise when tests are added;
  NEVER lower").
- `refimpl`: `python3 -m pytest -q tools/refimpl`.
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
