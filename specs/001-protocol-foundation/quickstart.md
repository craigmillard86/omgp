# Quickstart: proving the Protocol Foundation works

Validation guide for feature 001. Each step names the artefact it exercises and what a
failure looks like, so the evidence discriminates (CLAUDE.md working agreement).

## Prerequisites

```bash
python3 --version                      # ≥3.10 locally, 3.12 in CI
pip install -r tools/requirements.txt  # jinja2, pyyaml, pytest — NOT installed in the WSL sandbox today
cmake --version                        # ≥3.22
clang++ --version                      # ≥14 for fuzzing (optional locally; required in deep-verify)
docker --version                       # for the ESP32-S3 target build
```

## 1. Constants are generated, complete and deterministic (US1)

```bash
python3 tools/codegen.py && sha256sum build/gen/omgp_protocol.h build/gen/omgp_protocol.py > /tmp/a
python3 tools/codegen.py && sha256sum build/gen/omgp_protocol.h build/gen/omgp_protocol.py | diff - /tmp/a && echo DETERMINISTIC
python3 -m pytest -q tools/refimpl/test_codegen.py       # shuffled-YAML determinism, FR-004 conflicts, tables
python3 tools/codegen.py --check-docs                     # exit 0 = docs/protocol-l3.md tables match the YAML
```
Expected: `DETERMINISTIC`; pytest all passed; `--check` exit 0. If determinism were
broken the `diff` prints differing hashes and `DETERMINISTIC` never appears.

## 2. Build + unit/property tests in one invocation (US2, US3)

```bash
./pipeline.sh codegen quality build unit
```
Expected tail:
```
unit: executed <n> check(s) (ctest path)      # n ≥ UNIT_TEST_FLOOR
==> pipeline green
```
`quality` now includes `check_embedded.py` (zero findings). A stale binary cannot
report: build and unit are chained by `pipeline.sh` (`set -e`).

## 3. Golden vectors in both languages

```bash
python3 tools/refimpl/genvectors.py --check              # vectors reproducible from the reference impl
python3 -m pytest -q tools/refimpl                        # Python: every vector encodes/decodes
./build/native/test_l3_payload "[vectors]"                # C++: every vector via omgp_vectors.h
```
Expected: `--check` exit 0; all pytest pass; Catch2 reports `All tests passed` with an
`EXECUTED: <n>` line.

## 4. Differential: C++ vs Python across the corpus (SC-003)

```bash
time python3 tools/diffcheck.py
```
Expected: `diffcheck: <≥11200> cases, C++ and Python agree` in < 2 min. On mismatch the
first differing case prints `(seed, index)`, both outputs, and
`python3 tools/diffcheck.py --index <i>` replays it.

## 5. Descriptor rules at the boundaries (US3)

```bash
./build/native/test_l3_descriptor "[cap]"        # 2048 accepted, 2049 BlobTooLarge
./build/native/test_l3_descriptor "[unknown]"    # type 0x55 skipped, count reported, later records intact
./build/native/test_l3_descriptor "[required]"   # each required type missing → MissingRequired(type)
./build/native/test_l3_descriptor "[strings]"    # 24/25 and 16/17 byte boundaries; invalid UTF-8
printf 'DDEC <hex of tests/vectors/descriptor_sample.json>\nQUIT\n' | ./build/native/l3_helper
```
Expected: the helper prints the canonical descriptor line equal to the vector's
`canonical` field.

## 6. Fuzzing produces clean rejections only — and can fail (US4, SC-004)

```bash
./tools/fuzz-smoke.sh 60                          # local smoke; CI uses 600
```
Expected: four `fuzz: <target> ... findings=0` lines, exit 0.

**Discriminating check** (do not commit): delete the `if (len < 5) return
Status::Truncated;` line in `l3/l3_header.cpp`, rebuild, rerun → expect an ASan
heap/stack-buffer-overflow report, an artefact under `build/fuzz/artifacts/fuzz_header/`
and exit ≠ 0. Restore the line. If the run still said `findings=0`, the harness was not
reaching the decoder and step 6 proves nothing.

## 7. Mutation scoping bites — and is cheap when idle (US4, SC-005)

```bash
./tools/mutate.sh --diff origin/main --dry-run    # print the in-scope files and stop
./tools/mutate.sh --diff origin/main              # local: disclosed-skip if Mull absent
./tools/mutate.sh --diff origin/main --require    # what CI runs: fails if Mull missing
./tools/mutate.sh --diff HEAD                      # clean tree → "nothing in scope" in < 60 s
# Without root: extract the matching .deb (tools/mutate.cfg names it) and point the harness at it
MULL_RUNNER=/path/extracted/usr/bin/mull-runner-14 MULL_PLUGIN=/path/extracted/usr/lib/mull-ir-frontend-14 \
  ./tools/mutate.sh --diff HEAD~1 --require
```
Output ends with `mutation: diff_ref=… mutants=N killed=K survived=S not_covered=C
kill_rate=…% threshold=80%` and one `survivor:` line per surviving mutant
(`file:line:column mutator`). Zero mutants in a non-empty scope is a **failure**
(instrumentation not reaching the code), never a pass.
**Discriminating check** (do not commit): in `l3/l3_payload.cpp` change
`value > LIMIT_param_value_max` to `>=` and temporarily delete the 4095-boundary test →
report lists one survivor, exit 1. Restore both → killed, exit 0.

## 8. Both builds green (rule 10)

```bash
./pipeline.sh                                      # all default stages
./pipeline.sh esp32                                # ESP-IDF component compiles l3/ for ESP32-S3 (Docker)
```
Expected: `==> pipeline green` twice. The ESP-IDF build failing on `l3/` (e.g. an
accidental `<string>` include) is the portability gate; `check_embedded.py` should have
caught it earlier — if it did not, that is a bug in the scan to fix in the same PR.

## 9. Adding an opcode end-to-end (SC-009 walkthrough)

1. Add the opcode + `l3_payloads` entry to `protocol/omgp-protocol.yaml`; update the
   `docs/protocol-l3.md` §3.1 table (same commit — T3).
2. `python3 tools/codegen.py` → constants and tables appear in both outputs.
3. Add the dataclass + codec to `tools/refimpl/omgp_l3.py`; add its vector to
   `genvectors.py`; run `genvectors.py` (commit the new JSON with justification).
4. Write the failing Catch2 test against `omgp::vectors::V_<name>`; implement the C++
   codec in `l3/l3_payload.cpp`; extend `l3_helper` and `diffcheck.py` corpus generator.
5. `./pipeline.sh` green; raise `UNIT_TEST_FLOOR`.
