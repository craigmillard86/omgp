# Implementation Plan: Protocol Foundation

**Branch**: `001-protocol-foundation` | **Date**: 2026-08-28 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/001-protocol-foundation/spec.md`

## Summary

Deliver the L3 protocol foundation as three verified layers: (1) a deterministic
Jinja2-based generator that turns `protocol/omgp-protocol.yaml` into a C++17 constants
header and a Python constants module, including the attribute tables (required/repeated/
max_len, opcode targets) that validators need; (2) an embedded-path C++17 codec library
`omgp_l3` (new top-level `l3/`) for the 5-byte L3 header, every v1 opcode payload, the
status block, and the TLV descriptor — caller-provided buffers, `Status`-returning
functions, no exceptions/RTTI/heap; (3) an independent Python reference implementation
under `tools/refimpl/`, from which the golden vectors are generated once and committed
as immutable JSON. Verification is dual: Catch2 unit/property tests fed by a generated
vectors header, a batch-mode differential check (C++ helper vs Python) over a seeded
corpus of ≥10k messages / ≥1k descriptors plus an invalid-input corpus, real libFuzzer
targets behind `tools/fuzz-smoke.sh`, and Mull incremental mutation behind
`tools/mutate.sh --diff` so the CI `deep-verify` job can finally fail.

## Technical Context

**Language/Version**: C++17 (portable subset for `l3/`: no exceptions, no RTTI, no heap
after init, no OS/wall-clock; enforced by `-fno-exceptions -fno-rtti` on the library
target plus a static scan). Python 3.11+ for tooling and the reference implementation —
**constraint**: the local WSL environment has Python 3.10, so tooling MUST avoid
3.11-only syntax/stdlib (`tomllib`, `except*`, `typing.Self`) or the bootstrap path
breaks; CI runs 3.12.

**Primary Dependencies**:
- Build: CMake ≥3.22 with presets (`native` exists; add `fuzz`); GCC 11 / Clang 14+
  natively; ESP-IDF v5.3 via `espressif/idf` Docker for the ESP32-S3 target (existing
  `esp32` pipeline stage).
- Codegen: PyYAML (present), **Jinja2** (already in `tools/requirements.txt`, NOT
  installed locally — quickstart step).
- Test framework: **Catch2 v3**, vendored amalgamated (`third_party/catch2/`), BSL-1.0
  licence — approved by human ruling 2026-08-28 (OPEN-QUESTIONS), lands as its own T2
  slice (spec FR-032).
- Mutation: **Mull** (Apache-2.0, LLVM-based, incremental via git diff) — same ruling;
  installed in CI `deep-verify` only; `mutate.sh` discloses and passes locally when
  absent, but **fails** under `--require` (used by CI) so the gate bites.
- Fuzzing: libFuzzer from clang's compiler-rt — verified linking locally (clang 14),
  present in CI (`deep-verify` installs `clang`); not a new package.
- Python tests: pytest (in requirements.txt, not installed locally).

**Storage**: files only — YAML in, generated sources out (`build/gen/`), golden vectors as
JSON under `tests/vectors/`, fuzz seed corpus derived from vectors at build time.

**Testing**: Catch2 (unit + property, native with ASan/UBSan); pytest (reference
implementation); `tools/diffcheck.py` (differential, batch-mode C++ helper); libFuzzer
targets (`tests/fuzz/`); Mull (`tools/mutate.sh --diff`); static embedded-constraint scan
(`tools/check_embedded.py`) in the `quality` stage; determinism test for codegen.

**Target Platform**: Linux native (dev + CI) and ESP32-S3 (Xtensa LX7, ESP-IDF v5.3)
compile of `l3/` as an IDF component. **Conflict recorded**: the plan input asked for an
`arm-none-eabi-gcc` compile-only check; CLAUDE.md rule 10 and constitution Principle IV
forbid a generic ARM toolchain for this target. Not adopted — see research.md R-01.

**Project Type**: embedded-portable library + host-only tooling (single repository,
existing layout extended with `l3/`, `third_party/`, `tests/fuzz/`, `tests/property/`,
`tests/vectors/`).

**Performance Goals**: none functional beyond spec SC-003 (differential corpus < 2 min on
CI) and SC-005 (mutation stage < 60 s when nothing is in scope). Codec throughput is not
a target for this feature.

**Constraints**: `l3/` compiles with `-fno-exceptions -fno-rtti -Wall -Wextra -Werror`;
zero heap after init (static scan + link check); all protocol values come from
`build/gen/omgp_protocol.h` (FR-005 scan); descriptor parse is streaming (O(1) memory,
no fixed record-count caps); both builds green; `UNIT_TEST_FLOOR` raised.

**Scale/Scope**: 13 opcodes (+ status block, ERROR), 14 TLV record types, ~30 golden
vectors, ≥11k differential cases, 4 fuzz targets. Expected ~2.5k lines C++, ~1k Python,
~400 lines templates/tooling.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Gate | Status |
|---|---|---|
| I Spec-first | All constants via codegen; FR-008/FR-034 layouts enter YAML + `docs/protocol-l3.md` §3.1 **before** codec code depends on them; discrepancies found during implementation go to OPEN-QUESTIONS, not code | PASS — requires a T3 slice (YAML + docs table + CLAUDE.md layout line + risk-score regex) sequenced first |
| II Test-first | Vectors generated by the reference implementation and committed first; every codec lands with failing Catch2 tests + pytest first; bug fixes with a reproducing case | PASS |
| III Dual verification | Independent Python reference (written from docs, not transliterated — enforced by task ordering: Python first, C++ author works from spec + vectors); differential corpus; libFuzzer on every decoder | PASS |
| IV Portability & determinism | `l3/` is C++17, `-fno-exceptions -fno-rtti`, caller buffers, `Status` returns, no time; native ASan/UBSan + ESP-IDF component build both in pipeline | PASS — ESP-IDF, not arm-none-eabi (R-01) |
| V Idempotency | Codecs only; SET_PARAM/SET_BYPASS encode absolute values; encoder rejects out-of-range rather than clamping | PASS |
| VI Simulator | Out of scope; nothing here touches transport/scenario runner | N/A |
| VII Bridge discipline | Out of scope | N/A |
| VIII Minimal dependencies | Catch2 (BSL-1.0) + Mull (Apache-2.0) approved by ruling; Jinja2 (BSD-3) already listed; no new host-core dependency; no JSON parser in C++ (canonical text instead) | PASS — see Complexity Tracking |
| IX Traceability | Every `l3/` file cites §; every vector carries `spec_ref`; commits `Refs #n` | PASS |

