# Quickstart: proving the trunk link layer works (feature 002)

Runnable validation scenarios, one per user story plus the gates. Each states what the
output would be if the claim were false (CLAUDE.md working agreement). Prerequisites and
build presets are those of feature 001 (`quickstart.md` there): Python 3.10+, Jinja2,
PyYAML, pytest; CMake ≥ 3.22; clang for fuzzing; Docker for the ESP32 stage.

## 1. Build + every unit/property test in one invocation (all stories)

```bash
./pipeline.sh codegen quality build unit
```
Expected: `unit: executed <n> check(s)` with `<n>` above the raised `UNIT_TEST_FLOOR`, the
six `test_link_*` and two property binaries listed by ctest, `==> pipeline green`. If the
`link/` sources were not part of the build, the `test_link_*` binaries would be absent
from the ctest list and the floor would fail.

## 2. Frames survive a hostile wire (US1)

```bash
./build/native/test_link_frame "[vectors]"          # every frame_* vector byte-for-byte both ways
./build/native/test_link_resync                      # random cut points / corruptions
python3 tools/diffcheck.py --frames-only             # C++ vs Python: ≥10k frames + torture corpus
```
Expected: `diffcheck: frames 10000, torture <m> (≥1000 per class), C++ and Python agree`
in < 60 s. **Discriminating check** (do not commit): in `link/frame.cpp` make `BadEscape`
tolerate one violation → the torture run reports the first mismatch with its `(seed,
index)`, the stream, the C++ and the Python line; the Python side is the reference, so
the C++ change is the defect.

## 3. Host polls, retries, never double-applies (US2, US3, SC-004)

```bash
./build/native/test_link_loop -s                     # Master ↔ Responder over MockWire, verbose
```
Expected: for each of drop / duplicate / delay / corrupt in each of positions {first
attempt, retry 1, retry 2, after give-up}: `handler invocations == 1` per new sequence,
`transmissions ≤ 3`, and the accepted response's `seq` equals the transaction's. A wrong
implementation shows as `handler invocations == 2` (double-apply) or a response
attributed to the next transaction.

## 4. Every §9 timing symbol has a boundary test (SC-001)

```bash
python3 -m pytest -q tools/refimpl/test_timing_map.py -s
```
Expected: the printed map lists all nine symbols with ≥ 1 test each, e.g.
`T_resp → "response at T_resp-1 accepted, at T_resp missed"`. Rename one tag and the test
fails naming that symbol. Change `T_resp_us` in the YAML to 199 and run `./pipeline.sh
codegen build unit` → the `[timing:T_resp]` test fails (the value is read from the
generated header, not restated).

## 5. Node health state machine (US4)

```bash
./build/native/test_link_health "[timing:T_poll]"    # SUSPECT polled once per 10×T_poll
./build/native/test_link_health                      # all transitions + boundaries (2 failures, 999 ms)
```
Expected: one notification per transition, none at the boundaries; `poll_due` false for a
SUSPECT node at 9×T_poll and true at 10×T_poll.

## 6. Dead bus vs dead node (US5)

```bash
./build/native/test_link_busfault -s
```
Expected: with three enrolled nodes all SUSPECT → exactly one `BUS_FAULT` + one `ALERT`;
`next_probe()` rates alternate `115200, 1000000, 115200, …`; a `Rate` step making one node
hear only 115.2 kbit/s → the fault clears once on that node's answer and `bit_rate()` is
115200; with a single enrolled node the same sequence declares the fault (ruling Q2); with
two nodes of which one stays ENROLLED, nothing is declared.

## 7. Fuzzing produces clean rejections only — and can fail (SC-003)

```bash
./tools/fuzz-smoke.sh 60
```
Expected: `fuzz: fuzz_frame runs=<n> cov=<c> findings=0 exit=0`. **Discriminating check**
(do not commit): remove the `TooLong` guard in `Deframer::feed` → ASan
`stack-buffer-overflow` at `frame.cpp:<line>`, `findings=1`, exit 1; restore → 0.

## 8. Mutation triage gate bites (FR-035)

```bash
./tools/mutate.sh --diff origin/main --require       # CI form (Mull needed; local: MULL_RUNNER/MULL_PLUGIN)
```
Expected: `mutation: mode=diff … unlabelled=0`, `mutation: PASS`. **Discriminating check**:
change `attempt < TRUNK_retries` to `<=` in `link/master.cpp` and delete the "exactly two
retries" assertion → `UNLABELLED survivor: link/master.cpp:<line>:<col> cxx_lt_to_le`,
`mutation: FAIL`, exit 1; restore → PASS.

## 9. Both builds green (rule 10)

```bash
./pipeline.sh                                        # all default stages
./pipeline.sh esp32                                  # IDF component omgp_link links link_smoke.cpp
```
Expected: `==> pipeline green` twice. An accidental `<chrono>` or `new` in `link/` fails
`quality` (`check_embedded.py`) before either build runs.

## 10. Embedded-path scan covers `link/` (FR-028, FR-034)

```bash
python3 tools/check_embedded.py
```
Expected: no findings. Add `const int t = 200;` to `link/master.cpp` → `link/master.cpp:<n>:
protocol literal 200 (TRUNK_T_resp_us)`; remove the `// trunk §` citation from a `link/`
file → `no spec citation (expected a trunk §… comment)`.
