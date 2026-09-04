# Contract: tooling, pipeline and CI changes for feature 002

Every fast/partial path states its blind spot (CLAUDE.md working agreements). Nothing here
touches `.github/` — the existing CI already runs every stage this feature extends.

## `tools/diffcheck.py --frames`

- Adds three corpora through the one long-lived `l3_helper` process: valid frames
  (`FENC`/`FDEC`, ≥ 10 000, seeded), torture elements (`FSTREAM`, `torture.corpus(seed)`),
  and multi-frame streams (`FSTREAM`, ≥ 1 000, seeded: 2-3 frames sharing FLAG delimiters,
  with reserved-dst / over-cap / bad-CRC discard elements placed before valid frames so
  discard→delivery resync is exercised, and reserved-ctrl-bit frames that must be
  DELIVERED with the reserved ctrl bits 2-3 ignored — forward-compat behaviour that
  link/frame.cpp implements and the corpus pins; §4 defines those bits as "reserved 0"
  but states no ignore-on-receive rule, so this is pinned-by-test, not spec-mandated
  (see docs/OPEN-QUESTIONS.md 2026-09-04, reserved ctrl bits)).
- Default run (no flag) includes them, so the pipeline's `diffcheck` stage covers L2;
  `--frames-only` for local iteration. Summary line gains `frames <n>, torture <m>,
  streams <s>`.
- Budget: the whole stage stays under 2 minutes on CI (feature-001 SC-003 plus SC-002 of
  this feature: torture < 60 s).

## `tools/fuzz-smoke.sh`

- `TARGETS` gains `fuzz_frame`; seeds for it come from `tests/vectors/frame_*.json`
  (wire bytes) plus the torture generator's first 32 elements (written by the seed step).
- Budget split stays `BUDGET / #targets`.

## `tools/check_embedded.py`

- Default `--cite-dirs` becomes `l3 link`: every `link/` file must cite `trunk §…`
  (`link/crc16.hpp` already does). Literal rules unchanged; `TRUNK_*` values ≥ 0x10
  (e.g. 0x7E, 0x7D, 0x20, 20, 50, 100, 200, 2000, 1000) are already in the YAML-derived
  literal table, so a restated timing value in `link/` fails the quality stage.
- Regression test added to `tools/refimpl/test_check_embedded.py`: a `link/` file without
  a citation fails; a restated `200` in `link/` fails.

## `tools/refimpl/test_timing_map.py` (research R-12)

- Reads `link_trunk` keys from the YAML; maps each to its tag:
  `T_turn_min_us → [timing:T_turn_min]`, `T_turn_max_us → [timing:T_turn_max]`,
  `T_resp_us → [timing:T_resp]`, `T_gap_us → [timing:T_gap]`, `T_poll_us → [timing:T_poll]`,
  `bit_rate → [timing:bit_rate]`, `bit_rate_fallback → [timing:bit_rate_fallback]`,
  `retries → [timing:retries]`, plus `limits.max_l3_payload → [timing:max_payload]`.
- Greps `tests/unit/test_link_*.cpp` and `tests/property/test_link_*.cpp`; fails naming any
  tag with zero occurrences; prints the tag → `TEST_CASE` name map (SC-001's listing).

## `pipeline.sh`

- `build` (bootstrap g++ path): compile `link/*.cpp` into every Catch2 binary and into
  `l3_helper`; the disclosure line unchanged.
- `quality`: clang-tidy set gains `link` (already in clang-format's).
- `unit`: `UNIT_TEST_FLOOR` raised to the new total minus 5 (commit message records the
  numbers; never lowered).
- `esp32`: unchanged invocation; the IDF project gains the `omgp_link` component.

## CMake

- `link/CMakeLists.txt`: `add_library(omgp_link STATIC frame.cpp master.cpp responder.cpp
  health.cpp)`, `-fno-exceptions -fno-rtti`, include dirs `link/`, root, `build/gen`.
- Root: `add_subdirectory(link)`; `omgp_add_catch_test` links `omgp_link` as well as
  `omgp_l3`; `omgp_test_support` gains `mock_wire.cpp`; `l3_helper`/`omgp_canon` link
  `omgp_link`; new tests registered; `tests/fuzz/CMakeLists.txt` adds `fuzz_frame`.
- `esp32-host/components/omgp_link/CMakeLists.txt`: `idf_component_register(SRC_DIRS
  ../../../link INCLUDE_DIRS ../../../link ../../../build/gen)` with
  `-fno-exceptions -fno-rtti`; `main/link_smoke.cpp` references `encode_frame`, `Deframer`,
  `Master`, `Responder`, `HealthTracker` with a trivial in-memory `ByteWire`/`Clock` so
  the component is linked, not just compiled.

## Mutation and CI

- `tools/mutate.cfg` scope already includes `link`; the triage gate (zero unlabelled
  survivors on changed lines) applies to this feature's PR via `deep-verify`. Labels, if
  any, follow the 2026-08-29 policy and are listed in the PR body.
- Risk: `link/` is T2 (`risk-score.yml`), so `deep-verify` runs (fuzz 600 s + mutation)
  on the PR; no workflow edit is needed.

## Evidence the gates bite (quickstart proves each)

- Fuzz: remove the `TooLong` guard in `Deframer::feed` → ASan overflow in `fuzz_frame`
  within the budget; restore → `findings=0`.
- Mutation: change `attempt < TRUNK_retries` to `<=` in `Master` and delete the
  "exactly two retries" test → `UNLABELLED survivor … cxx_lt_to_le`, exit 1; restore → PASS.
- Timing map: rename one `[timing:T_gap]` tag → `test_timing_map.py` fails naming `T_gap`.
