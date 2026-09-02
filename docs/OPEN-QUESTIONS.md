# Open Questions

Append-only log of spec ambiguities encountered during implementation, and
their resolutions. Entries are dated and never edited or deleted. A
decision is changed by appending a new, superseding entry that references
the entry it supersedes — never by editing history.

## Entry format

```
## YYYY-MM-DD — <short title>

**Context:** what was ambiguous and where (spec section, file).
**Recommendation:** the safe default proposed, if any.
**Ruling:** human decision, or "pending" until one is made.
**Supersedes:** (optional) link/reference to the prior entry this replaces.
```

---

## 2026-08-28 — Software licence: Apache-2.0

**Context:** `docs/omgp-spec-v0.7.md` §50 "Licensing Direction" says
licensing "remains to be formally selected" (Apache-2.0 or MIT plausible
for software), while `docs/GOVERNANCE.md` §4b and constitution Principle
VIII state Apache-2.0. The committed `LICENSE` file contained no licence
text at all — only a sandbox network-egress error message captured when
an earlier fetch of the Apache-2.0 text failed — so no licence was
actually granted for this public repository.
**Recommendation:** Apache-2.0, as already stated by the constitution and
GOVERNANCE.md; replace `LICENSE` with the canonical text.
**Ruling:** Apache-2.0 — human instruction 2026-08-28. `LICENSE` replaced
with the canonical Apache License 2.0 text (sourced from the system
licence store, not the network). Spec §50 should be updated to record the
decision; the spec is a human-ruling artefact, so that edit is left to a
human.
**Supersedes:** none.

## 2026-08-28 — L3 response payload layouts (IDENTIFY, READ_DESC, GET_PARAM, GET_EVENT)