**Post-design re-check (after Phase 1)**: unchanged — the streaming descriptor API (R-06)
removed the only design pressure toward heap or large fixed arrays; the canonical-text
differential format (R-07) removed the pressure toward a C++ JSON dependency.

## Project Structure

### Documentation (this feature)

```text
specs/001-protocol-foundation/
├── plan.md              # This file
├── research.md          # Phase 0: decisions R-01..R-12
├── data-model.md        # Phase 1: wire layouts, records, error kinds, validation rules
├── quickstart.md        # Phase 1: how to prove it works end-to-end
├── contracts/
│   ├── l3-codec-cpp.md          # C++ API surface of omgp_l3
│   ├── l3-codec-python.md       # Python reference API
│   ├── generated-constants.md   # what codegen emits, CLI, determinism contract
│   ├── canonical-text.md        # key=value line format shared by helper/diffcheck/vectors
│   ├── golden-vector.schema.json# tests/vectors/*.json schema
│   └── tooling.md               # fuzz-smoke.sh, mutate.sh, l3_helper, check_embedded, pipeline changes
└── tasks.md             # Phase 2 (/speckit-tasks) — not created here
```

### Source Code (repository root)

```text
protocol/
├── omgp-protocol.yaml            # source of truth (+ FR-008 layouts, descriptor.crc, EVT_NONE)
└── templates/                    # NEW: Jinja2
    ├── omgp_protocol.h.j2
    ├── omgp_protocol.py.j2
    └── omgp_vectors.h.j2         # golden vectors → C++ header

l3/                               # NEW top-level: L3 codec library `omgp_l3` (embedded path)
├── l3_types.hpp                  # Header, Status, payload structs (POD, fixed size)
├── l3_header.hpp/.cpp            # §3 common header encode/decode
├── l3_payload.hpp/.cpp           # §3.1 per-opcode payload codecs + status block + ERROR
├── l3_descriptor.hpp/.cpp        # §4 RecordCursor, typed record decode, DescriptorWriter, validate
├── l3_utf8.hpp                   # UTF-8 well-formedness check (no tables on heap)
└── CMakeLists.txt                # add_library(omgp_l3) -fno-exceptions -fno-rtti

link/crc16.hpp                    # unchanged; reused by descriptor_crc()

third_party/catch2/               # NEW (T2 dependency slice): amalgamated Catch2 v3 + LICENSE.txt + VERSION
tools/
├── codegen.py                    # rewritten on Jinja2; --out, --yaml, --check, --vectors
├── diffcheck.py                  # extended: batch mode, message + descriptor + invalid corpora
├── l3_helper.cpp                 # host-only CLI (stdin lines → stdout lines) for diffcheck
├── check_embedded.py             # NEW: static scan (heap/exceptions/RTTI/protocol literals)
├── fuzz-smoke.sh                 # real: builds `fuzz` preset, runs each target, non-zero on finding
├── mutate.sh                     # real: Mull incremental (--diff <ref>), --require for CI
├── requirements.txt              # unchanged (jinja2, pyyaml, pytest)
└── refimpl/
    ├── omgp_crc.py               # existing
    ├── omgp_l3.py                # NEW: header + payload codecs
    ├── omgp_descriptor.py        # NEW: TLV parse/serialise/validate + descriptor_crc
    ├── canonical.py              # NEW: canonical-text ⇄ typed conversion
    ├── genvectors.py             # NEW: writes tests/vectors/*.json (run once per vector change)
    └── test_*.py                 # pytest

tests/
├── unit/
│   ├── test_smoke.cpp            # kept; EXECUTED contract preserved
│   ├── test_l3_header.cpp        # Catch2
│   ├── test_l3_payload.cpp
│   ├── test_l3_descriptor.cpp
│   └── catch_listener.cpp        # prints "EXECUTED: <n>" (assertion total) for the floor
├── property/
│   └── test_l3_roundtrip.cpp     # seeded generators: encode→decode→encode identity, invalid→Status
├── fuzz/
│   ├── fuzz_header.cpp, fuzz_payload.cpp, fuzz_descriptor.cpp, fuzz_roundtrip.cpp
│   └── CMakeLists.txt            # only when OMGP_FUZZ=ON (clang)
└── vectors/                      # NEW, immutable: one JSON per vector (schema in contracts/)
    ├── msg_ping_req.json … msg_error_resp.json
    ├── status_block.json
    └── descriptor_sample.json

esp32-host/
├── CMakeLists.txt                # runs codegen before IDF configure
└── components/omgp_l3/CMakeLists.txt  # NEW: idf_component_register(SRCS ../../l3/*.cpp ...)
    main/l3_smoke.cpp             # NEW: references encode/decode so the component links

CMakeLists.txt, CMakePresets.json # omgp_l3 target, Catch2 target, tests, l3_helper, `fuzz` preset
pipeline.sh                       # quality: check_embedded; unit: Catch2 binaries; refimpl: pytest;
                                  # UNIT_TEST_FLOOR raised
.github/workflows/ci.yml          # deep-verify: install Mull, `mutate.sh --diff origin/main --require`
.github/workflows/risk-score.yml  # add l3/ to the T2 "portable protocol-critical code" regex
CLAUDE.md                         # repo layout gains l3/; rule 5 names l3/ alongside core/ link/
docs/protocol-l3.md               # §3.1 table gains the ruled response layouts (T3, with YAML)
```

