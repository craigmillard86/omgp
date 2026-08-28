# Research: Protocol Foundation

**Feature**: 001-protocol-foundation | **Date**: 2026-08-28

Each decision is labelled per CLAUDE.md rule 11: what makes it true is either *by
construction* (language/structure), *demonstrated* (a named check run in this session),
or *assumed* (to be verified at implementation). No unresolved NEEDS CLARIFICATION
remains from the Technical Context.

## R-01 — Embedded target check: ESP-IDF component, not arm-none-eabi

- **Decision**: compile `l3/` inside the existing ESP-IDF build (`esp32-host/` gains a
  component `omgp_l3`; `esp32-host/main/l3_smoke.cpp` references the API so the linker
  keeps it). The pipeline `esp32` stage (Docker `espressif/idf:v5.3`) is the target
  build. Additionally the native `omgp_l3` target compiles with
  `-fno-exceptions -fno-rtti` so the portable-subset constraint is checked on every
  local build, not only in Docker. Codegen runs on the **host** before the Docker
  build (`pipeline.sh stage_esp32` calls `stage_codegen`; the CI `esp32` job installs
  the Python deps and runs `./pipeline.sh codegen` first) — the `espressif/idf` image
  has no Jinja2, so the IDF CMake only asserts `build/gen/omgp_protocol.h` exists.
- **Rationale**: the plan input asked for "esp32 cross-compile check using
  arm-none-eabi-gcc, compile-only". The ESP32-S3 is Xtensa LX7; CLAUDE.md rule 10 says
  the target build uses ESP-IDF "never a generic ARM toolchain", and constitution
  Principle IV names ESP-IDF. An ARM compile would prove portability to a different
  architecture, not that the real target builds — and would add a toolchain dependency
  (`arm-none-eabi-gcc` is not present locally either). Documents win over the prompt;
  the conflict is surfaced here and in the plan for a human to override if a
  portability-only ARM check is genuinely wanted as an *extra*.
