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
| `fuzz_frame` | byte stream, fed byte-at-a-time to two independent `Deframer`s | deframe never over-reads or hangs on arbitrary bytes; the `TooLong`/`kMaxUnstuffed` accumulator bound is never exceeded; two fresh `Deframer`s fed the identical byte sequence deliver identical frames — a determinism check, not a chunk-boundary differential (`feed()` has no notion of chunk boundaries and is called once per byte either way, `fuzz_frame.cpp:27-31`; trap on mismatch, `fuzz_frame.cpp:59-63`); a delivered frame re-encodes and re-parses to equal fields |

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
- [x] **Fuzz reaches the `TooLong` bound (spec 002 T026, SC-003 planted-bug half).**
      SC-003 is per target: a run at the CI fuzz budget with zero crashes, and
      "a planted missing bounds check is found within that budget" — only the
      second half is recorded here. `tools/fuzz-smoke.sh <seconds>` splits its
      argument evenly across all five targets (`fuzz-smoke.sh:53`), so `fuzz_frame`'s
      own share is `<seconds>/5`: 12 s for this record's invocation
      (`./pipeline.sh fuzz` → `tools/fuzz-smoke.sh 60`), versus 120 s under CI
      deep-verify's `tools/fuzz-smoke.sh 600` (`.github/workflows/ci.yml:150`). The
      other half of SC-003 — a clean run at that 120 s CI share — is not established
      by this PR: it is T0 and skips the deep-verify job (`ci.yml:128-131`); it is
      established by whichever T2/T3 deep-verify run most recently reported
      `fuzz_frame` clean, not cited here. First, the clean
      baseline: `./pipeline.sh fuzz` (`tools/fuzz-smoke.sh 60`, unmodified tree). Then,
      neutralize `link/frame.cpp` `Deframer::append`'s guard —
      `if (len_ >= kMaxUnstuffed) {` → `if (false) { // ...` (an always-false condition,
      not a deleted check: `len_` stays live via `buf_[len_++] = byte`, so unlike the
      `l3_header.cpp` precedent's `(void)len;` there is no unused-parameter to silence) —
      rebuild the `fuzz` preset, rerun. Expected: with no upper bound, a stream with no
      FLAG byte for 71+ unstuffed bytes writes past `buf_[kMaxUnstuffed]`. UBSan will
      diagnose the exact out-of-bounds index, but it is built recoverable here (no
      `-fno-sanitize-recover=undefined` in `tests/fuzz/CMakeLists.txt:4`, no
      `UBSAN_OPTIONS` set in `fuzz-smoke.sh`), so a UBSan report alone prints and execution
      continues; the harness's actual failure signal (`findings ≥ 1`, `exit ≠ 0`,
      `fuzz-smoke.sh:66-68`) comes from ASan's abort once the same unchecked write reaches
      a redzone. Finally, restore the guard
      (`git diff -- link/frame.cpp` empty against HEAD), rebuild, rerun a third time —
      expect `findings=0` matching the clean baseline. **Recorded 2026-09-04 (Ubuntu 24.04.4
      LTS, clang 18.1.3):**
      ```
      clean (before):
      fuzz: fuzz_frame runs=167764 cov=95 findings=0 exit=0

      broken (guard neutralized):
      fuzz: fuzz_frame runs=119 cov=95 findings=1 exit=1
      /home/runner/work/omgp/omgp/link/frame.cpp:79:5: runtime error: index 70 out of bounds for type 'uint8_t[70]' (aka 'unsigned char[70]')
      SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior /home/runner/work/omgp/omgp/link/frame.cpp:79:5
      =================================================================
      ==7411==ERROR: AddressSanitizer: stack-buffer-overflow on address 0x7ffff5901888 at pc 0x55555569c26b bp 0x7fffffffada0 sp 0x7fffffffad98
      WRITE of size 1 at 0x7ffff5901888 thread T0
          #0 0x55555569c26a in omgp::link::Deframer::append(unsigned char) /home/runner/work/omgp/omgp/link/frame.cpp:79:18
          #1 0x55555569c26a in omgp::link::Deframer::feed(unsigned char, omgp::link::FrameView&) /home/runner/work/omgp/omgp/link/frame.cpp:168:5
          #2 0x555555698874 in (anonymous namespace)::feed_all(...) /home/runner/work/omgp/omgp/tests/fuzz/fuzz_frame.cpp:33:19
          #3 0x555555697871 in LLVMFuzzerTestOneInput /home/runner/work/omgp/omgp/tests/fuzz/fuzz_frame.cpp:53:42
      Address 0x7ffff5901888 is located in stack of thread T0 at offset 136 in frame
          #0 0x5555556985e7 in (anonymous namespace)::feed_all(unsigned char const*, unsigned long, unsigned long) /home/runner/work/omgp/omgp/tests/fuzz/fuzz_frame.cpp:23
        This frame has 2 object(s):
          [32, 136) 'd' (line 24) <== Memory access at offset 136 overflows this variable
          [176, 192) 'v' (line 25)
      SUMMARY: AddressSanitizer: stack-buffer-overflow /home/runner/work/omgp/omgp/link/frame.cpp:79:18 in omgp::link::Deframer::append(unsigned char)
      ==7411==ABORTING
        reproduce: build/fuzz/fuzz_frame build/fuzz/artifacts/fuzz_frame/crash-220ee0b66c4e8d5c1682437e7dc9e4d85578c500

      restored (after):
      fuzz: fuzz_frame runs=150860 cov=95 findings=0 exit=0
      ```
      The guard's exact bound is pinpointed by the UBSan line, `index 70 out of bounds
      for type 'uint8_t[70]'` at `link/frame.cpp:79` — the `buf_[len_++]` write the removed
      guard exists to stop. The ASan report that follows is a *consequence* of the same
      unchecked write, not independent confirmation of the bound, and it is not a smooth
      walk to the object's edge: `buf_` occupies `Deframer` object offsets 1..70 and `len_`
      (an 8-byte `size_t`) starts at offset 72 (`state_`(1) + `buf_[70]` + 1 byte pad +
      `len_`(72..79) + `stats_`(80..103) = 104 bytes, matching the report's `[32, 136)` for
      `d`), so the very next out-of-bounds write, `buf_[71]`, lands on `len_`'s own low byte
      (little-endian) — the loop's own index is corrupted by its own out-of-bounds write
      before the access ever reaches a redzone. From there the reported crash address is a
      function of whatever value that corruption produced, not a fixed offset; this run's
      corrupted walk happened to land at offset 136, the byte immediately past the whole `d`
      object (`[32, 136)` in the report, itself annotated "overflows this variable") — not,
      as an earlier version of this entry said, the frame's *next* variable: `v` sits at
      `[176, 192)`, well beyond where the access actually landed. (This layout-based
      mechanism is inferred from `link/frame.hpp:41-44`, not traced with a debugger —
      labelled per CLAUDE.md rule 11.) `git diff -- link/frame.cpp` against HEAD was empty after restoration,
      confirmed both by direct diff and by the third run's `findings=0` matching the first.
      Source verified identical to HEAD afterwards.

      Note also that the "broken" block above is not literal `./pipeline.sh fuzz` stdout:
      on a finding the script's own stdout is only the `fuzz: fuzz_frame runs=… findings=…`
      line plus five grep'd lines (`fuzz-smoke.sh:70-71`); the full stack trace and report
      bodies pasted above are copied from `build/fuzz/fuzz_frame.log`, which the script
      always writes in full regardless of what it echoes to stdout.