**Structure Decision**: the codec library gets its own top-level `l3/` rather than living
under `protocol/` (every path under `protocol/` scores T3 in `risk-score.yml`, which would
make routine codec work un-dispatchable), `link/` (L2 must stay opaque to L3) or `core/`
(the module-side SDK needs the codec without host-core). `l3/` mirrors the spec's layering
next to `link/` (L2). It is embedded-path code and inherits CLAUDE.md rule 5; that rule,
the repo-layout block, and the risk-score T2 regex are updated in the feature's T3 slice.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| New top-level directory `l3/` + governance edits (CLAUDE.md layout/rule 5, risk-score regex) | Codec must be reusable by host-core, simulator and SDK without pulling `core/`; must not score T3 on every change | Placing it under `protocol/` makes all codec PRs T3 (agents cannot deliver them); under `core/` couples the SDK to host-core; under `link/` violates the L2/L3 opacity invariant |
| Two new test-only dependencies (Catch2, Mull) | Human ruling 2026-08-28 chose open-source frameworks over repo-local tooling; a codec suite of hundreds of cases outgrows the `CHECK` macro; Mull's incremental mode is what makes `--diff` scoping real | Hand-rolled harness + regex mutator was offered and declined; universalmutator (Python) lacks diff scoping and produces weak mutants (R-04) |
| Third generated artefact (`omgp_vectors.h`) | Keeps a JSON parser out of C++ while letting C++ tests consume the same immutable vectors as Python (clarification Q4) | Hand-copying vectors into C++ tests duplicates immutable evidence and invites drift; a C++ JSON library is an unapproved dependency |