**Context:** `docs/protocol-l3.md` §3.1 describes these four responses in
prose only ("Returns protocol version, module type, descriptor length,
descriptor CRC-16"; "response carries value"; etc.) with no field widths
or order. Feature `specs/001-protocol-foundation` (FR-008) cannot encode
them faithfully without a fixed layout.
**Recommendation:** adopt concrete provisional layouts, little-endian per
§4 convention: IDENTIFY = u8 major, u8 minor, u8 module_type, u16
desc_len, u16 desc_crc; READ_DESC = u16 offset (echo), u8 len, u8[len];
GET_PARAM = u8 param_id, u8 scope, u16 value; GET_EVENT = u8 event_type,
u8 remaining_count, u8[] detail. Add them to `protocol/omgp-protocol.yaml`
and the §3.1 table in the same (T3, CODEOWNERS-reviewed) change.
**Ruling:** adopted as recommended — human, 2026-08-28. Provisional: the
GET_EVENT layout may be revisited when protocol-l3.md §6.4 (drain multiple
events per call?) is settled; that would be a superseding entry here.
**Supersedes:** none.

## 2026-08-28 — Backplane opcode payloads (BP_SLOT_MAP, BP_POWER, BP_ROUTE)

**Context:** `docs/protocol-l3.md` §3.1 gives BP_SLOT_MAP and BP_POWER in
prose only and marks BP_ROUTE "format TBD with routing hardware"; §6.1
questions whether BP_ROUTE belongs at L3 at all. Feature
`specs/001-protocol-foundation` (FR-009) must still exercise the opcode
dispatch path for every v1 opcode.
**Recommendation:** opaque passthrough — header codec plus verbatim
payload bytes, no field-level validation — so a future field-level codec
is a purely additive change with no wire-format break.
**Ruling:** adopted as recommended — human, 2026-08-28. Field-level
layouts remain open pending backplane hardware design (slot count, rail
set) and the §6.1 decision; expect a superseding entry per opcode.
**Supersedes:** none.

## 2026-08-28 — Test-only dependencies for feature 001 (test framework, mutation tool)

**Context:** GOVERNANCE.md §1 makes any new dependency a human decision
and OPERATING-POLICY §2 forbids agents introducing one without a
`needs-human` ruling. Feature `specs/001-protocol-foundation` needs a
C++ unit-test framework for a large codec suite and a mutation-testing
tool so the CI `deep-verify` job can fail (both stubs currently exit 0).
The alternative (hand-rolled harness + repo-local mutator) was offered.
**Recommendation:** no new dependencies (repo-local tooling).
**Ruling:** the recommendation was NOT adopted. Human, 2026-08-28: one
C++ test framework and one mutation-testing tool are approved, on
condition that each is open-source under an Apache-2.0-compatible
licence, pinned to an exact version, builds offline (vendored or
equivalent), and never becomes a host-core dependency. Specific tool
selection is delegated to the feature plan; introduction lands as its own
T2 slice (spec FR-032). Adopting a framework must keep the pipeline's
executed-check floor working (spec FR-033).
**Supersedes:** none.

## 2026-08-28 — Descriptor CRC-16 variant and scope (IDENTIFY desc_crc)

**Context:** `docs/protocol-l3.md` §3.1 says IDENTIFY returns a
"descriptor CRC-16" and §4.1 uses it for descriptor caching, but neither
names the CRC variant nor the bytes it covers. `protocol/omgp-protocol.yaml`
records `link_trunk.crc: crc16_ccitt_false` for trunk frames only.
**Recommendation:** CRC-16/CCITT-FALSE over the entire descriptor blob
exactly as served by READ_DESC — reuses the trunk's variant, for which a
C++ helper and Python reference already exist and are differentially
tested; record the variant in the YAML under the descriptor limits.
**Ruling:** adopted as recommended — human, 2026-08-28. Feature 001
provides `descriptor_crc()` in both implementations (spec FR-034); the
YAML/docs update travels with the FR-008 T3 slice.
**Supersedes:** none.

## 2026-08-28 — Protocol version after pre-release YAML additions

**Context:** feature 001 adds response layouts and the descriptor CRC
variant to `protocol/omgp-protocol.yaml`. `docs/protocol-l3.md` §4.2 says
minor versions are purely additive, but is silent on whether pre-release
draft completion counts as a version-worthy change.
**Recommendation:** stay at 1.0 — the draft ("provisional until exercised
by the virtual bus") is being completed, not extended; no deployed module
can observe the change.
**Ruling:** adopted as recommended — human, 2026-08-28. Minor-version
discipline begins at the first tagged protocol release; until then,
definition-file changes do not bump `protocol.minor`.
**Supersedes:** none.

## 2026-08-28 — L3 payload details not fixed by §3.1 (feature 001 defaults)

**Context:** implementing `specs/001-protocol-foundation` surfaced
details `docs/protocol-l3.md` §3–§4 leaves open beyond the layouts ruled
above (plan research R-11). Each has a safe default that the codecs adopt
now; the reviewer of the feature's Phase 1 PR is asked to rule inline.
**Recommendation (adopted as defaults):**
- Responses to SELECT_CHANNEL / SET_BYPASS / SET_PARAM are empty
  (acceptance); failures arrive as ERROR. (§3.2 says "responds
  immediately with accepted" and names no field.)
- GET_EVENT on an empty queue returns `event_type = 0x00` — a new
  `events.NONE` symbol — with `remaining_count = 0` and no detail.
- PING / IDENTIFY / GET_STATUS / GET_EVENT requests carry no payload.
- ERROR `detail` is optional bytes after the code, preserved verbatim.
- Required string records (NAME, MANUFACTURER) may be empty: the spec gives
  only an upper bound; a lower bound is host acceptance policy.
- POWER_TUBE `power_class` is 1–4 (T1–T4); 0 and >4 are rejected.
- READ_DESC response `len` ≤ 61 (64 − 3); the 28-byte module-bus chunk is
  a transport limit the codec does not enforce.
**Ruling:** pending — rule inline on the Phase 1 PR of feature 001, or
append a superseding entry per item.
**Supersedes:** none.

## 2026-08-28 — Mutation kill-rate threshold for deep-verify

**Context:** `tools/mutate.cfg` carries `threshold_pct = 80`, the provisional
value from the feature-001 plan (research R-04: "tune after first real
run"). The first real run — Mull 0.34.0, diff scope = the descriptor
commit (`l3/l3_descriptor.*`, `l3/l3_utf8.hpp`), oracle = the three unit
binaries — measured a **62 % kill rate** (107 killed / 171 reached).
Survivors are dominated by mutants the unit tests cannot distinguish
(byte-shift/or operators in `get16` when test values have a zero high
byte, `assign_const` on report counters, boundary `<`/`<=` on values the
tests never place at the edge). The property tests would kill more but
are too slow per mutant at `-O0`. Lowering a gate threshold is a human
ruling (GOVERNANCE.md §1), not something the agent adjusts to go green.
**Recommendation:** keep 80 % as the target; set the enforced threshold
to 60 % now with the measured baseline recorded here, and raise it as
unit tests gain boundary cases (raising is agent-safe; lowering is T3).
Alternatively enforce 80 % immediately and accept that `deep-verify` is
red for this feature's PR until survivors are killed.
**Ruling:** pending — human, when wiring T062 (deep-verify `--require`).
**Supersedes:** none.

## 2026-08-28 — Mutation kill-rate threshold: corrected measurement

**Context:** the 62 % in the previous entry was measured before the
harness scoped mutants correctly — Mull's own `gitDiffRef` filter keeps
mutants in *modified* files and drops every mutant in files the diff
*adds*, so that run had scored `tools/canonical.cpp` mutants against the
descriptor unit tests. With scoping done by `tools/mutate.sh` from
`git diff -U0` (new files included) the descriptor commit measures
**76.8 % (265 killed / 345, 80 survived)**; survivors are boundary
comparisons (`<`→`<=`, `>`→`>=`) and `assign_const` on report counters
in `l3/l3_descriptor.cpp` and the UTF-8 byte-class bounds in
`l3/l3_utf8.hpp`.
**Recommendation:** keep 80 % as the enforced threshold — the gap is
small and every survivor names a missing boundary test; killing them is
ordinary agent work (CLAUDE.md working agreements: "kill surviving
mutants rather than chasing line %"). Enforce on `deep-verify` from
T062 onward.
**Ruling:** pending — human, with T062.
**Supersedes:** the previous entry's measurement and its 60 % suggestion.

## 2026-08-29 — trunk-link-layer.md §8 bridging bullet reads as self-contradictory

**Context:** raised by review on PR #13. `docs/trunk-link-layer.md` §8 says the
backplane "holds the trunk response until the module answers or its
module-bus timeout (5 ms) expires, whichever is sooner — but must always
respond on the trunk within T_resp" (200 µs). Read literally, a response
cannot both wait up to 5 ms and meet a 200 µs deadline. The intended
behaviour is unambiguous elsewhere (constitution Principle VII; the same
bullet's next clause: "if the module transaction is still in flight, the
backplane answers `ERROR: busy` … MUST NOT stall the trunk"): the backplane
answers on the trunk within T_resp *every time* — with the module's reply
if it is already available, otherwise with `ERR_BUSY` — while the module-bus
transaction continues autonomously under its own 5 ms timeout and the host
retries later. The "holds the trunk response" phrase is the misleading part.
**Recommendation:** reword the bullet to: "Frames whose L3 node ID belongs
to one of the backplane's slots are translated to module-bus transactions.
The backplane MUST answer on the trunk within T_resp on every poll: with the
module's reply if it has arrived, otherwise `ERROR: busy` (the host retries
later). The module-bus transaction proceeds independently under the 5 ms
module-bus timeout; the backplane never waits on I2C while the trunk is
waiting on it." No change to timing values or codec behaviour.
**Ruling:** pending — the trunk document is a human-ruling artefact; not
edited by the agent.
**Supersedes:** none.

## 2026-08-28 — CodeQL and dependency review become required checks

**Context:** GOVERNANCE.md §2 listed "CodeQL + dependency review" as a
gate, but branch protection on `main` required only `ci-gate`, and the
security workflow's jobs cannot be wired into `ci-gate` (different
workflow). On PR #15 the code-scanning results check was red while the
PR was mergeable — the doc and the mechanism disagreed.
**Recommendation:** make the checks required in branch protection
(`CodeQL` = GitHub's code-scanning results check, encoding "no new alert
at or above the configured severity"; `codeql` and `dependency-review` =
the security workflow's jobs), accepting that protection now names four
contexts instead of one. Alternatives considered: moving the security
jobs into ci.yml (permission and schedule differences); leaving them
advisory (a security finding would never block a merge).
**Ruling:** adopted — human, 2026-08-28 ("1"). Applied the same day via the
API: required contexts `ci-gate, CodeQL, codeql, dependency-review`,
`strict` (branch up to date) kept `true`; review count, CODEOWNERS and
force-push settings untouched. Consequence: PRs are blocked by vendored
third-party alerts until `codeql-ignore-third-party.patch` lands, and
`--admin` merges still bypass (enforce_admins is off — see §7).
**Supersedes:** none.

## 2026-08-29 — Mutation gate: per-survivor triage replaces the kill-rate threshold

**Context:** the two 2026-08-28 entries above asked for a percentage
(60 % / 80 %) to gate `deep-verify`. Measured on the whole of `l3/`
(Mull 0.34.0, unit binaries as oracle) the survivors split into three
kinds a percentage cannot tell apart: bounds the tests never place at
the edge (a missing test), mutations no observable behaviour can
distinguish (equivalent), and error-path minutiae where a test would
assert an implementation detail. A threshold passes a PR that adds an
untested branch as long as the rest of the file is well covered, and
fails a PR that adds one equivalent mutant to a small file.
**Recommendation:** none needed — direct human ruling.
**Ruling:** adopted — human, 2026-08-29. Percentage gating is the wrong
policy for this codebase. Every surviving mutant on the feature-001
code is triaged into exactly one of: (a) missing test — write the test
that kills it; (b) equivalent — labelled on its source line
`// mutant-ok(equivalent): <one line>`; (c) accepted — error-path
minutiae not worth a test, `// mutant-ok(accepted): <one line>`. The
gate is **zero survivors without a label** on changed lines
(`tools/mutate.cfg [policy] max_unlabelled_survivors = 0`,
`label_categories = equivalent accepted` — T3 constants, never lowered
to get green). The whole-repo kill rate is reported as a trend
(`mutation-trend:` line; nightly) and is never gated. The triage of the
existing survivors is recorded in the PR #15 body per CLAUDE.md rule 11.
Implemented in `tools/mutate_report.py` / `tools/mutate.sh`.
**Supersedes:** both 2026-08-28 "Mutation kill-rate threshold" entries
(their measurements stand as history; their pending rulings are closed).

## 2026-08-29 — Feature label ids: `feature:f1-codecs` … `feature:f5-cli`

**Context:** CLAUDE.md and OPERATING-POLICY §2/§4a say PRs and task
issues carry `feature:<id>` but never define `<id>`; `tools/gh-setup.sh`
creates `feature:f1-codecs`, `feature:f2-link`, `feature:f3-core`,
`feature:f4-simrig`, `feature:f5-cli`, while Spec Kit numbers feature
directories `specs/001-…`. The first agent PRs used `feature:001` in
prose and could not be labelled at all because the labels had not been
created (setup obligation GOVERNANCE §7 item 3, run 2026-08-29).
**Recommendation:** use the `gh-setup.sh` ids as the canonical labels
and map Spec Kit directories to them (001-protocol-foundation →
`feature:f1-codecs`); adding a feature means adding its id to
`gh-setup.sh` in the same PR as its spec directory.
**Ruling:** adopted — human, 2026-08-29 ("use feature:f1-codecs"). PRs
#15 and #16 relabelled accordingly.
**Supersedes:** none.

## 2026-08-29 — Trunk §4: a single invalid escape aborts the frame

**Context:** `docs/trunk-link-layer.md` §4 discards a frame at "≥ 8
consecutive stuffing violations" but never says how fewer than eight
decode. Feature 002 (trunk link layer) needs the host-core and Python
frame parsers to agree byte-for-byte, so the rule must be exact.
**Recommendation:** any invalid escape (`0x7D` followed by anything but
`0x5E`/`0x5D`) aborts the frame immediately, resynchronising on the next
FLAG; the "8" is read as a bound on how long a receiver may keep consuming
a babbling stream before abandoning the current frame, not as a tolerance.
**Ruling:** adopted — human, 2026-08-29 (feature 002 clarification Q1,
option A). Spec FR-002. The trunk document's wording could say so
explicitly; that edit is a human's (T3).
**Supersedes:** none.

## 2026-08-29 — Trunk §7: BUS_FAULT means every enrolled node SUSPECT at once

**Context:** §7 declares BUS_FAULT "if all nodes fail simultaneously"
without defining failure or saying whether one enrolled node counts.
**Recommendation:** BUS_FAULT when every enrolled node (HEALTHY, SUSPECT or
OFFLINE — never UNKNOWN) is SUSPECT or worse at the same time; a single
enrolled node counts as all, because the fallback re-probe — not the node
count — is what distinguishes a dead node from a dead bus. Declared once
per episode with one alert.
**Ruling:** adopted — human, 2026-08-29 (feature 002 clarification Q2,
option A). Spec FR-024.
**Supersedes:** none.

## 2026-08-29 — Trunk §7: after BUS_FAULT, alternate probe rates; the answering rate wins

**Context:** §7 says the host "re-probes at the fallback bit rate, and
surfaces a system alert" — nothing about retrying the reference rate, how
long, or what clears the fault.
**Recommendation:** while BUS_FAULT is declared, enrolment probes alternate
between the reference and fallback bit rates; the first valid response at
either rate clears the fault exactly once (recovery notification) and that
rate becomes the rate in use until the layer above changes it. Diagnoses
both "wrong rate" and "cable reconnected" with one policy.
**Ruling:** adopted — human, 2026-08-29 (feature 002 clarification Q3,
option A). Spec FR-026.
**Supersedes:** none.

## 2026-08-30 — Deep-verify mutation gate: pure-interface headers can never produce a mutant

**Context:** PR #91 (T007, `link/clock.hpp` + `link/byte_wire.hpp`) failed the
`deep-verify` / `Diff-scoped mutation` CI job: `tools/mutate.sh --diff
origin/main --require` reported `mutation: scope: link/byte_wire.hpp
link/clock.hpp` then `mode=diff ... mutants=0` and failed with "scope is
non-empty but Mull generated no mutants — failing (blind spot:
instrumentation is not reaching the code)" (`tools/mutate_report.py`).
Root cause: both files are pure abstract interfaces — every member is a
`= 0` pure-virtual declaration, no function body anywhere in either file.
Mull mutates operators/expressions inside function bodies; a file with none
can never yield a mutant regardless of which test binaries run, how the
diff-scoping ranges are computed, or whether a consumer/test includes the
header (confirmed: PR #91's follow-up commit added
`tests/unit/test_link_interfaces.cpp`, exercising both interfaces under
ASan/UBSan, and the gate still failed the same way — the body that would
need to move is in the *test*, not the *header*). This is structurally
different from the blind spot the check exists to catch (a real bug or
misconfiguration silently excluding mutable code from instrumentation) —
it is a class of file, not a class of bug, and any future PR touching only
declaration-only interface headers (`link/`, `l3/`, `core/`) hits the same
wall.
**Recommendation:** a narrow, explicit, per-file opt-out — `//
mutation-exempt(no-body): <justification>` anywhere in the file, checked
by `tools/mutate_report.py` only on the "no mutants in a non-empty scope"
path and only when every file named in the diff's scope ranges carries the
marker (any changed file in scope without one still fails the existing
way, so real blind spots on files that do have executable code stay
caught). Reviewed like `mutant-ok`, just at file granularity, so a human
sees and can dispute the claim "this file has no mutable code" in the PR
diff. Does not touch `tools/mutate.cfg [policy]`'s T3 constants
(`max_unlabelled_survivors`, `label_categories`) — those are unchanged and
still gate every file that does contain logic.
**Ruling:** pending — human. Implemented as the safe default per CLAUDE.md
("implement nothing speculative... proceed only if a safe default
exists"): `tools/mutate_report.py` (`NO_BODY_EXEMPT`, `exempt_reason`),
markers added to `link/clock.hpp` and `link/byte_wire.hpp`, doc comment in
`tools/mutate.sh`. If a human ruling instead prefers, e.g., excluding
declaration-only files from `scope_dirs` matching entirely, or a
tree-wide static check that a file has zero function bodies (removing the
need for a per-file marker), that should supersede this entry.
**Supersedes:** none.

---

## 2026-08-30 — T010 (MockWire) is scheduled before the Deframer it must call

**Question:** `specs/002-trunk-link-layer/tasks.md` places T010 (`tests/support/`
`FakeClock` + `MockWire` skeleton, issue #28) in Phase 2/Foundational, but
`contracts/mock-wire.md` requires `MockWire::transmit` to deframe every frame "with
the real `Deframer`" — and the `Deframer` is T022 (issue #40), Phase 3/US1, two
phases later. T010's own Out-of-scope forbids stubbing framing in `tests/support/`.
Found live: the dispatch agent claimed #28 (runs 33332211854, 33332538294), verified
`link/frame.*` does not exist, implemented nothing, and escalated `needs-human` with
this analysis — the contradiction is in the plan artefacts, not the story content.
**Options:** (1) pull T022 (and its Python-first US1 predecessors) ahead of T010 —
drags the whole vector/golden-commit chain before Phase 2 closes; (2) split T010
(skeleton + `Silence` now, `Respond` after T022) — two PRs and a contract amendment;
(3) re-slot T010/T011 after T022 — nothing in US1 consumes `MockWire` (its tests are
codec-level: vectors, torture corpus, fuzz); the first consumer is US2 (T028+).
**Recommendation:** (3), as the smallest coherent change: no contract edit, no task
split, US1 critical path unchanged.
**Ruling:** human, 2026-08-30 ("do it", this session): option (3). tasks.md dependency
notes amended in the same PR; issue #28 re-queued with `- #40 (T022)` as its blocking
dependency (promote-queued releases it when #40 closes); #29 (T011) already chains
behind #28. US1 stories no longer list #28/#29 as blockers.
**Supersedes:** none.

## 2026-08-30 — T013: torture-corpus `Element` shape and `frames` parameter meaning

**Context:** implementing `tools/refimpl/test_torture.py` (T013, issue #31) against
`torture.py`'s not-yet-written contract surfaced two mismatches between
`specs/002-trunk-link-layer/contracts/link-python.md` and `data-model.md` §11 that the
test's exact assertions must pick one reading of:
(1) contracts/link-python.md types `Element` as a flat 4-tuple `(stream: bytes,
expected: list[Frame], expected_discards: int, recipe: str)`; data-model.md gives a
richer shape (`seed`, a `segments: [{kind, bytes}]` list, and `expected_discards:
{reason: n}` as a per-reason tally). Neither document says the two are the same object
described at different detail, and the acceptance criteria (issue #31, sourced verbatim
from contracts/link-python.md's own "pytest" section) name exactly `stream`, `expected`,
`expected_discards`, `recipe` — never `segments` or a per-reason dict.
(2) `corpus(seed, frames: int = 10_000, ...)` — whether `frames` bounds the corpus's
total *element* count (valid + corrupted) or only its count of *valid, deliverable*
frames (SC-002: "a torture corpus of at least 10,000 frames with at least 1,000
corruptions of every class" reads corruptions as additional to, not part of, the
10,000). The issue's own enrichment comment flagged this second point for a human
second look; the releasing comment accepted the reading below without objecting but
asked for it to be rechecked at PR time.
**Recommendation:** (1) `Element` is a flat one-corruption-per-element record with
exactly the four contract fields — `stream` is the complete byte sequence for that
element (a lone valid encoded frame, or one corrupted rendering of one), `recipe` is
either `"valid"` or one of the eight corruption-class names, and `expected_discards` is
a single int (0 for a valid element, else the discard count that stream produces) —
matching the differential's own description in data-model.md §11 ("compares … the
total discard count"), not a per-reason dict. `segments`/per-reason tallying is
`torture.py`'s (T024) internal generation detail, not part of the public `Element`
contract this test pins. (2) `frames` counts delivered/valid frames only; total corpus
size is `frames` valid elements plus at least `per_class` corrupted elements per class
(≥ 8 × `per_class` beyond `frames`). `test_torture.py` asserts against this reading only
(sum of `len(element.expected)` ≥ `frames`; per-class tally of `recipe` ≥ `per_class`),
not against total element count.
**Ruling:** pending — safe default implemented per CLAUDE.md ("implement nothing
speculative … proceed only if a safe default exists"); flagged for the human review this
issue's releasing comment already asked for, at PR time for #31.
**Supersedes:** none.

---

## 2026-08-30 — Splitting a write-first test task from its implementation turns the merge gate red

**Question:** spec-002's decomposition puts write-first test files and the module they
import/include in separate issues two phases or several tasks apart (T012 vs T018,
T013 vs T024, T014–T017 vs T022). A bare `import`/`#include` of the missing module
fails pytest collection (or the C++ build) for the WHOLE tree, so the test task's PR
turns `ci-gate` red by design — incompatible with the green-CI merge gate. Observed
twice live: PR #99 (T012) and PR #100 (T013), each triggering a ci-failure-router
auto-fix cycle that resolved it by implementing the partner task in-attempt.
**Options:** (1) `pytest.importorskip` guards in write-first Python test files —
Python-only; a C++ test referencing a nonexistent header cannot skip, it cannot
compile; (2) accept per-pair router bundling as the mechanism — works, but collapses
the decomposition by accident each time and burns an auto-fix cycle; (3) make each
test+implementation pair ONE dispatch unit: one PR, test commits first with the
recorded failing run as rule-8 evidence, then the implementation, closing both issues
— the pattern PR #94 (T008+T009) already used cleanly, and consistent with CLAUDE.md's
"prefer small vertical slices" working agreement.
**Recommendation:** (3).
**Ruling:** human, 2026-08-30 ("do it", this session): option (3). tasks.md Phase-3
preamble and "Within Each User Story" amended; DEFINITION-OF-READY gains the
"Test-first pairs are one dispatch unit" section; the enricher is instructed to mark
partner stories as same-PR siblings; US1's remaining pair (T014–T017 + T022,
issues #32–#35 + #40) marked as one unit on the issues.
The working precedent for option (3) is PR #94 (T008+T009): both commits pushed
together, green throughout. PR #99 (T012+T018) and PR #100 (T013+T024) are NOT that
shape — each pushed its test commit alone, went red, and the second commit came from
an option-(2) router auto-fix cycle, accepted retroactively; they MOTIVATED this
ruling rather than exemplify it (red-team on PR #101, 2026-08-30). Accordingly the
unit is pushed ONCE, complete: the failing state exists only in local commit history,
evidenced by the recorded failing run in the PR body. Pushing the test commits alone
recreates the incident — and for C++ pairs a missing header aborts the entire native
build stage (every later pipeline stage with it), a larger blast radius than Python's
collection error.
**Supersedes:** none.

---

## 2026-08-31 — Agent approval below T3 (verdict-gated)

**Context:** every merge required one human review; with the WIP-cap-1 pipeline the
human review became the throughput bound for low-risk agent PRs. Green checks alone
were demonstrably insufficient to replace it: PRs #94, #99 and #100 were fully green
on every required check while the advisory Claude review then found real HIGH
defects (byte_time_us(0) UB; the Deframer TooLong-clobber phantom-frame bug; torture
corpus class mislabeling). In all three, the REVIEW was the control that worked.
**Recommendation:** allow the required approval to be satisfied mechanically, but
only by the control that worked: a clean machine-readable review verdict
(`VERDICT(review): clean @ <head sha>`, plus `VERDICT(red-team)` at T2) for the
exact pushed head, for `agent-authored` PRs only, fail-closed on anything
unresolved. Recommended starting tier: 1, extending to 2 after a month of verdict
accuracy per GOVERNANCE §6.
**Ruling:** human, 2026-08-31 ("do it at tier 2", this session):
`auto_approve_max_tier: 2` from the start — the T1 staging recommendation was
considered and overridden; the accepted risk is a reviewer miss on a clean verdict
at T2 (protocol-critical paths), named here with #94/#99/#100 as the evidence that
findings-bearing PRs are NOT approvable under this gate (all three carried
findings, so none would have auto-approved). Mechanics: claude-review reviews every
agent PR per push (`synchronize`) and emits the verdict; a separate minimal-
permission workflow (`agent-approve.yml`) approves as github-actions[bot]
(distinct from the authoring claude[bot]). Hardened per the Copilot review on
#103 (2026-08-31): the approval workflow triggers on `issue_comment`, so the
DEFAULT-BRANCH definition runs and its inputs (risk-tier resolver, the knob)
come from the default branch — a same-repo PR cannot rewrite the gate in its
own diff (pull_request-triggered workflows run the PR's version and never hold
approval logic); verdicts are accepted only from claude[bot] exactly, and only
as the final non-empty line of the comment (a quoted verdict token mid-comment
never counts); the bot dismisses its own stale approvals when the head
moves and never touches a human review; T3 is never auto-approved; CODEOWNERS
paths still require the owner; the merge click remains human (GOVERNANCE §1).
Kill switch: `auto_approve_max_tier: -1` or disable the workflow. Monthly review
(§6) tracks auto-approved PRs later found defective; sustained misses lower the
tier.
**Supersedes:** none.

---

## 2026-08-31 — Model tiers for agent workflows

**Context:** every Claude CI workflow ran claude-code-action's default model
(claude-sonnet-5, verified in run logs; no workflow set a model). With approvals now
gated on review/red-team verdicts (ruling above), the quality of the judgement loops
directly bounds what can merge with one human click.
**Recommendation:** run the judgement-heavy, low-volume loops on claude-opus-5 —
claude-review (backs approvals), red-team (both modes), story-enrich (planning:
decomposition + tier prediction), agent-converge-audit (weekly spec drift),
agent-triage (failure diagnosis) — and keep the high-volume implementation loops
(agent-dispatch implement, ci-failure-router auto-fix, claude-mention) on the
default: their output passes through the full mechanical gate stack plus the
now-opus reviews, so extra model cost buys less there. Pinned per workflow by a
wiring test so the split is a reviewable diff, not a drift.
**Ruling:** human, 2026-08-31 ("lets use higher models on critical areas, planning,
reviews etc", this session): adopted as recommended. Assumed until the first run:
claude-opus-5 is available to the CLAUDE_CODE_OAUTH_TOKEN subscription — if a run
fails on model access, the flag is the one-line revert.
**Supersedes:** none.

---

## 2026-08-31 — SC-008's "exactly 142 bytes" worst-case frame is unreachable

**Context:** dispatching #38 (T020, `frame_*` golden vectors), the agent proved the
`frame_worst_stuffing` acceptance criterion unsatisfiable and escalated `needs-human`
without landing anything: spec.md SC-008 demanded a 142-byte wire frame, but
`docs/trunk-link-layer.md` §4's layout makes `ctrl` (low nibble ≤ 0x3: response, retry,
seq only) and `len` (≤ 0x40) structurally incapable of requiring stuffing — so at most
68 of the 70 unstuffed body bytes can escape, ceiling 140. Verified independently by
execution: an exhaustive sweep of ALL 4,177,920 legal frames with the vector's defined
64 × 0x7E payload (every dst 0x00–0xFE × src × response × retry × seq) gives a maximum
of **139 bytes** (e.g. dst = src = 0x7D); the agent's own brute force found the same
number. 142 = `kMaxWire`, the conservative buffer-sizing formula
`2 + 2 × (4 + 64 + 2)` — a bound, conflated in SC-008 with an achievable length.
**Options:** (a) correct SC-008 and the contract row to the measured achievable
maximum (first measured 139 for the all-0x7E payload; final 140 — see Ruling; bound
stays 142, untouched); (b) keep "142" reworded as the sizing bound only — leaves the vector's
expected length unstated, the exact ambiguity that cost this dispatch cycle;
(c) make `ctrl`'s reserved bits usable so 142 becomes reachable — a wire-protocol
change in service of a test vector.
**Recommendation:** (a).
**Ruling:** human, 2026-08-31 ("do it", this session): option (a), with the number
settled by two rounds of execution. First pass set the target to 139 (the exhaustive
maximum for the vector's then-specified all-0x7E payload). The Opus review of PR #104
(finding 4) conjectured from CRC affinity that 140 — the structural ceiling — is
reachable with a mixed 0x7D/0x7E payload; confirmed by execution: dst = src = 0x7D,
response = 1, retry = 1, seq = 11 with the payload recorded in
contracts/frame-vectors.md yields CRC 0x7D7E (both CRC bytes escape) → exactly
140 wire bytes. Final ruling: SC-008 says 140 bytes / 1.40 ms with that pinned frame
as the vector; "legal" is scoped as encoder-legal (dst ≠ 0xFF) — a §5-conformant
trunk address plan yields less; `kMaxWire = 142` stays the untouched sizing bound.
tasks.md T014, plan.md and research.md R-02/R-09 stale "142 on the wire" claims
corrected in the same PR (review findings 1 and 3). Issue #38's acceptance criteria
pin the same frame; the story is re-released; the `tests/vectors/` commit remains
human-triggered (CLAUDE.md rule 9).
Round three (review pass 3 on PR #104): the review correctly flagged that the
ceiling sentences were scoped only to encode — a RECEIVED frame may carry reserved
`ctrl` bits (spec Edge Cases) and an escaping `ctrl` makes 69 escapable bytes, a
naive wire length of 141 —
but its 141 receive-path ceiling overclaimed: CRC-16/CCITT-FALSE's generator has the
factor x+1, so CRC parity follows message parity, constant across every
all-escapable body; the four both-CRC-bytes-escaping values are even-weight while
that class is odd — for EVERY body in the class, by construction (review pass 4
re-derived the same proof independently: G(1)=0 since 0x1021 has weight 4; the
0xFFFF init flips 16 bits, preserving message parity; so codeword weight is even and
CRC parity equals message parity, which is 403 mod 2 = odd for every 141-candidate
body, while all four both-escape CRC values have popcount 12, even). The 24k-body
corner-combination run corroborates (one parity class observed, zero hits) but is
NOT the warrant — at ~1.46 expected unconstrained hits, zero hits would also occur
~23% of the time if the claim were false. 141 (wire length; 69 escapable bytes) is
unreachable; 140 is the wire maximum on both paths. The six affected sentences
(four from round three, two more in research.md R-02 found by review pass 4) are
now scoped accordingly.
**Supersedes:** none.

---

## 2026-08-31 — A bot approval satisfied require_code_owner_reviews (live gate bypass)

**Context:** the first live auto-approval (github-actions[bot] on PR #104, per the
"Agent approval below T3" ruling) produced `reviewDecision: APPROVED` and
`mergeState: CLEAN` on a PR that changed two CODEOWNERS-listed paths
(`docs/OPEN-QUESTIONS.md`, `specs/002-trunk-link-layer/tasks.md`) with
`require_code_owner_reviews: true` on `main`. The design assumption stated across
that ruling — "a bot approval never satisfies code-owner review" — was falsified by
the live event: the merge button went green with no human review. Demonstrated, not
theoretical. (Merge itself still required a human click; auto-merge is disabled.)
**Recommendation:** make the gate independent of GitHub's approval-counting
semantics: `agent-approve` refuses to approve any PR whose changed files match any
CODEOWNERS pattern (patterns read from the default-branch checkout; unreadable
CODEOWNERS refuses too — fail closed), so owned paths always reach the owner
unreviewed-by-bots. Also enable `dismiss_stale_reviews` on `main` as the platform
brace behind the gate's own stale-approval dismissal.
**Ruling:** human, 2026-08-31 ("do it", this session): adopted.
`dismiss_stale_reviews` enabled (verified true). The fail-closed check lands in
`agent-approve.yml` with harness cases for exact, `**`, directory and root-file
patterns plus the unreadable-file path. The risk-score T3 regex is deliberately NOT
extended to `specs/` (agents are sanctioned to produce those artefacts; with this
fix, owned specs paths cannot auto-approve regardless of label — the label gap is
accepted and revisitable).
**Supersedes:** none (extends "Agent approval below T3", same date).

---

## 2026-08-31 — Deframer must discard, not deliver, a frame addressed to reserved dst 0xFF

**Context:** CI run 33432903844 (PR #108, `ci-failure-router` auto-fix) failed
`deep-verify`'s `fuzz_frame` target after only 13,618 runs: `fuzz: fuzz_frame ...
findings=1 exit=77`, `ERROR: libFuzzer: deadly signal`. `tests/fuzz/fuzz_frame.cpp`
asserts that any frame the `Deframer` delivers must re-encode via `encode_frame` to the
same fields (round-trip stability) — a property neither the contract nor
`docs/trunk-link-layer.md` states explicitly but that any frame codec needs to hold, and
that the vectors/property tests never exercise because they deliberately exclude `dst ==
0xFF` from every frame they generate (`tests/property/test_link_resync.cpp:34`,
`test_link_stuffing.cpp:40`, `tools/refimpl/torture.py:53`). Root cause: `encode_frame`
refuses `dst == 0xFF` (`Status::ReservedAddress`, trunk §5: "MUST NOT be used in v1"),
but the `Deframer`'s `on_flag()` never checked the address — a wire frame carrying it
(bit corruption, or a future sender ignoring the rule) was delivered as a `FrameFields`
that `encode_frame` then refused, tripping the fuzz harness's own
`__builtin_trap()`. Identical gap in the Python reference (`tools/refimpl/omgp_link.py`
checked `_RESERVED_DST` only in `encode_frame`, not `_on_flag`), confirming the bug
predates the C++ port (frame.cpp's own header comment: "Ported 1:1 from ...
omgp_link.py"). Neither `docs/trunk-link-layer.md` §4's discard list (`BadCrc, BadLength,
≥8 stuffing violations`) nor §5's reserved-address clause say what a *receiver* must do
with an incoming `dst == 0xFF` frame — §5 only constrains transmission.
**Recommendation:** treat receipt of `dst == 0xFF` the same as any other structurally
invalid frame: discard it silently and count it, mirroring the encode-side refusal
instead of contradicting it. Add `Discard::ReservedAddress` (new last entry before
`COUNT`, preserving existing indices 0-3) to both `link/link_types.hpp` and
`tools/refimpl/omgp_link.py`'s `stats` dict; `Deframer::on_flag`/`_on_flag` check `dst ==
0xFF` immediately after the CRC check passes (so a corrupted-CRC frame that happens to
decode to `dst == 0xFF` is still counted `BadCrc`, not `ReservedAddress`) and discard
before incrementing `delivered`.
**Ruling:** adopted as the safe default per CLAUDE.md ("implement nothing speculative …
proceed only if a safe default exists"); this is the minimal change that restores the
codec's round-trip invariant without touching `protocol/omgp-protocol.yaml`,
`tests/vectors/`, or the trunk document itself (human-ruling artefacts, untouched).
Implemented in `link/frame.cpp`, `link/link_types.hpp`,
`tools/refimpl/omgp_link.py`, with new unit/pytest coverage
(`tests/unit/test_link_frame.cpp`, `tests/unit/test_link_types.cpp`,
`tools/refimpl/test_link.py`) written first and confirmed red before the fix. Pending a
human second look: whether `docs/trunk-link-layer.md` §4's discard list should name this
reason explicitly (a documentation edit only, no behaviour change) — left to a human as
that document is a human-ruling artefact.
**Supersedes:** none.

---

## 2026-08-31 — workflow_run events are not delivered for auto-fix pushes: the router cannot see its own attempt's failure

**Context:** PR #108's auto-fix (attempt 1) pushed commit 58e4e91; the resulting `ci`
run failed deep-verify (9 unlabelled mutation survivors) at ~21:10 — and NO
`ci-failure-router` run was created for it, so attempt 2 never fired and the PR
stalled red at `auto-fix-1`. The only router run near that time was for an unrelated
`main` event. Root-cause hypothesis (consistent with every observation): the auto-fix
agent pushed using the checkout's persisted `GITHUB_TOKEN` credentials — the same
mechanism that made the push's `synchronize` actor `github-actions` (the bug PR #109
patches around in allowed_bots) — and GitHub suppresses `workflow_run` chaining off
runs originating from such pushes. Net effect: the router's core loop (fix → push →
re-run CI → route the result) silently loses exactly the failures it exists to route.
A manual `gh run rerun <ci-run> --failed` was tried as remediation and DID NOT work:
the rerun completed (failure, 21:32) and produced no router run either — re-runs are a
second suppressed delivery path (demonstrated, not hypothesized). With no
workflow_dispatch trigger on the router, attempt 2 currently cannot fire by any
existing mechanism.
**Options:** (a) `persist-credentials: false` on the router autofix job's checkout, so
the agent's `git push` uses the claude App token — fixing the actor at the source
(synchronize events then run as claude[bot], already in allowed_bots) AND restoring
workflow_run chaining; (b) a scheduled router sweep (e.g. 2-hourly) that scans open
agent PRs for a failing required check with no fresh attempt marker and routes them —
a delivery backstop independent of event semantics; (c) both — (a) as the fix, (b) as
the belt, mirroring the dispatcher's own nudge+cron pattern.
**Recommendation:** (c). (a) alone leaves any future event-suppression variant
undetected; (b) alone leaves the first ~2 hours dark. After the rerun evidence, (b)'s
sweep is not a belt but the only delivery path that does not depend on
`workflow_run` semantics at all.
**Ruling:** (c), 2026-09-02 — human, both parts implemented in `ci-failure-router.yml`.
(a) the `autofix` checkout takes `persist-credentials: false`, so the agent's push carries
the Claude App token. (b) a `sweep` job on a 2-hourly cron scans open agent PRs for a
failed `ci`/`security` run at the current head with no `ci-failure-router sha=<sha>
pass=<n>` marker and re-dispatches the router for it via `workflow_dispatch`
(input `run_id`), which doubles as the manual path for a suppressed failure. The sweep
decides NOTHING: `route` holds the bounds and escalation for both entry paths, which is
why `route` now reads the failed run from the API when no event delivered one, and
`autofix` reads the branch/sha/run from `route`'s outputs rather than the event context.
Regression cover: `test_ci_failure_router_delivery_backstop` and the persist-credentials
assertion in `test_ci_failure_router_wiring` (tools/refimpl/test_workflow_scripts.py).
NOT EXAMINED: neither part is demonstrated against live GitHub — the delivery semantics
that caused this entry cannot be exercised in the mocked harness, so the first real
auto-fix push after this change is the evidence that (a) works.
**Supersedes:** none (extends the PR #109 findings; same date).

---

## 2026-09-01 — MockWire (T010) design choices: Respond's answer, wildcard-script ordering, and deferred-REQUIRE capacity checks

**Context:** review on PR #111 (T010, `tests/support/mock_wire.{hpp,cpp}`) flagged three
`MockWire` decisions as unrecorded interpretations rather than code defects, per
CLAUDE.md's rule that spec ambiguity is resolved here, not in a code comment.
(1) `contracts/mock-wire.md`'s `Kind::Respond` says "the node's `RequestHandler`
(usually a real `Responder`) answers" — but neither `RequestHandler` nor `Responder`
exists yet (`Responder` is US2/T031). `schedule_respond()` instead echoes the request
payload back to the sender as its own deterministic answer.
(2) `next_step()` reads an exhausted per-node script as falling through to the shared
`node == 0xFF` wildcard script before the documented default (`Respond`,
`TRUNK_T_turn_min_us`) applies; the contract's "an exhausted script behaves as Respond
with the default delay" is also readable as own-script exhaustion going straight to the
default, with `0xFF` only covering nodes that never had a script at all.
(3) Three "never a silent drop" checks that used to be plain `REQUIRE` at the point of
failure (RX-queue capacity, transcript capacity, `encode_frame` refusing a `Respond`
answer) all run on the call stack of `transmit()` — which `Master`/`Responder`
(T028/T031) call, and which `link/CMakeLists.txt` builds with `-fno-exceptions`. A
`REQUIRE` thrown from there would unwind through those frames, which is undefined
behaviour and loses the diagnostic exactly when it fires. Fixed by recording a
`fault_` message instead and `REQUIRE`-ing it drained at the next `advance_to()` call
(always on the test's own stack) — but this defers the failure by up to one
`advance_to()` step rather than reporting the instant an overflow occurs, which
pressures the contract's "overflow is a REQUIRE failure" wording.
**Recommendation:** (1) keep the echo as the interim `Respond` answer — it is the
smallest thing that makes `Respond` usable before `Responder` exists, and T034 already
plans to replace the mock's per-node handling with a real `Responder`; no contract
change needed, just this record of the interim behaviour. (2) keep own-script →
wildcard → default as the combined per-node sequence (implemented, `mock_wire.cpp`'s
`next_step()`) — it lets a wildcard script express rig-wide default behaviour (e.g. "every
node responds late unless scripted otherwise") without every test authoring a
17-element per-node script; if a future test needs "wildcard applies only to
never-scripted nodes", that is a distinguishable, additive change (an `own_script_set`
flag), not a revert. (3) accept the deferred-REQUIRE reading of "overflow is a REQUIRE
failure" — a failure that fires with a one-`advance_to()`-call lag but before the test
draws any conclusion from the wire's state is still "the test fails and says why", which
is what the contract clause protects against (a silent drop the test never notices).
**Ruling:** pending — human. Implemented as the safe default per CLAUDE.md ("implement
nothing speculative … proceed only if a safe default exists"); T011 (#29) and T028/T031
should be written against these three readings, or a superseding entry should replace
them first.
**Supersedes:** none.

---

## 2026-09-02 — `Step::count` (`uint16_t`) cannot represent `Kind::Rate`'s bit-rate values

**Context:** review on PR #111 (T010, `tests/support/mock_wire.hpp`) flagged that
`contracts/mock-wire.md`'s `Step` struct (and `data-model.md` §10, `research.md` R-07,
which all agree with each other) types `count` as `uint16_t`, but the same contract
defines `Kind::Rate` as "the node now hears only at `count` interpreted as bit rate
(1 000 000 or 115 200)" — neither value fits in `uint16_t` (max 65 535). The
inconsistency is in the spec artefacts themselves, not introduced by this PR's code,
which copies the documented width verbatim. Not yet load-bearing: `Rate` is
unimplemented until T030 (surfaced as a loud `fault_` if scripted today, per the
2026-09-01 MockWire entry above), so no script has attempted to author a `Rate` step's
`count` yet.
**Recommendation:** widen `count` to `uint32_t` in `contracts/mock-wire.md`,
`data-model.md` §10, `research.md` R-07 and `tests/support/mock_wire.hpp::Step`
together, in the T030 change that first gives `Rate` a body — a struct-layout change
across three human-ruling documents plus code is a single T3 slice, not a T010 fix.
Until then `count` stays `uint16_t` (matches every current spec artefact); a future
`Rate` step's `count` cannot yet be authored at either documented bit rate, which is a
pre-existing spec gap, not a new one.
**Ruling:** pending — human, to land with T030.
**Supersedes:** none.

---

## 2026-09-02 — review findings had no fix loop: the L3 cycle stopped at "findings"

**Context:** `claude-review` and `red-team` are read-only by design and end in a
machine-readable verdict; the only consumer of that verdict, `agent-approve`, merely
WITHHOLDS approval when it reads `findings`. Nothing anywhere was triggered by a findings
verdict. The single Claude-writes-code loop in the repository, `ci-failure-router`'s
`autofix`, fires on `workflow_run` completion of `ci`/`security` — CI failures only. Net
effect: an agent PR with green CI and open review findings could not be advanced by any
agent, and sat until a human either fixed it or closed it. The review pass ran, the verdict
was correct, and the cycle simply had no edge leading out of it.
**Options:** (a) leave it — a human addresses every finding, and the review pass is
advisory only; (b) let `claude-review` fix what it finds in the same run — rejected: the
authoring and reviewing passes must stay separate, and a reviewer that edits the code it is
judging cannot produce an independent verdict; (c) a separate bounded `review-fix` loop
triggered by the findings verdict, on the agent's own branch, with the same shape of bounds
and escalation as the CI router.
**Recommendation:** (c).
**Ruling:** (c), 2026-09-02 — human. Implemented as `.github/workflows/review-fix.yml`;
GOVERNANCE.md §4 records the loop. Two decisions worth naming:
1. **Severity policy (human direction):** HIGH and MEDIUM findings are fixed; a LOW finding
   is fixed only where the agent is already changing that code, otherwise it is listed as
   consciously deferred. LOW-only churn burns attempts and re-review cycles for no risk
   reduction. `claude-review` and `red-team` now emit `[HIGH]`/`[MEDIUM]`/`[LOW]` prefixes
   so the policy has something to route on.
2. **Accepted consequence:** a deferred LOW keeps the verdict at `findings`, so
   `agent-approve` will never auto-approve that PR and a human merges it. Making deferred
   LOWs read as clean would change the meaning of the verdict the approval gate parses —
   a T3 change to the 2026-08-31 approval ruling — and is NOT done here. If the deferral
   turns out to block enough PRs to matter, that is the question to reopen.
Regression cover: `tests/workflows/review_fix_harness.js` (24 cases: verdict discipline,
who may be pushed to, one attempt per head commit, the two-attempt bound and its
escalation) plus `test_review_fix_wiring`.
NOT EXAMINED: the loop has not run live; the severity policy's effect on how many PRs
reach `needs-human` is unmeasured, and `delivery-metrics` does not yet record review-fix
attempts.
**Supersedes:** none.

---

## 2026-09-02 — the merge click was the last human step: autonomous merge below T3

**Context:** with `review-fix` closing the findings edge (entry above), every step of the
cycle except one was autonomous: dispatch, implement, review, red team, fix, approve. The
merge itself still waited on a human, so a PR could sit complete, approved and green for as
long as it took someone to click — the queue, not the work, became the constraint.
**Options:** (a) keep the human merge click (GOVERNANCE.md §1 as written); (b) GitHub native
auto-merge — enable it on approval and let GitHub merge when checks pass; (c) a workflow that
performs the merge itself, pinned to the head the verdicts were issued for.
**Ruling:** (c) with a T2 ceiling, 2026-09-02 — human. Implemented as
`.github/workflows/agent-merge.yml`, knob `auto_merge_max_tier: 2`; GOVERNANCE.md §1, §2
and §4 updated.
Why NOT (b): branch protection has `dismiss_stale_reviews` OFF (the reason `agent-approve`
dismisses its own stale approvals). Under native auto-merge a push landing after approval
could be merged on the older head's approval — an unreviewed head reaching main through a
mechanism nobody watched. (c) passes `sha` to the merge API, so exactly that race ends in a
409 and no merge. Native auto-merge becomes viable if `dismiss_stale_reviews` is ever turned
on, and would then be the simpler mechanism.
**CODEOWNERS scope (human direction):** the owner keeps every owned path EXCEPT
`docs/OPEN-QUESTIONS.md` and `specs/**/tasks.md` — the two OPERATING-POLICY §2 already
sanctions agents to write. Without that exception nearly every PR recording a spec ambiguity
would still have waited for a human, and the loop would have been autonomous in name only.
The alternative offered — merging owner paths too, relying on the PR #104 finding that a
github-actions[bot] approval satisfies `require_code_owner_reviews` — was declined: it would
leave the risk label as the only thing between an agent and `protocol/` or `.github/`.
Regression cover: `tests/workflows/agent_merge_harness.js` (37 cases, including one per
protected CODEOWNERS class, the 409 race, and every fail-closed refusal) plus
`test_agent_merge_wiring` and `test_codeowners_still_protects_ground_truth_and_governance`,
which fails if the exemption list ever widens or an owned path is dropped.
NOT EXAMINED, and worth watching on the first live merges: (1) whether `GITHUB_TOKEN` is
permitted to merge under the current branch-protection rules at all — if it is not, the
workflow comments once per head and leaves the PR, it never retries around the rule; (2) the
check-completeness rule assumes every required check appears as a check RUN at the head — a
required check that only ever appears as a legacy commit status is covered by the combined
status call, but a required check that has not started at all reads as "no such check", not
as "pending", so a protection rule requiring it is the backstop, not this workflow; (3) the
kill switch is now split — revoking the Claude token stops the agents but NOT the merger.
**Supersedes:** extends the 2026-08-31 approval ruling (auto-approval ≤ T2); does not change
it.