- **Alternatives considered**: arm-none-eabi-gcc compile-only (rejected: rule 10);
  skipping the target build for this feature (rejected: rule 10 "both builds must stay
  green").
- **Label**: rule conflict *demonstrated* by reading CLAUDE.md rule 10 and
  `.github/workflows/ci.yml` `esp32` job; IDF component wiring *demonstrated* on
  2026-08-29 — `./pipeline.sh esp32` (espressif/idf:v5.3, Docker Desktop WSL
  integration) compiled `l3/l3_header.cpp`, `l3_payload.cpp`, `l3_descriptor.cpp`,
  `l3_status.cpp` and `main/l3_smoke.cpp` with `xtensa-esp32s3-elf-g++`, zero warnings,
  `omgp-host.bin` 206,800 bytes.

## R-02 — Codegen: Jinja2 templates, deterministic by construction

- **Decision**: rewrite `tools/codegen.py` around three Jinja2 templates under
  `protocol/templates/`. Determinism rules: iterate every mapping in sorted key order
  (never source order); no timestamps, hostnames, paths or Python version in output;
  `\n` line endings; `keep_trailing_newline=True`; `trim_blocks/lstrip_blocks` on;
  integers rendered in a fixed style (`0x%02X` for byte-sized codes, decimal for
  limits/timing). CLI: `--yaml FILE` (default `protocol/omgp-protocol.yaml`), `--out DIR`
  (default `build/gen`), `--check` (generate to temp, exit 1 if different from `--out`),
  `--vectors DIR` (also emit `omgp_vectors.h` from `tests/vectors/*.json`).
  Non-numeric values are emitted as `inline constexpr const char*` / Python `str`;
  per-entry attributes become tables (`OPCODE_INFO[]`, `TLV_INFO[]`) in both languages.
  Validation before rendering: duplicate codes/types, values wider than their field,
  unknown `target` values → non-zero exit naming the conflict (FR-004).
- **Rationale**: current generator loses string values as `0 /* name */` comments and
  hard-codes emission in Python — attribute tables (required/repeated/max_len) are what
  FR-020 needs. Jinja2 is already in `tools/requirements.txt` and CI installs it.
- **Alternatives considered**: keep string-building Python (rejected: three outputs,
  tables, and escaping get unreadable); Mako/string.Template (rejected: Jinja2 already
  listed; no new dependency).
- **Constraint found**: Jinja2 is **not installed** in the local WSL environment
  (`python3 -c 'import jinja2'` fails) and local Python is 3.10; `pip install -r
  tools/requirements.txt` is a quickstart prerequisite, and tooling must stay
  3.10-compatible despite the 3.11+ stated floor.
- **Label**: determinism *by construction* (sorted iteration, no volatile inputs) and
  *demonstrated* by the determinism test (two runs → same sha256; shuffled-key YAML →
  same sha256) which the tasks add first.

## R-03 — Test framework: Catch2 v3, vendored amalgamated

- **Decision**: vendor `catch_amalgamated.hpp` + `catch_amalgamated.cpp` under
  `third_party/catch2/` with `LICENSE.txt` (Boost Software License 1.0 — permissive,
  Apache-2.0-compatible) and a `VERSION` file recording the exact release tag and the
  sha256 of both files. Build as a static library once (`catch2_amalgamated`), link
  test executables against it. The bootstrap g++ path compiles the same two files.
  A `tests/support/catch_listener.cpp` event listener prints `EXECUTED: <n>` where `n` is
  the total assertion count at run end, preserving the `pipeline.sh` floor contract on
  both the ctest path (parsed from `LastTest.log`, summed across binaries) and the
  bootstrap path.
- **Rationale**: human ruling 2026-08-28 approved a framework; Catch2 was the one named
  in the plan input and in the existing CMake comment. Amalgamated vendoring builds
  offline (FR-032) — FetchContent needs network at configure time, which the local
  sandbox lacks.
- **Alternatives considered**: FetchContent (rejected: offline requirement); doctest
  (rejected: not what was asked; no advantage here); GoogleTest (rejected: heavier,
  BSD-3 fine but CMake integration noisier).
- **Label**: licence compatibility *demonstrated* by the BSL-1.0 text; exact version and
  hashes *assumed* until the dependency slice fetches them (no network here); listener
  API (`Catch::EventListenerBase::testRunEnded`, `Catch::Totals::assertions.total()`)
  *assumed* — verify against the vendored header.

## R-04 — Mutation tool: Mull with git-diff incremental mode

- **Decision**: `tools/mutate.sh --diff <ref>` builds a Mull-instrumented native build
  with clang (`-fpass-plugin` / `mull-runner`), restricted to lines changed relative to
  `<ref>` via Mull's incremental options, and enforces a survival threshold read from
  `tools/mutate.cfg` (initial: ≥ 80 % kill rate of *reached* mutants; tune after first
  real run). Output: killed/survived/not-covered counts and a list of survivors with
  file:line. Behaviour when Mull is absent: print the disclosure `mutation: mull not
  present — skipped (blind spot: no mutation coverage in this environment)` and exit 0,
  **unless** `--require` is given, in which case exit non-zero. CI `deep-verify`
  installs the Mull package pinned to the runner's clang major version and calls
  `mutate.sh --diff origin/main --require`. With no changed lines under `l3/`, `link/`,
  `core/`, print `mutation: nothing in scope` and exit 0 in < 60 s (SC-005).
- **Rationale**: ruling approved a third-party open-source mutation tool; Mull is
  Apache-2.0, LLVM-native (real semantic mutants, sanitizer-compatible), and its
  incremental/diff mode is exactly FR-027. Its coupling to an LLVM major version is the
  known cost — mitigated by pinning and by `--require` making a broken install fail
  loudly rather than pass silently (spec FR-028).
- **Alternatives considered**: universalmutator (Python, regex-based; rejected: no diff
  scoping, textual mutants, slow full rebuild per mutant); Dextool mutate (D toolchain;
  rejected: heavy toolchain for CI); repo-local script (rejected by ruling).
- **Label (updated during implementation, T058/T059)**: *demonstrated*, not assumed.
  Verified against the project on 2026-08-28: latest release **0.34.0**; `.deb` per LLVM
  major for Ubuntu 22.04/24.04 (`Mull-<N>-0.34.0-LLVM-<x.y.z>-ubuntu-amd64-<ver>.deb`);
  plugin `/usr/lib/mull-ir-frontend-<N>` with `-fpass-plugin=… -g -grecord-command-line`;
  runner `mull-runner-<N>`; incremental mode via `mull.yml` `gitDiffRef`/`gitProjectRoot`;
  reporters `IDE, SQLite, GitHubAnnotations, Patches, Elements, Sarif` (no "JSON").
  Exercised locally by extracting the LLVM-14 package (no install) and running it on
  this repository — 997 mutants across `l3/`. Two traps found and fixed in the harness:
  (1) the IR frontend reads `mull.yml` at **compile** time, so the config must exist
  before the instrumented build and the build must be fresh; (2) an unknown mutator
  name silently yields zero mutants — the harness now fails on "no mutants in a
  non-empty scope" instead of passing; (3) `gitDiffRef` must NOT be in the
  compile-time config (the plugin then embeds nothing) — it belongs to the run-time
  config only; (4) `includePaths`/`excludePaths` must not be used at all: excluding
  the TU that holds `main()` (Catch2) removes the run-time mutant dispatch, so every TU
  is instrumented; (5) Mull's `gitDiffRef` filter drops every mutant in files the diff
  **adds** (only modified-file hunks survive), so it is not used at all — `mutate.sh`
  runs every mutant (`--workers`) and scopes the report itself from `git diff -U0`,
  new files included, restricted to `l3/ link/ core/`. Measured kill rate on the
  descriptor commit with the unit binaries as oracle: **76.8 % (265/345)** — threshold
  ruling queued in `docs/OPEN-QUESTIONS.md`. The "gate can fail" property is
  *demonstrated* by the planted-mutant test in quickstart / `tests/fuzz/README.md`.

## R-05 — Fuzzing: libFuzzer via a `fuzz` CMake preset

- **Decision**: add configure preset `fuzz` (`CMAKE_CXX_COMPILER=clang++`,
  `OMGP_FUZZ=ON`, `OMGP_SANITIZERS=ON`, `-fsanitize=fuzzer,address,undefined`) building
  four targets in `tests/fuzz/`: `fuzz_header` (decode_header on raw bytes),
  `fuzz_payload` (byte 0 = opcode, byte 1 = direction, rest = payload → payload decoder),
  `fuzz_descriptor` (RecordCursor + validate on raw blob), `fuzz_roundtrip`
  (decode → if OK encode → assert bytes identical → decode again → assert equal).
  `tools/fuzz-smoke.sh <seconds>` configures/builds the preset, derives a seed corpus
  from `tests/vectors/*.json` (bytes field), runs each target for `seconds/4` with
  `-max_total_time`, `-timeout=5`, `-rss_limit_mb=512`, and exits non-zero if any
  target reports a crash/timeout/leak; artefacts go to `build/fuzz/artifacts/`. Longer
  local runs are the same script with a larger budget.
- **Rationale**: libFuzzer links locally (clang 14: `-fsanitize=fuzzer,address` compiled
  and ran 10 iterations) and CI `deep-verify` already installs clang; zero new packages.
- **Alternatives considered**: AFL++ (rejected: extra package, no sanitizer integration
  advantage); honggfuzz (same).
- **Label**: libFuzzer availability *demonstrated* (compile+run in this session);
  target/harness design *by construction*; "planted bug is found within budget"
  *demonstrated* by the quickstart check once implemented.

## R-06 — Descriptor API: streaming cursor + validator, no record-count caps

- **Decision**: no materialised `Descriptor` struct with fixed arrays. The C++ API is
  (a) `RecordCursor` — zero-copy iterator over TLV records in a caller's blob,
  returning `{type, len, value_ptr}` and `Status::Truncated` when a length overruns;
  (b) typed decoders per record type (`decode_protocol(const RecordView&, ProtocolRec&)`
  etc.) into small POD structs holding string *views* (pointer+length into the blob);
  (c) `validate_descriptor(blob, len, Report&)` — one pass using a 256-bit seen-bitmap
  to enforce cap, required presence (from generated `TLV_INFO`), duplicates of
  non-repeated types, per-type fixed lengths, `max_len`, UTF-8, and to count skipped
  unknown types; (d) `DescriptorWriter` — appends records into a caller buffer, enforcing
  the 2048 cap and `max_len` at write time, with `append_raw(type, bytes)` for unknown/
  vendor records so round-trips are exact; (e) `descriptor_crc(blob, len)` =
  `crc16_ccitt_false` over the blob.
- **Rationale**: a descriptor may legally hold hundreds of CHANNEL/PARAM records
  (2048 bytes ÷ 3-byte minimum), so any fixed-capacity struct either wastes RAM on the
  ESP32-S3 or invents a limit the protocol doesn't have. Streaming makes memory O(1) *by
  construction* and keeps rule 5 trivially true. Semantic identity for differential
  testing is then "same sequence of typed records" — see R-07.
- **Alternatives considered**: `Descriptor` struct with `MAX_CHANNELS/MAX_PARAMS`
  constants (rejected: invents limits; FR-005 scan would also flag them as suspicious
  literals); heap-allocated vectors (rejected: rule 5).
- **Label**: O(1) memory *by construction*.

## R-07 — Differential format: canonical text lines, batch-mode helper

- **Decision**: define one line-oriented **canonical text** format
  (`contracts/canonical-text.md`) used by: the golden-vector JSON (`fields` are rendered
  to it), the generated `omgp_vectors.h`, `tools/l3_helper` (C++, host-only: reads
  request lines on stdin, writes one result line per input), and `tools/refimpl/`
  (`canonical.py`). `diffcheck.py` spawns the helper **once** per corpus and streams
  thousands of lines through it, so ≥11k cases finish well inside SC-003's 2 minutes
  (200 subprocess spawns today take ~1 s; one long-lived process removes spawn cost).
  Decoding equality = string equality of canonical output; rejection equality = same
  `ERR <Status-name>` token.
- **Rationale**: keeps JSON out of C++ (clarification Q4, Principle VIII), makes
  mismatches human-readable in CI logs, and makes the helper trivially parsable with
  `strtoul` — no parser dependency.
- **Alternatives considered**: nlohmann/json in the helper (rejected: unapproved
  dependency); Python C extension/ctypes binding to `omgp_l3` (rejected: ABI plumbing,
  and it would let the two implementations share process state).
- **Label**: *by construction*; throughput claim *assumed* until measured (SC-003 is the
  measurement).

## R-08 — Golden vectors: generated once by the reference implementation

- **Decision**: `tools/refimpl/genvectors.py` writes `tests/vectors/*.json` (schema in
  `contracts/golden-vector.schema.json`) from a curated list of field values with a
  `spec_ref` per vector. It is run by a human when vectors are created or when a
  protocol ruling changes them, and the commit message carries the justification
  (CLAUDE.md rule 9). It is **not** run by the pipeline; instead the pipeline verifies
  that regenerating would be a no-op (`genvectors.py --check`), so a stale vector set
  fails loudly rather than being silently rewritten.
- **Rationale**: vectors are immutable evidence; producing them from the independent
  implementation (Principle III) and then requiring the C++ side to match them is the
  ordering that makes the C++ tests meaningful.
- **Label**: *by construction*.

## R-09 — Error model: `Status` enum, caller-provided buffers

- **Decision**: every codec function returns `omgp::l3::Status` (enum class, `Ok` = 0)
  and writes results through out-parameters; encoders take `(const T&, uint8_t* out,
  size_t cap, size_t& written)` and return `BufferTooSmall` without partial writes when
  `cap` is insufficient; decoders take `(const uint8_t* in, size_t len, T& out)`. The
  `Status` set maps 1:1 onto the categories spec FR-011/US4-5 require: `Truncated`,
  `LengthMismatch`, `OutOfRange`, `UnknownOpcode`, `MissingRequired`, `DuplicateRecord`,
  `StringTooLong`, `BlobTooLarge`, `BufferTooSmall`, `InvalidUtf8`, `MalformedRecord`,
  `ReservedViolation`. A `Report` struct carries detail (offending type/offset) for
  descriptor validation. `status_name(Status)` returns a string literal for the helper.
- **Rationale**: CLAUDE.md style ("expected-style result type", errors by return value);
  C++17 has no `std::expected`; a full `Expected<T>` template adds little over
  `Status` + out-param for POD results and complicates the no-exceptions story.
- **Alternatives considered**: hand-rolled `Expected<T,E>` (rejected: extra template
  surface for no caller benefit here; may be added later for `core/`); error codes as
  `int` (rejected: not type-safe).
- **Label**: *by construction*.

## R-10 — Static enforcement of rules 1, 4, 5 (`tools/check_embedded.py`)

- **Decision**: a Python scan run in the `quality` stage over `l3/`, `link/`, `core/`:
  (a) forbidden tokens — `new `, `delete `, `malloc`, `free(`, `std::vector`,
  `std::string`, `std::map`, `std::unique_ptr`, `throw`, `try`, `catch`, `dynamic_cast`,
  `typeid`, `#include <string>|<vector>|<map>|<chrono>|<thread>|<iostream>`;
  (b) protocol-literal check — any integer literal whose value equals an opcode, error
  code, TLV type, event, module type, flag, or limit from the YAML, **excluding** values
  < 0x10 (bit masks and small counts are too common) unless the literal appears in a
  `case`/comparison against a field named in the YAML; an escape hatch comment
  `// literal-ok: <reason>` suppresses one line. Additionally every native Catch2 test
  links with `-Wl,--wrap=malloc` and a **counting** `__wrap_malloc` (forwards to
  `__real_malloc`); tests wrap each codec call in `HEAP_FREE_SCOPE` and assert the
  count did not change. (An aborting wrapper is wrong: `--wrap` is link-wide and Catch2
  itself allocates, so every test would abort at startup.)
- **Rationale**: SC-007 asks for zero findings; a grep-level scan is cheap, portable,
  and runs without cmake (bootstrap path).
- **Alternatives considered**: clang-tidy checks only (rejected: not present on the
  bootstrap path); `-ffreestanding` (rejected: not what it enforces).
- **Label**: heap absence on native *demonstrated* by the counting wrapper (zero
  `malloc` calls across every codec invocation in the suite); on Xtensa *assumed* (same
  sources, `-fno-exceptions -fno-rtti` applied in the IDF component).

## R-11 — Payload layouts not fixed by §3.1 beyond the FR-008 ruling (assumed defaults)

The following were not covered by the four clarification rulings and are **assumed**
with a safe default; each is queued for `docs/OPEN-QUESTIONS.md` in the T3 slice:

| Item | Default adopted | Why safe |
|---|---|---|
| Response payload of SELECT_CHANNEL / SET_BYPASS / SET_PARAM | empty (0 bytes) = "accepted"; failures come as ERROR responses | §3.2 says "responds immediately with accepted"; no field is named |
| GET_EVENT on an empty queue | `event_type = 0x00` (new YAML symbol `EVT_NONE`), `remaining_count = 0`, no detail | YAML has no 0x00 event; a "none" value is needed for "repeat until empty flag" |
| PING / IDENTIFY / GET_STATUS request payloads | empty | §3.1 names no fields |
| ERROR detail | optional bytes after the code, preserved verbatim, ≤ 63 | §3.1 "optional detail" |
| Required string records may be empty (len 0) | accepted (spec gives only an upper bound) | not inventing a lower bound; host acceptance logic can be stricter later |
| POWER_TUBE `power_class` | 1–4 (T1–T4), 0 and >4 rejected | §4.1 names T1–T4 |
| AUDIO `input_mode` | 0 or 1, else `OutOfRange` | §4.1 enumerates two values |
| PARAM `kind` | 0–5, else `OutOfRange` | §4.1 enumerates six |
| SET_BYPASS value | 0 or 1, else `OutOfRange` on decode, refused on encode | §3.1 "payload = 0/1" |
| READ_DESC response `len` | ≤ 61 (64 − 3); codec does **not** enforce the 28-byte module-bus chunk (transport concern) | `limits.max_l3_payload` |

## R-12 — Local environment blind spots (stated, per CLAUDE.md working agreements)

- Python 3.10, no Jinja2, no pytest locally → `codegen` and `refimpl` stages fail here
  until `pip install -r tools/requirements.txt`; CI has 3.12 with deps.
- clang 14 locally vs clang 18 in CI → fuzz targets must compile on both; avoid
  clang-18-only flags.
- No Mull locally → mutation is disclosed-skipped locally; only CI exercises it (R-04
  `--require`).
- No `arm-none-eabi-gcc` locally (and not wanted, R-01); Docker present → `esp32` stage
  runnable locally.
