# tests/fuzz — libFuzzer targets

Built only under the `fuzz` CMake preset (clang, `OMGP_FUZZ=ON`); run by
`tools/fuzz-smoke.sh <seconds>` (CI deep-verify: 600 s; local smoke: 60 s). Seeds come from
`tests/vectors/*.json`. Artefacts land in `build/fuzz/artifacts/<target>/`; the script
prints a `reproduce:` command for each.

| Target | Input shape | Invariants that trap |
|---|---|---|
| `fuzz_header` | raw message bytes | header decode never over-reads; a decoded header re-encodes to the same bytes |
| `fuzz_payload` | `[opcode, dir, payload…]` | every typed decoder accepts/rejects without a crash; any decoded value re-encodes byte-identically |
| `fuzz_descriptor` | raw descriptor blob | cursor/validator never over-read; a validated blob re-emits identically via `add_raw`; every typed decoder is total |
| `fuzz_roundtrip` | raw message bytes | decode → canonical → parse → encode reproduces the input; rendering is stable |

Reproduce a finding: `build/fuzz/<target> build/fuzz/artifacts/<target>/<crash-file>`.

## Planted-bug evidence (spec 001 T061 — paste output into the PR)

The harnesses only prove something if they *can* fail. Each procedure below is run
locally, its discriminating output recorded here, and the code restored before commit.
If the "broken" run produced the same output as the clean run, the harness is not
reaching the code and must be fixed before the PR is opened.

- [x] **Fuzz reaches the decoder.** Replace `if (len < HEADER_LEN) return Status::Truncated;`
      in `l3/l3_header.cpp` `decode_header` with `(void)len;` (deleting it outright fails
      the build on `-Werror=unused-parameter` — a correct refusal, not the experiment),
      rebuild, `./tools/fuzz-smoke.sh 20`. Expected: ASan report from `fuzz_header`, an
      artefact, exit ≠ 0. **Recorded 2026-08-28 (WSL2, clang 14):**
      ```
      fuzz: fuzz_header runs=1 cov=0 findings=1 exit=1
      ==219891==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000000011 ...
      SUMMARY: AddressSanitizer: heap-buffer-overflow .../l3/l3_header.cpp:39:19 in omgp::l3::decode_header(...)
        reproduce: build/fuzz/fuzz_header build/fuzz/artifacts/fuzz_header/crash-da39a3ee5e6b4b0d3255bfef95601890afd80709
      ```
      (the artefact is the empty input). With the guard restored the same target ran
      12,235,541 iterations with `findings=0`. Source verified identical to HEAD afterwards.
- [x] **Mutation scoping reaches the codec — and detects a missing test.** Mutation testing
      finds *absent tests*, so the plant is a deleted test, not a changed source: touch the
      `r.offset >= LIMIT_max_descriptor_bytes` line in `l3/l3_payload.cpp`
      `encode_read_desc_req` (a comment — brings the line into `--diff HEAD` scope) and
      delete the only assertion that exercises that bound
      (`encode_read_desc_req(ReadDescReq{2048, 1}, …) == Status::OutOfRange` in
      `tests/unit/test_l3_payload.cpp`; the golden vectors use offsets 0/1987/2040, so
      nothing else covers it). Run `./tools/mutate.sh --diff HEAD --require`; then restore
      the test and run again. **Recorded 2026-08-28 (Mull 0.34.0 for LLVM 14, extracted
      package, `MULL_RUNNER`/`MULL_PLUGIN` overrides):**
      ```
      run A (test deleted):  mutants=2 killed=1 survived=1 kill_rate=50.0% threshold=80%
                             survivor: l3/l3_payload.cpp:101:18 cxx_ge_to_gt
                             mutation: FAIL   exit 1
      run B (test present):  mutants=2 killed=2 survived=0 kill_rate=100.0% threshold=80%
                             mutation: PASS   exit 0
      ```
      The survivor names exactly the bound whose test was removed (`>=` → `>`). Sources
      verified identical to HEAD afterwards. Baseline for the descriptor commit
      (`--diff HEAD~1`): 345 mutants, 265 killed, 80 survived, 76.8 %. (Recorded under the
      percentage gate; the 2026-08-29 ruling in `docs/OPEN-QUESTIONS.md` replaced it with
      the per-survivor triage.)
- [x] **The triage gate bites end-to-end (unplanned, 2026-08-29).** After the whole-`l3/`
      triage (123 baseline survivors → 111 killed by new tests, 12 labelled) the CI-form run
      `./tools/mutate.sh --diff origin/main --require` reported
      ```
      mutation: mode=diff diff_ref=origin/main reports=3 mutants=599 killed=586 survived=13
                not_covered=0 kill_rate=97.8% labelled[equivalent=6 accepted=6] unlabelled=1 max_unlabelled=0
        UNLABELLED survivor: l3/l3_descriptor.cpp:80:42 cxx_sub_to_add
      mutation: FAIL   exit 1
      ```
      A mutant the baseline had counted as killed (`len - 1` → `len + 1` in the CHANNEL
      UTF-8 check) survived once the new tests changed the heap layout: its "kill" had been
      an over-read into garbage past a `std::vector`. The gate refused the run at 97.8 %,
      which a percentage gate would have passed; the deterministic killing test
      ("string-tail checks read exactly len bytes") followed, and the re-run is the PASS
      recorded in the PR #15 body.
