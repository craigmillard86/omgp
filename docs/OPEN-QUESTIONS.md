# Open Questions

Append-only log of spec ambiguities encountered during implementation, and
their resolutions. Entries are dated and append-only. Exactly one in-place
edit is part of the format: filling a `pending` **Ruling:** field with the
decision it was waiting for (that is what the placeholder is for). Every
other line of a landed entry is never edited or deleted; SUBSTANTIVE
superseded text that a filled ruling replaces is quoted verbatim inside the
ruling — the placeholder stub itself (the word `pending` plus a scheduling
note of who or when would rule, e.g. "pending — human, with T062") needs no
quoting, since it records no decision or fact. A
decision is changed by appending a new, superseding entry that references
the entry it supersedes — never by editing history. (Lifecycle written
down 2026-09-03 per review on PR #122, the precedent-setting instance.)

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
**Ruling:** human, 2026-09-03 (open-questions session, one-at-a-time Q&A): ratified — all seven defaults stand as the ruling; future changes need superseding entries. The superseded pending text read, verbatim: "rule inline on the Phase 1 PR of feature 001, or append a superseding entry per item."
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
**Ruling:** closed 2026-09-03 (session Q&A) as superseded by the 2026-08-29 triage-gate ruling: no percentage is ever enforced; the whole-tree rate is a trend only.
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
**Ruling:** closed 2026-09-03 (session Q&A) as superseded by the 2026-08-29 triage-gate ruling, with the entry above.
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
**Ruling:** human, 2026-09-03 (open-questions session, one-at-a-time Q&A): rewording adopted as recommended; docs/trunk-link-layer.md §8's first bullet is amended in the same PR (CODEOWNERS review is the ruling's gate). The landed text also carries the recommendation's forward-progress clause (`ERR_BUSY` never abandons the module-bus transaction) and two scoping sentences added per red-team review on #122: the MUST applies to accepted polls only (§4 silent discard stands), and an `ERR_BUSY` answer is a valid response for §7's failure accounting. The superseded pending text read, verbatim: "the trunk document is a human-ruling artefact; not edited by the agent." AUTHORISATION RECORD (review round 4 on #122): the maintainer directly instructed the session agent to prepare this §8 amendment as part of the 2026-09-03 rulings PR — a ONE-OFF authorisation exercised through a CODEOWNERS-gated PR the maintainer reviews; it does not generalise. Whether OPERATING-POLICY §2 should gain a matching carve-out is the maintainer's own decision, in the policy, by their own hand.
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
**Ruling:** human, 2026-09-03 (open-questions session, one-at-a-time Q&A): ratified as implemented; the per-file marker is the mechanism. The superseded pending text read, verbatim (quoted per review on #122 so the prior wording survives byte-for-byte): "Implemented as the safe default per CLAUDE.md (\"implement nothing speculative... proceed only if a safe default exists\"): `tools/mutate_report.py` (`NO_BODY_EXEMPT`, `exempt_reason`), markers added to `link/clock.hpp` and `link/byte_wire.hpp`, doc comment in `tools/mutate.sh`. If a human ruling instead prefers, e.g., excluding declaration-only files from `scope_dirs` matching entirely, or a tree-wide static check that a file has zero function bodies (removing the need for a per-file marker), that should supersede this entry."
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
**Ruling:** human, 2026-09-03 (open-questions session, one-at-a-time Q&A): both readings ratified (flat four-field Element; frames counts delivered/valid frames). The superseded pending text read, verbatim (quoted per review on #122): "safe default implemented per CLAUDE.md (\"implement nothing speculative … proceed only if a safe default exists\"); flagged for the human review this issue's releasing comment already asked for, at PR time for #31." — this ruling is that review.
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
**Ruling:** human, 2026-09-03 (open-questions session, one-at-a-time Q&A): all three defaults ratified (interim echo until T034; first-matching-step order; deferred REQUIRE on capacity). The superseded pending text read, verbatim (quoted per review on #122): "Implemented as the safe default per CLAUDE.md (\"implement nothing speculative … proceed only if a safe default exists\"); T011 (#29) and T028/T031 should be written against these three readings, or a superseding entry should replace them first." That instruction stands and is forward-looking: T011/T028/T031 are unimplemented at this head and are TO BE written against these readings.
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
**Ruling:** human, 2026-09-03 (open-questions session, one-at-a-time Q&A): widen `count` to uint32_t atomically in T030 across all four artefacts; baked into issue #48's criteria.
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

---

## 2026-09-02 — CodeQL on review-fix.yml: privileged checkout accepted, moving tag pinned

**Context:** CodeQL's Actions pack raised two alerts against the new `review-fix.yml` on
PR #113, failing the required `CodeQL` results check (GOVERNANCE.md §2, ruled 2026-08-28):
alert 187 (medium, `actions/unpinned-tag`) for `anthropics/claude-code-action@v1`, and
alert 186 (high, `actions/untrusted-checkout`) for the `fix` job checking out the PR's own
branch in a privileged, secret-holding workflow triggered by `issue_comment`.
The high alert describes a real and DELIBERATE exposure, not a defect: a fixer that cannot
check out the branch it is fixing cannot run `./pipeline.sh` on it or push the fix.
`ci-failure-router`'s `autofix` job does exactly the same thing and is not flagged only
because its trigger is `workflow_run` rather than `issue_comment` — the exposure is
identical, the query's heuristic differs.
**Options:** (a) dismiss 186 as accepted, keeping the explicit `ref:` checkout; (b)
restructure to `claude-mention`'s shape — check out the default branch, then switch to the
branch in a controlled `run:` step — which stops the query matching WITHOUT changing what
the agent then executes; (c) leave the check red pending a repo-wide pinning and
untrusted-checkout pass covering both fix loops at once.
**Recommendation:** (a). (b) buys a green check by moving the same code past the heuristic,
which is worse than recording the decision: the alert becomes invisible instead of judged.
**Ruling:** (a), 2026-09-02 — human. Alert 186 dismissed as "won't fix" citing this entry.
Alert 187 fixed properly instead of dismissed: the action is pinned to commit 8251c103 (the
commit `v1` resolved to on this date) in `review-fix.yml` only.
The mitigation that makes (a) acceptable is the `gate` job, which runs from the
DEFAULT-branch definition and refuses forks, non-`agent-authored` PRs, non-`task/*` heads
and `needs-human` PRs BEFORE the privileged job starts — so the checked-out code is always
claude[bot]'s own work inside this repository, never a fork contributor's.
NOT EXAMINED: the other workflows still track the moving `v1` tag and still carry the same
untrusted-checkout shape; neither was touched here because they are not new code on this PR.
A repo-wide pinning pass and a single ruling covering `ci-failure-router`'s `autofix` under
the same reasoning are still open — option (c) reduced to a follow-up rather than a blocker.
If the dismissal is ever reverted, the required check fails again and this entry is the
place to re-argue it.
**Supersedes:** none.

---

## 2026-09-02 - the first autonomous merge worked; the claim it left behind stalled the loop

**Context:** PR #114 (T021, `risk:t1`) was reviewed clean and merged by `agent-merge` at
21:25 with no human in the loop - the cycle working end to end for the first time. It then
stopped: issue #39 stayed OPEN with `in-progress`, because the PR body wrote
`Closes T021 (issue #39)` and GitHub only honours a closing reference that directly follows
the keyword. `agent-dispatch`'s WIP cap counts open issues labelled `task,in-progress`, so
every one of the eight dispatch runs over the next ~10 hours logged "WIP cap: a task is
already in progress - not pulling" and did nothing, with 26 `queued` and 2 `ready` tasks
behind it. Nothing failed; nothing was red; the loop was simply wedged, and only a human
reading the labels would notice.
**Options:** (a) tighten the dispatch prompt so the agent writes `Closes #n` exactly;
(b) have `agent-merge` release the claim itself after a successful merge, from the body's
references AND the branch's `task/<n>`; (c) have a sweep detect `in-progress` issues whose
PR has merged and release them after the fact.
**Recommendation:** (b) with (a). (a) alone is an instruction to an LLM about prose - the
same class of thing that just failed. (b) makes the release mechanical at the exact moment
the fact becomes true. (c) is a slower rediscovery of the same state and is unnecessary once
(b) holds.
**Ruling:** (b) + (a), 2026-09-02 - human. `agent-merge` now removes `in-progress` and closes
each target after merging (404 = non-event; anything else is a `warning`, because an
unreleased claim silently blocks every later dispatch). The dispatch prompt additionally
states the exact required form and cites this incident. Issue #39 was closed and its claim
released by hand to unblock the queue.
Regression cover: four new cases in `tests/workflows/agent_merge_harness.js` (41 total),
including the malformed-prose case reproduced from #114 verbatim.
NOT EXAMINED: the same wedge is reachable through paths this fix does not cover - a PR
CLOSED unmerged, or an implement run that dies after claiming, both leave `in-progress` with
no merge event to hang the release off. `ci-failure-router` releases the claim only on
auto-fix exhaustion. A periodic claim-reaper (option (c)) remains the honest answer for
those; it is not implemented here.
**Supersedes:** none (extends the 2026-09-02 autonomous-merge ruling).

---

## 2026-09-03 - review-fix bound raised from 2 to 4, and made a config knob

**Context:** PR #116 (T023) was the loop's first live run and it exhausted the two-attempt
bound in 38 minutes: attempt 1 fixed a MEDIUM contract divergence, attempt 2 fixed a real
`strtoul` range bug in `parse_uint`, and the third review still found 2 MEDIUM + 6 LOW. The
bound did what it was written to do, but it stopped a loop that was demonstrably still
converging - each round fixed genuine defects rather than churning. Two attempts was a guess
made before any live evidence existed.
**Options:** (a) leave it at 2 and let a human take every PR that needs a third round;
(b) raise it to a larger fixed number in the workflow; (c) raise it AND move the number to
`.github/agent-config.yml` so it can be retuned from evidence.
**Recommendation:** (c). The number is a tuning parameter, not a safety property - the safety
properties are one-attempt-per-head-commit, the labels being the bound, and nothing but a
human resetting them, all unchanged. Keeping it in the workflow also makes it the one thing
the fixer agent can never adjust: `.github/workflows/*` needs a `workflow` OAuth scope the
Claude App token does not carry (demonstrated on #113).
**Ruling:** (c), 2026-09-03 - human direction. `review_fix_max_attempts: 4`; the gate reads it
from the DEFAULT-branch config at run time and fails closed if it is unreadable, absent or
below 1. Labels `review-fix-3`/`review-fix-4` provisioned in `tools/gh-setup.sh`. The
attempt and exhaustion comments state the configured bound rather than a hard-coded 2.
Regression cover: the harness derives its expectations from the real config file, so the
bound and the tests cannot drift apart (`review_fix_harness.js`, 29 cases).
NOT EXAMINED: whether 4 is right either. The evidence for it is one PR. What is now cheap is
changing it: a one-line edit to a non-workflow file. Worth revisiting once
`delivery-metrics` records attempt counts - it does not today, so the only way to see how
often the bound is hit is to read PR labels by hand.
**Supersedes:** the two-attempt bound in the 2026-09-02 review-fix ruling; nothing else in
that entry changes.

## 2026-09-03 — Frame line out-of-range fields: C++ rejects, Python reference masks/accepts

**Context:** review on PR #116 (T023, @ d30ef1c) flagged that `tools/canonical.cpp`'s
`parse_frame_line` rejects out-of-range `dst`/`src`/`flags`/`seq`/`payload` tokens as `ERR
BadRequest`, while `tools/refimpl/canonical.py`'s `canonical_to_frame` accepts the same tokens
and either masks them (`omgp_link.py:97`, `dst`/`src`/`seq`) or raises a Python exception with
no canonical rendering. Three concrete cases: `seq=16` -> C++ `ERR BadRequest`, Python masks to
`seq=0` and returns `OK`; a 256-byte `payload=` -> C++ `ERR BadRequest`, Python `ERR
PayloadTooLong` — inverting the PR body's own stated intent that above-limit payloads keep
their contract spelling instead of collapsing into `BadRequest` (true for 65-255 bytes, false
at exactly 256); `dst=0x100` -> C++ `ERR BadRequest`, Python raises `ValueError` (neither
`CanonicalError` nor `FrameError`, so `canonical.py:293`'s error mapper has nothing to render).
`contracts/frame-vectors.md` does not define behaviour for out-of-range frame-line fields at
all, so neither side contradicts the contract — they contradict each other, and nothing pins
which is normative before `tools/diffcheck.py --frames` (T025, issue #43) compares them
line-for-line.
**Recommendation:** the C++ side (reject as `ERR BadRequest`) becomes normative, and the Python
reference is brought in line with it as part of T025: rejecting malformed/out-of-range text
before it reaches the codec is the stricter, fail-closed behaviour, matches this task's existing
choice to reject rather than mask (`tools/canonical.cpp` comment above `parse_frame_line`), and
keeps a caller error from silently being reinterpreted as a different, valid request.
**Ruling:** human, 2026-09-03 (open-questions session, one-at-a-time Q&A): C++ strict rejection is normative; T025 aligns the Python reference (no seq masking); baked into issue #43's criteria. Precision (per red-team on #122): `ERR BadRequest` covers malformed input text AND, for `FDEC`, well-formed hex whose bytes run out mid-frame with no discard counted (`test_canonical_frame.cpp`) — it is not purely pre-codec.
**Supersedes:** none.

## 2026-09-03 — l3_helper frame verbs: three error shapes outside the frame-vectors contract vocabulary

**Context:** review on PR #116 (T023, @ d30ef1c) flagged that `contracts/frame-vectors.md`
defines exactly two error shapes (`ERR <Status>` for FENC, `ERR <Discard>` for FDEC) and, for
FSTREAM, zero or more `OK <canonical frame line>` lines followed by one `END <discards>` line —
but the code emits `ERR BadRequest` from three places neither the contract nor
`tools/refimpl/canonical.py`'s `frame_error_to_canonical` has a counterpart for: `fdec_line`
(malformed/truncated input with no discard counted), `fenc_response` (malformed canonical text
ahead of `encode_frame_line`), and `fstream_response` (malformed hex, terminated with `END 0`
per the prior review-fix pass on this PR). The `END 0` termination itself is correct and
uncontested; the gap is that no artefact records `ERR BadRequest` as part of any of these three
verbs' vocabulary, so a future differential test has nothing to check FDEC's truncated-input
path against.
**Recommendation:** amend `contracts/frame-vectors.md` to name `ERR BadRequest` explicitly as
the malformed-input-text response for FENC, FDEC and FSTREAM, distinct from a codec-level `ERR
<Status>`/`ERR <Discard>` refusal of well-formed-but-invalid input, and add a
`frame_error_to_canonical`-equivalent mapping on the Python side. No behaviour change implied —
this documents what the code already does.
**Ruling:** human, 2026-09-03 (open-questions session, one-at-a-time Q&A): contract amended in the same PR: `ERR BadRequest` is the malformed-input-text response for FENC/FDEC/FSTREAM, distinct from codec-level refusals.
**Supersedes:** none.

## 2026-09-03 — Correction: the Python reference does not mask `dst`/`src`, only `seq`

**Context:** review on PR #116 (@ 641ee1e) flagged that the "Frame line out-of-range fields"
entry above (2026-09-03) misstates what `tools/refimpl/omgp_link.py`'s `encode_frame` does with
out-of-range `dst`/`src`: it says the Python side "masks them (`omgp_link.py:97`, `dst`/`src`/
`seq`)", but line 97 (`ctrl = ... | ((f.seq & 0x0F) << 4)`) only masks `seq`. `dst` and `src` are
never masked — they go straight into `bytes([f.dst, f.src, ctrl, len(f.payload)])`, which raises
`ValueError` for anything outside 0-255 — exactly what the same entry's own `dst=0x100` example
already said two sentences later, so the entry contradicted itself on its central claim. Per
CLAUDE.md's append-only rule for this file, the error is corrected here rather than by editing
the original entry's text.
**Why it matters:** left uncorrected, a human ruling T025 from that entry could read "Python
masks `dst`/`src`" and choose masking as the normative behaviour for both sides. `dst=0x100`
would then silently become `dst=0x00` instead of being rejected — a frame addressed to the wrong
node accepted instead of the caller's malformed request being refused.
**Recommendation:** when ruling on the superseded entry's question, treat only `seq` as
masked by the Python reference; `dst`/`src` out-of-range is a raised `ValueError` with no
canonical rendering on that side, same as the entry's `dst=0x100` example.
**Ruling:** human, 2026-09-03 (open-questions session, one-at-a-time Q&A): folded into the strict-input ruling above — only `seq` was masked; dst/src raised bare ValueError; all become uniform `ERR BadRequest` rejections at T025. (Superseded pending text, verbatim: "travels with the superseded entry's own ruling at T025.")
**Supersedes:** 2026-09-03 — Frame line out-of-range fields: C++ rejects, Python reference
masks/accepts (corrects its "masks them ... `dst`/`src`/`seq`" sentence only; every other claim
in that entry stands).

## 2026-09-03 — FSTREAM's multi-line response will desynchronise a naive T025 driver

**Context:** red-team pass on PR #116 (T023, @ 65922b5) ran `tools/diffcheck.py`'s own `Helper`
class (`Helper.ask()` reads exactly one line per request, `tools/diffcheck.py:49-63`) against
`build/native/l3_helper` as an early preview of what T025 (`diffcheck.py --frames`, issue #43)
will do. FSTREAM is the first verb in this task to emit more than one line (zero or more `OK
<frame>` lines then one `END <discards>` line, per `contracts/frame-vectors.md`); batching an
FSTREAM request alongside later requests through `Helper.ask()` shifts every later answer by one
line with no exception raised, so the differential silently compares the wrong pairs instead of
failing. Concretely: `["FSTREAM <hex>", "CRC 01020304", "CRC 05060708"]` returns the FSTREAM
line, then `END 0` where `CRC 01020304`'s answer belongs, and the `CRC 05060708` answer is left
unread in the pipe for whatever request comes next. Nothing in this PR or in `diffcheck.py`
today is affected — `diffcheck.py` does not yet call FENC/FDEC/FSTREAM at all (that's T025's own
job), so this is a trap laid for that future driver, not a live break, and out of this task's
declared scope (`tools/canonical.{hpp,cpp}`, `tools/l3_helper.cpp`).
**Recommendation:** T025's `Helper` (or its own request driver) must read frame-verb responses
by verb, not by line count: read one line for FENC/FDEC, and read lines until `END` for FSTREAM,
never assume a 1:1 request:line ratio once frame verbs are mixed into a batch.
**Ruling:** human, 2026-09-03 (open-questions session, one-at-a-time Q&A): the differential driver reads BY VERB (one line for FENC/FDEC; until `END` for FSTREAM); baked into issue #43's criteria.
**Supersedes:** none.

---

## 2026-09-03 — WIP cap widened to 2 stories, as an agent-config knob

**Context:** the cap of 1 froze the pipeline whenever a single agent PR stalled:
observed live — PR #118 (T037, auto-fix exhausted, needs-human) held the cap while
#43 (T025, cleanly `ready`) starved; `pick` logged "WIP cap: agent PR awaiting
review — not pulling". tasks.md anticipated the widening ("US4 … can run in
parallel with US2/US3 by a second agent — but the WIP cap is one"), and the
GOVERNANCE §6 cadence names the cap as the intended widening lever.
**Options:** (1) keep 1 and clear stalls by hand each time; (2) cap 2, counting
STORIES in flight — open agent-authored PRs ∪ claimed task issues, joined on the
task/<n> head-branch convention so a story with both counts once, with
needs-human PRs still counting (they are review load; pressure to resolve them
stays real without freezing everything) and non-task/<n> agent branches counting
fail-closed; (3) also exclude needs-human PRs from the count — maximises
throughput but lets stalled PRs accumulate invisibly.
**Known friction accepted with (2):** two concurrent PRs may both bump
UNIT_TEST_FLOOR in pipeline.sh or append to this file; with strict up-to-date
checks the second lands BEHIND and needs an update-branch, occasionally a trivial
conflict — rebase noise, not a correctness risk.
**Recommendation:** (2), as `wip_cap: 2` in .github/agent-config.yml (T3 constant;
`1` restores the original behaviour and is the kill switch; an unreadable value
fails closed to 1).
**Ruling:** human, 2026-09-03 ("do it but also ask open questions", this session):
option (2) adopted. Pick logic harness-covered for the first time (12 cases:
cap counting, the both-halves dedupe, needs-human skipping, cap=1 equivalence,
fail-closed parsing). GOVERNANCE §2's cap row updated; OPERATING-POLICY §2's
"one item in flight" wording is a human-ruling artefact left for the
maintainer's own hand (same precedent as the §4 loop table).
**Supersedes:** none.

---

## 2026-09-03 — Auto-fix attempt bound raised to 4, as an agent-config knob

**Context:** the 2-attempt bound (ruled 2026-08-30 with the router) has exhausted on
real work twice (#108's mutation survivors needed a maintainer @claude mention as a
third attempt, which then succeeded; #118 escalated needs-human with the failure
plausibly fixable). Two attempts often covers only "diagnose + one fix"; genuinely
iterative failures (fuzz findings, mutation triage) can need more.
**Options:** (1) keep 2 and route third attempts through @claude mentions by hand;
(2) 4, as `auto_fix_max_attempts` in .github/agent-config.yml — same knob pattern as
`wip_cap`/`auto_approve_max_tier`, labels auto-fix-1..4, unreadable values failing
closed to the previously ruled 2; (3) unlimited with a time budget — unbounded spend,
rejected out of hand.
**Recommendation:** (2).
**Ruling:** human, 2026-09-03 ("meant the auto-fix attempt bound should be 4 rather
than 2", this session): option (2). Router counts `auto-fix-<n>` labels against the
knob; comments say "attempt n of <max>"; exhaustion says "after <max> attempts";
labels 3/4 provisioned in gh-setup.sh and created in-repo. Harness re-pinned: two
priors now yields attempt 3, four priors exhausts, `auto_fix_max_attempts: 2`
reproduces the original bound exactly, unreadable knob fails closed with a notice.
**Supersedes:** the bound sentence of "CI-failure auto-resolution" (2026-08-30);
the rest of that ruling stands.

---

## 2026-09-03 — auto_fix_max_attempts knob semantics beyond the value 4

**Context:** review round 3 on PR #120: GOVERNANCE §4 states knob semantics that
the 2026-09-03 bound entry above does not record — the ruling recorded option (2)
(value 4, unreadable fails closed to 2) and nothing else. The additional
semantics were added on PR #120 in response to red-team findings F2/F5/F7 and
review findings there, and this append-only file must not trail the governing
document.
**Recommendation (as implemented on PR #120, for ratification with that PR's
CODEOWNERS review):** a value < 1 DISABLES auto-fix (route posts one marker
comment per sha+pass so the boundless sweep stays quiet); non-digit values are
unreadable and fail closed to 2; values above 10 are clamped to 10 (the ruled-out
"unlimited" enforced against a fat-fingered knob); attempt labels are written to
the first FREE index so the bound stays reachable after a human removes a label
(label index is a free slot, not the attempt ordinal); labels auto-fix-1..10 are
provisioned to cover the clamp range. Red-team round 2 on #120 added: the knob
value tolerates an inline comment or quotes (an operator's `0  # OFF` must
disable, not silently mean 2 — an off-switch failing open), and `timed_out` /
`startup_failure` conclusions route as failures end-to-end (guard, script,
sweep) — previously the exhaustion report listed them as failures while the
router could never act on them.
**Ruling:** pending — ratified by the human merge of PR #120, which lands this
entry and the semantics in the same commit history.
**Supersedes:** nothing; extends the 2026-09-03 "auto-fix attempt bound" entry
above (its option (2) ruling stands).

---

## 2026-09-03 — Persistent ERR_BUSY: no bound anywhere detects a wedged bridge

**Context:** review on PR #122 (MEDIUM). The §8 rewording makes `ERR_BUSY` the
mandated per-poll answer while a module transaction is in flight, and the first
draft of that PR's scoping sentence said a busy answer "is not a failed
transaction" for §7. §7's only health triggers are 3 consecutive failed
transactions → SUSPECT and 1 s in SUSPECT without a valid response → OFFLINE —
so under that sentence a backplane wedged answering `ERR_BUSY` forever stays
ENROLLED indefinitely, undetected by any bound in the document (proved by
reading §7 as it stands; no accounting code exists yet — `link/` holds only
`HealthState`, so there is no code divergence). T031/T039 implement against
this text, so the question must be ruled before they land. To be precise
about what §8 as amended does and does not settle (review round 3 on #122):
it DOES decide the per-poll case — a single `ERR_BUSY` is a valid response,
not a failed transaction, and via §7's "valid response" clause it would also
rescue a SUSPECT node — and it marks only the PERSISTENCE bound as open.
**Recommendation:** bound persistent busy explicitly in §7: N consecutive
`ERR_BUSY` answers to the same outstanding request (spanning its retries)
count as one failed transaction, so ordinary busy stays harmless while a
wedged bridge still walks to SUSPECT. LAYERING CONSEQUENCE the ruling must
weigh (review round 3 on #122): §7's health accounting is L2, implemented in
`link/` (T031 master, T039 HealthTracker), and the architecture invariant
says `link/` never interprets payloads — but distinguishing an `ERR_BUSY`
answer from any other response IS payload inspection, so this option needs
either a narrow, explicitly-sanctioned carve-out (e.g. L4 feeds a
busy/not-busy hint back to the tracker) or it violates L2/L3 opacity.
Alternative with no layering conflict: an explicit sentence declaring
persistent busy out of node-health scope and owned by L4 — which is already
where §7 reports OFFLINE.
**Ruling:** ADOPTED the recommendation's bound — human, 2026-09-06, via #110 F1(b): see the
2026-09-06 entry "#110 F1: a backplane that never answers, and a node that answers ERR_BUSY
forever, are both bounded" — a "failed transaction" is a timeout, a CRC failure, or eight
consecutive `ERR_BUSY` answers to the same request, after which the host backs off
exponentially for that node and reports to L4 (documents: #155; host: #157). The LAYERING
CONSEQUENCE above is NOT ruled by F1(b): where the busy-count lives (an L4 hint fed back to
the tracker, or a sanctioned carve-out in `link/`) is a design ruling owed before #157/T039
is written (review of PR #163, FOLLOW-UP). Filled 2026-09-06 (was: pending — human; blocks
nothing until T031/T039).
**Supersedes:** none (the §7-accounting sentence this entry discusses was
replaced within the still-unmerged PR #122, not by a landed entry).

---

## 2026-09-03 — specs/**/contracts/ are T3 artefacts no path gate treats as T3

**Context:** review round 3 on PR #122 (MEDIUM). The pending text superseded on
that PR said, verbatim, "`contracts/frame-vectors.md` is a T3 artefact; a human
amends it (or rules otherwise), with T025." — but nothing enforces that: risk-score's T3 regex covers protocol/,
tests/vectors/, .github/, CLAUDE.md, the named docs/*, .specify/memory/ and
tools/mutate.cfg, while CODEOWNERS covers only `specs/**/tasks.md` under specs/.
PR #122 scored T3 solely because it also touches docs/trunk-link-layer.md; an
identical amendment to frame-vectors.md alone would score T0 and be eligible for
autonomous merge under GOVERNANCE §1. The same gap was recorded for T030's
`tests/support/**` slice (issue #48, now needs-human).
**Recommendation:** extend the mechanical gates rather than relying on entry
text: add `specs/**/contracts/`, `specs/**/data-model.md` and
`specs/**/research.md` to CODEOWNERS and to risk-score's T3 regex (a T3 workflow
change, its own CODEOWNERS-gated PR). Until then, contract amendments ride only
in PRs that also touch an already-gated artefact, as this one does. Companion
verification gap (red-team round 4 on #122): nothing EXECUTES the artefact's
claims either — the full pipeline stays green with a factually wrong helper
description in the tree. The same gate-extension ruling should add a refimpl
test that drives `build/native/l3_helper` with the verb table's own probes
(truncated-hex FDEC, bad-hex FSTREAM, the 65 B and 256 B payload boundaries)
and asserts the documented spellings — it would have caught two of this PR's
own review findings mechanically.
**Ruling:** pending — human; the gate extension is a one-line T3 PR when ruled.
**Supersedes:** none.

---

## 2026-09-03 — Bridged replies after ERR_BUSY: retention and correlation unspecified

**Context:** red-team round 4 on PR #122 (HIGH). §8 as amended mandates that the
module-bus transaction proceed after the poll is answered `ERR_BUSY` ("never
abandons or restarts it"), but states no rule for where the module's reply goes
when it arrives after its poll was already answered. T_resp (200 µs) is ~25×
shorter than the module-bus timeout (5 ms), so `ERR_BUSY` is the COMMON first
answer for every bridged request, not a corner. Consequence on the plain
reading: the §7 single-frame replay buffer for that seq holds `ERR_BUSY`, so
`GET_EVENT` — the protocol's only non-idempotent opcode, justified in the YAML
as "replay-safe via L2 seq replay buffer" — can drain an event whose reply has
nowhere defined to go; a retry drains another. Two §8-conformant
implementations both lose events (drop the late reply; or serve it uncorrelated
to the next poll). Golden rule 2 ("retries at L2 must always be safe") is not
provably preserved by the text as it stands. A backplane that answers event
reads from its own prefetch cache (which the `event_pending` status summary
already gestures at) never hits this path — but §8's plain reading describes
and permits the unsafe model.
**Recommendation:** rule one of: (a) retain-and-correlate — the backplane keeps
the completed reply and serves it to the next poll carrying the same request,
with the correlation rule (same src+seq? same opcode?) spelled out; (b) the
backplane MUST NOT begin a module-bus transaction whose reply it cannot
deliver (restores the replay-buffer justification directly); (c) event reads
are answered from a backplane-side event cache filled autonomously, making
GET_EVENT bridge-local and never subject to the late-reply hole — likely the
intended design, and the recommendation. §8 now marks the question open
(§10.6) and mandates none of the three.
**Ruling:** pending — human; must be ruled before any bridge implementation
(feature f4), gates nothing in feature 002.
**Supersedes:** none.

---

## 2026-09-03 — next_probe's "no candidate" sentinel is not in the contract

**Context:** review on PR #118 (MEDIUM). `HealthTracker::next_probe` returns
`Probe{ADDR_host, ...}` when no UNENROLLED/OFFLINE address exists — the steady
state of a healthy rig. The behaviour is tested and `ADDR_host` is never a real
probe target, but `contracts/link-cpp.md` "Health tracker" declares no "none"
value, so a T039 scheduler written from the contract alone would spend trunk §6's
one enrolment-probe slot per superframe addressing the host itself.
**Recommendation:** amend `contracts/link-cpp.md` to state the sentinel, or —
better for callers — replace it with an explicit `bool valid` on `Probe` in the
same amendment. Until ruled, the sentinel is documented at the declaration
(`link/health.hpp`) and pinned by tests; T039 (issue #57) must not be
implemented against the contract's silence.
**Ruling:** pending — human, with T039.
**Supersedes:** none.

---

## 2026-09-03 — A comment-only diff in scope_dirs is indistinguishable from broken instrumentation

**Context:** review + red-team on PR #124 (HIGH, convergent). Moving a `mutant-ok`
label onto its own comment-only line — the placement `tools/mutate_report.py`'s own
docstring prescribes for clang-format stability — made that comment the diff's only
in-scope line. Mull attributes no mutant to a comment, so the report saw zero
in-scope mutants and took the blind-spot branch ("instrumentation is not reaching
the code"), failing deep-verify with a bogus FAIL. Demonstrated at 4265f78 (the CI
run) and by the red-team's saturated-report reproducer: 170 survivors elsewhere in
the file, exit 1 regardless. Any future PR whose only embedded-path change is a
comment hits the same wall; `mutation-exempt(no-body)` is not an answer (it is a
file-level claim about files with no function bodies).
**Recommendation:** teach the blind-spot check the difference: when every in-scope
changed line is a comment or blank (strip → empty or starting `//`), report
"comment-only change: no mutable code in the diff" and pass instead of failing.
DIRECTION CHANGE, stated plainly (review round 4 on #124): that branch alone
flips today's fail-closed behaviour to fail-open for exactly the diff shape that
adds triage labels — a PR whose only scope_dirs change is a new `mutant-ok`
comment line would produce no survivor report at all, never consult
`Label.covers()`, and skip the stale-label sweep. So the second clause is PART OF
the recommendation, not optional: a changed `mutant-ok` label line must pull the
line it governs into scope, so the labelled survivor is re-examined and the
label's mutator list is enforced. Gate-scoping semantics either way, so a human
rules it (both reviewers declined to pick a remedy for the same reason). PR #124
itself sidesteps it in-diff: a short trailing `// labelled above` marker keeps
the governed line in scope without disturbing the format-stable label placement.
**Ruling:** pending — human; the workaround unblocks #124, the tool fix is the
durable answer.
**Supersedes:** none.

---

## 2026-09-04 — May a HealthListener re-enter HealthTracker from on_notice?

**Context:** red-team round 6 on #124 showed the notify-after-state-assign ordering in tick()
is observable only under a re-entrant listener (one that calls on_result/tick
from inside on_notice), and that no document defines whether re-entrancy is
allowed — specs/002-trunk-link-layer/contracts/link-cpp.md is silent. F3's
scheduler is the real listener and its needs are not yet designed.
**Recommendation:** forbid re-entry for now (a doc sentence on
HealthListener::on_notice, added in #124 alongside this entry), and let F3's
design either keep the prohibition or supersede this entry with a defined
re-entrancy contract plus tests. Forbidding is the safe default: no current
listener re-enters, and it leaves tick()'s internal ordering an implementation
detail rather than a promise.
**Ruling:** pending — F3 design (safe default applied: prohibition documented).
**Supersedes:** none.

---

## 2026-09-03 — Frame line out-of-range fields: recommendation landed with T025

**Context:** the 2026-09-03 "Frame line out-of-range fields" entry above carries the HUMAN
ruling (filled 2026-09-03, session Q&A): C++ strict rejection is normative and T025 aligns
the Python reference — no `seq` masking, no bare `ValueError` for `dst`/`src`. (An earlier
draft of this entry misquoted that ruling line as still "pending"; corrected per review
round 6 on #121 — the correction is an ordinary edit, this entry being unmerged.) A review
on PR #121 (T025, @ ed9f0ae) reported the alignment as not yet implemented, a HIGH finding: `tools/refimpl/canonical.py`'s `canonical_to_frame` still accepted `seq=16` (masked to
`0` downstream by `omgp_link.encode_frame`), silently dropped high bits of `flags` above `0x03`,
and raised an unmapped `ValueError` for `dst`/`src` outside `0-0xFF` — none of which match
`tools/canonical.cpp`'s `parse_frame_line`, so a line invalid on one side of the differential
could still be `OK` (or crash) on the other, undetected by the frame corpus itself (it is
valid-only by construction, per `contracts/frame-vectors.md`).
**Fix:** `canonical_to_frame` now validates `dst`/`src` (0-0xFF), `flags` (0-0x03), `seq`
(0-0x0F) and `len(payload)` (<=0xFF) before constructing a `Frame`, raising `CanonicalError` —
the same exception every other malformed-token path in this function already raises — instead
of masking or falling through to an unmapped `ValueError`. `dst=0xFF` (reserved, in-range) and
payload lengths 65-0xFF (in-range here, refused by `encode_frame`'s own `PayloadTooLong`) are
deliberately left to `encode_frame`'s own checks, matching `parse_frame_line`'s own layering
(the boundary is "does it fit the wire representation at all", not "is it a legal frame").
TDD: `tools/refimpl/test_canonical.py::test_canonical_to_frame_rejects_out_of_range_fields`
(5 cases at the time; review rounds 3-4 later folded in `negative-seq`, `negative-dst` and
`leading-whitespace` for 8) and `test_canonical_to_frame_does_not_mask_seq`, confirmed
failing pre-fix (`DID NOT RAISE CanonicalError` for all 6 then present), then passing; `./pipeline.sh refimpl diffcheck`
green (counts as recorded at commit 94eefff, before the concurrent branch's tests were
unioned in; 42287 cases, frames 10000/torture 18000 still agree — the real corpus is
valid-only, so this is a regression guard, not a change to today's differential pass/fail).
**Not done:** the sibling entry above (`l3_helper` frame verbs: `ERR BadRequest` outside the
contract vocabulary) recommends a `frame_error_to_canonical`-equivalent mapping so a live
Python-side driver could render `CanonicalError` as `ERR BadRequest` the way `tools/l3_helper`
does. No such driver exists in this repo (the real differential always talks to the compiled
`l3_helper` binary; `canonical_to_frame` is invoked directly by tests, `genvectors.py`, and the
`FakeHelper` fixtures in `test_diffcheck_frames.py`, which now propagate `CanonicalError` the
same as any other malformed-line failure), so there is nothing to render it into — left for
that entry, unstarted here.
**Ruling:** none made here — this entry records the T025 IMPLEMENTATION of the human ruling
already filled in the entry above (GOVERNANCE §1 keeps spec-ambiguity resolution with the
human; no agent-authored ruling exists to confirm, and no ruling line anywhere is stale).
**Supersedes:** none; the prior entry's ruling line is live and this implements it.
(Relocated to the file tail per review round 11 on #121 — entries stay in
append order; an ordinary edit, this entry being unmerged.)

---

## 2026-09-04 — Reserved ctrl bits 2-3: is ignore-on-receive a spec rule or just current behaviour?

**Context:** review round 16 on #121 (LOW). trunk §4 (docs/trunk-link-layer.md:38)
defines ctrl bits 2-3 as "reserved 0" and its discard-conditions bullet lists bad CRC,
bad length and >=8 stuffing violations — it does NOT state that a receiver must IGNORE
those bits when set. link/frame.cpp and omgp_link.py both ignore them (mask seq from the
high nibble, never inspect bits 2-3), and #121's run_streams now pins that behaviour with
a reserved-ctrl-bit delivery element. The tooling contract briefly attributed the rule to
"§4 forward compatibility", which overstated the spec's backing.
**Recommendation:** treat ignore-on-receive as the intended forward-compatibility rule
(it mirrors L3's "unknown TLV types skipped, unknown events ignored", CLAUDE.md golden
rule 7) and add one sentence to trunk §4 stating it, so the behaviour the corpus pins has
a normative source. Until then the tooling contract cites the implementation/test, not §4.
**Ruling:** pending — human (spec amendment to trunk §4, a human-ruling artefact).
**Supersedes:** none.

---

## 2026-09-04 — No hostile-TEXT differential corpus for ENC/DENC; two pre-existing message-codec divergences

**Context:** red-team round 17 on #121 (LOW). The differential feeds malformed BYTES to
DEC/DDEC/DVAL but never malformed canonical TEXT to ENC/DENC, though T025's rounds 12-17
hardened exactly those shared parsers (_int, _parse_named, _hexbytes, _tokens,
_split_records). Its absence is what let round 16's descriptor CR regression (fixed at
round 17) ship green. A PR-vs-merge-base sweep surfaced two divergences, both verified
PRE-EXISTING against 02507ee, neither introduced by #121: (a) an out-of-range scalar like
`channel=17500` is silently masked to uint8 on the C++ side (`1052cf00015c`) while the
Python reference raises `ERR OutOfRange` — the "text names one value, wire carries another"
hazard the parse_uint guards fight, one field-width layer up; (b) `DENC ... s="\x"` raised
a bare `ValueError` out of unquote_str (now mapped to CanonicalError here in round 17, so a
future corpus will not crash diffcheck with a traceback).
**Recommendation:** add a hostile-text ENC/DENC differential corpus (mirroring run_invalid's
byte corpus) AND fix the C++ scalar-masking so out-of-range message fields answer
`ERR OutOfRange`/`ERR BadRequest` on both sides. This is message-codec parity work beyond
T025's frame-differential scope and touches tools/canonical.cpp's message path; it belongs
in its own task rather than being bolted onto #121 at round 17. The unquote_str crash is
closed here as an in-scope robustness fix; the corpus and the masking divergence are
deferred and flagged for the CODEOWNER handling this PR's needs-human escalation.
**Ruling:** pending — human (follow-up task scope).
**Supersedes:** none.

---

## 2026-09-05 — Route review findings by scope, not severity (#134)

**Context:** the review-fix loop routed by severity (fix HIGH/MEDIUM, defer LOW),
which caused scope creep — a review found an adjacent weakness, the fix loop bolted
on code to satisfy it, and that code generated the next round's findings. PR #128
(T027) spent four review-fix rounds plus a red-team round on registration/selftest
gate machinery that was never in #45's acceptance criteria and was itself foolable;
scoping the PR back to its criteria and filing the durable gate as a new issue (#133)
reached the right place in one move. Severity and scope are orthogonal: a MEDIUM
"you could also harden X" outside the criteria should not block a correct PR, while a
LOW weakened assertion inside the diff should.

**Recommendation / ruling:** ADOPTED (human, via #134). Route findings by scope.
- BLOCKING (fixed in the PR, counts toward the verdict): a defect in the changed
  code, a security hole, a weakened/narrowed test, a spec divergence, an unmet
  acceptance criterion of the linked issue, or a false claim the PR makes (dropping
  the claim is a valid fix). Never deferrable, at any severity.
- FOLLOW-UP (proposed as an issue, does NOT count toward the verdict): a real
  improvement, hardening or pre-existing gap OUTSIDE the linked issue's acceptance
  criteria. MEDIUM-or-above only; pure style is a noted LOW.
- `claude-review`'s verdict is `clean` iff there are no BLOCKING findings; `review-fix`
  fixes only BLOCKING findings and MAY SHRINK the diff (remove out-of-scope machinery),
  not only add code. Human override stays: the maintainer can pull a deferred finding
  back into the PR or reject a proposed issue.
- **Dependency:** this makes the acceptance criteria the load-bearing contract, so it
  pairs with tighter enrichment (precise, testable criteria); weak criteria make a
  mis-classification able to wave a real gap through into auto-approve.
- **Both reviewers aligned (corrected after the #136 red-team round).** An earlier draft
  deferred aligning `red-team`'s verdict — the #136 red team showed that deferral created
  a liveness stall (out-of-scope red-team `findings` → review-fix does nothing → head
  never moves → the `needs-human` terminal state is unreachable). So `red-team` now
  classifies block-vs-follow-up and emits `clean` iff no BLOCKING finding, same as
  `claude-review`; a `findings` verdict always means real work. The guardrails
  (never-defer defects/security/weakened-tests, verdict-on-blocking) are pinned by tests
  (`test_scope_routing_guardrails_are_pinned`). Follow-ups are consumed by the new
  `review-followups` workflow, which files each as a dedup'd `task` issue.
**Supersedes:** the 2026-09-02 review-fix severity policy (that entry and GOVERNANCE §
"Review-finding auto-resolution").

---

## 2026-09-05 — Kind::CrcError's corrupted CRC byte is not a literal "XOR 0xFF"

**Context:** review round 4 on #137 (MEDIUM). `contracts/mock-wire.md`'s Step table says
Kind::CrcError produces "the real response with its last CRC byte XOR 0xFF". A bare XOR
0xFF crosses the FLAG/ESCAPE byte-stuffing boundary for exactly four real high-byte values
(0x7E/0x7D <-> 0x81/0x82), which would silently change the corrupted frame's wire length
relative to the real response's — breaking every timing assertion in
tests/unit/test_link_master.cpp that computes an expected instant from the UNCORRUPTED
response's own encode_frame length. `tests/support/mock_wire.cpp`'s `corrupt_crc_hi()`
deliberately picks a different (still-wrong) byte on the same side of that boundary for
those four values instead, and is tested for length-preservation
(`tests/unit/test_link_master.cpp`, "a CrcError response's wire length matches..."). T028
(#46) is specified to generate the tooling/reference implementation's Kind::CrcError
behaviour from this same contract table, so an implementation written from the table's
literal text would not match `mock_wire.cpp`'s behaviour at those four values.
**Recommendation:** amend `contracts/mock-wire.md`'s CrcError row to state the
length-preserving exception (or reference `corrupt_crc_hi()`'s rule directly) before T028
is implemented, so both implementations corrupt the CRC the same way at every value.
**Ruling:** pending — human (contract-doc amendment; not one of CLAUDE.md's three
authoritative `docs/` documents, but still a human-ruling artefact per GOVERNANCE §3).
**Supersedes:** none.

---

## 2026-09-05 — Master::begin() refuses dst >= kAddrCount, beyond encode_frame's contract

**Context:** review round on #137 (LOW). `contracts/link-cpp.md` specifies `begin()`'s
address refusals "as `encode_frame`", which refuses only `dst == 0xFF` (trunk §5 reserved
broadcast). `link/master.cpp`'s `begin()` adds a stricter refusal — `dst >= kAddrCount` →
`Status::ReservedAddress` — because `next_seq_`/`stats_` are `kAddrCount`-entry tables
indexed directly by `dst`; without it `begin(0x20, …)` wrote past both tables (the HIGH
that refusal fixed). trunk §5 makes `0x00–0x0F` the only trunk node addresses, so the
stricter refusal is correct and necessary, but the contract text now diverges from the
implementation.
**Recommendation:** amend `contracts/link-cpp.md`'s `begin()` refusal clause to state the
`dst >= kAddrCount → ReservedAddress` guard explicitly (the same way the Kind::CrcError
entry above records `corrupt_crc_hi()`'s deviation), so an implementation written from the
contract matches `master.cpp`.
**Ruling:** pending — human (contract-doc amendment; not one of CLAUDE.md's three
authoritative `docs/` documents, but still a human-ruling artefact per GOVERNANCE §3).
**Supersedes:** none.

---

## 2026-09-05 — Master::begin(dst == ADDR_host) is accepted; the host transacts with itself

**Context:** review round on #137 (red-team, LOW). `Master::begin()` refuses `dst == 0xFF`
and `dst >= kAddrCount`, but `ADDR_host` is itself a valid trunk address (`0x00..0x0F`), so
`begin(ADDR_host, …)` is accepted: the master transmits a request to itself and can book a
successful transaction against its own `stats_[ADDR_host]`. It does not crash or over-index
(ADDR_host is in range) — but a single-master trunk has no reason to address itself, and
`contracts/link-cpp.md` neither blesses nor forbids it. Adding a refusal is a behaviour
change to `begin()`'s contract, so it is recorded here rather than implemented speculatively
(CLAUDE.md working agreement: no speculative behaviour without a ruling or a safe default).
**Recommendation:** refuse `dst == host_addr_` in `begin()` (e.g. `Status::ReservedAddress`,
or a dedicated status) and state it in `contracts/link-cpp.md`, with a test. Low priority —
no corruption today, only a nonsensical-but-accepted input.
**Ruling:** pending — human (contract-doc amendment + a small behaviour change to `begin()`).
**Supersedes:** none.

---

## 2026-09-05 — an in-window CRC failure ends the attempt and is charged to the polled node

**Context:** review round on #137 (red-team, LOW). `Master::poll()` ends the open attempt on
ANY in-window CRC failure (`end_attempt(CrcFailed)`) without waiting out the rest of `T_resp`,
and charges `crc_failures` to `dst_`. A bad-CRC frame is by definition unattributable — its
source field did not survive the CRC check — so a hostile station emitting a short bad-CRC
frame early in node N's response window costs node N an attempt and a `crc_failure` even though
node N's own conforming answer arrives later in that same window and would have been accepted.
Repeated, trunk §7's failure accounting marks an innocent node SUSPECT on traffic it never sent
— squarely the hostile-module threat model CLAUDE.md names for an open platform.
**Recommendation:** the fast-fail is a literal reading of trunk §7 ("a CRC-failed response is a
failure"), so changing it is a spec question, not a code cleanup. Two candidate rulings: (a)
keep the fast-fail but do NOT charge `crc_failures`/health to `dst_` for a frame whose source
cannot be authenticated; or (b) do not surrender the attempt at all — count the CRC failure on
the bus, keep waiting out `T_resp`, and let a genuine answer still win. (b) costs latency only
in the already-failing case and removes the amplification entirely; (a) is the smaller change.
Recommend (b), with the bus-level count retained for diagnostics.
**Ruling:** pending — human (trunk §7 semantics; affects health/SUSPECT accounting).
**Supersedes:** none.

---

## 2026-09-05 — what should the Master do when the bus is NEVER idle for T_gap? (babble)

**Context:** review round on #137 (red-team, HIGH). `fire_pending()` re-evaluates the
gap-deferred transmit instant against the latest bus activity, so the engine never transmits
on top of an arriving frame. The unhandled case is the opposite one: if a station keeps bytes
on the wire continuously, `last_activity_ + T_gap` advances on every poll and the deferred
instant is pushed out **indefinitely** — no transmission, no retry, no `Failed`, `busy()` true
forever. trunk §3's "≥ T_gap of idle before transmitting" is physically unsatisfiable while
that continues, so *some* deferral is correct; deferring silently and unboundedly is not.
trunk §7 names babble as a failure mode, but this engine has no path to report it: the only
outcomes it can produce today are `Answered` and `Failed{Timeout|CrcFailed}`.
**Recommendation / what #137 now does:** option (a) below is IMPLEMENTED in #137; option (b)
remains open and is tracked in #138. (a) The engine no longer sits in `PendingTransmit`
indefinitely: the deferral is bounded by one worst-case frame (trunk §4) beyond the instant it
was originally deferred to, and — critically — the guard fires only when the bus has actually
DENIED a T_gap window in that time (the transmit instant is still in the future), never on
elapsed time alone. An elapsed-time-only guard cannot tell a busy bus from an infrequent
caller, and since `TRUNK_T_poll_us` (2000) exceeds that budget (1420 at `TRUNK_bit_rate`) it
abandoned every gap-deferred transaction and retry at the documented superframe cadence on a
completely idle wire. The transaction concludes `Failed{Timeout}` and NO per-node counter is
charged, since the request never reached the node and booking it a failure would feed trunk
§7's SUSPECT rule against an innocent node. (b) Still open: a caller cannot distinguish "the
node did not answer" from "the trunk was unusable"; that needs a distinct bus-fault
outcome/health transition, which changes `MasterEvent`'s contract and so wants a ruling.
**Note on process:** an earlier revision of this entry said the whole question was deliberately
left unimplemented in #137. That is superseded by the above — (a) shipped, (b) did not.
**Ruling:** pending — human (trunk §7 babble semantics; may need a new MasterEvent outcome).
**Supersedes:** none.

---

## 2026-09-05 — Master under a never-idle bus: bounded courtesy, then transmit (supersedes the babble entry)

**Context:** seventh review round on #137 (red-team at `afed239`, HIGH). The prior entry's
option (a) — conclude `Failed{Timeout}` once the bus has "denied a T_gap window" for one
worst-case frame — was the third bound of that shape to be falsified: sampled at `poll()`
instants, ONE stray byte per superframe made the bus look permanently busy, so a cheap
adversary (or a noisy line) blocked every transaction with a false "babble" outcome that
the caller reads as a node failure. The common root cause of all three: the engine observes
and transmits only at `poll()` instants, so any inference "the bus is unusable" drawn from
those samples is spoofable or a false positive, and any `Failed` it synthesises is
node-shaped. Two corrections to the record while here: (1) trunk §7 does NOT name babble —
the prior entry's "trunk §7 names babble as a failure mode" is wrong; the only authoritative
babble text is `specs/002-trunk-link-layer/spec.md` Edge Cases "Babble" ("the host discards
everything that is not the polled node's frame; the transaction in progress fails or succeeds
on its own merits; the babbling node's health is not adjusted on the host side"). (2)
contiguity of bytes within a frame (which `fire_pending()`'s protection argument relies on)
comes from spec.md's "Assumptions" transmission-time model, not trunk §4.
**Reading adopted (implemented in #137):** trunk §3 makes the host the only initiator ("no
multi-master arbitration, no CSMA, no token") and owes ≥ `T_gap` of idle after ITS OWN
transactions (FR-010's head clause: "the end of one transaction … and the start of the
next"). Deferring a transmission for bytes that are not the host's own is a courtesy on top
of that, bounded at `defer_origin + max_frame + T_gap` — long enough for any single frame
already on the wire at the deferred instant to finish AND receive its full gap (established
by construction in `fire_pending()`; demonstrated by `tests/unit/test_link_master.cpp`'s
"worst-case-length frame starting exactly at the deferred instant" case). Past the cap the
engine transmits on schedule and the transaction fails or succeeds on its own merits.
`MasterEvent::Failed` reasons stay exactly `Timeout | CrcFailed`; nothing is ever concluded
from the bus state. A trunk that stays jammed is found the way trunk §7 designed: every node's
transactions time out → BUS_FAULT via the per-node accounting (data-model §7).
**Tension acknowledged:** FR-010's parenthetical ("last byte transmitted or received") read
literally is unsatisfiable under continuous foreign traffic, so every option departs from a
literal reading somewhere; this one departs only for a station that is itself violating §3.
**Trade-off (assumed, re F3):** under a stuck driver each transaction now concludes after its
full three attempts (≈17 ms at the superframe cadence) instead of the falsified bound's ≈1.7 ms
false-Fail, so BUS_FAULT is reached ~10× later — in exchange no bus condition is ever misread
as a node outcome.
**Contract text:** `contracts/link-cpp.md` ("Master engine") and `data-model.md` §4 "Gap" are
amended in #137 to state the bound, marked *pending a ruling*; their previous wording
("deferred to that instant", "no earlier than `last_activity + T_gap`") described the
unbounded behaviour. Ruling wanted on the reading AND the amendment together.
**Still open (#138):** option (b) of the prior entry — a distinct bus-level outcome/counter so a
caller can tell "node silent" from "trunk unusable" (also wanted by the CRC-attribution entry
above for its bus-level count); and `begin()` transmitting without first draining the wire,
which means the cap's protection guarantee for a fresh `begin()` assumes `poll(now)` ran
immediately before it (true of every test and the intended F3 loop; not enforced). trunk §10
(open questions) has six numbered items; this deserves a seventh — a human edit to an
authoritative doc.
**Ruling:** pending — human (trunk §3/§7 reading; contract amendment; T3 artefacts).
**Supersedes:** the 2026-09-05 entry "what should the Master do when the bus is NEVER idle for
T_gap? (babble)" — its option (a) is withdrawn as falsified; its option (b) remains open in #138.

## 2026-09-06 — bounded courtesy: the worst-case conclusion time is larger than the entry above states (FLAG-delimited babble)

**Context:** red-team at `40355cf` (#137, MEDIUM). The entry above and the suite's "within a
symbol-derived bound" case gave the time for a full three-attempt transaction under continuous
babble as `3·(F + 2G + B) + 3·(n·B + R) + 6·P` (F = `kMaxWire` byte times, G = `T_gap`,
B = one byte time, R = `T_resp`, P = poll cadence, n = request bytes). That is the bound for
FLAG-FREE filler only: the filler never opens a frame, so the `T_resp` in-flight hold ("the
timeout gates the START BIT — a frame that opened inside the window is allowed to finish",
trunk §3) is never entered. A station that opens a frame on the last wire slot inside each
attempt's window and streams `kMaxUnstuffed` escaped FLAGs holds that attempt's timeout off
for one worst-case frame — exactly `kMaxWire·B` (= F), by construction from the Deframer's
`Discard::TooLong` limit plus `frame_arriving()`'s one-byte slack; a second frame cannot add
to it because its opening FLAG lands at or after the deadline, outside the window.
**Corrected bound (implemented in #137):** `3·(F + 2G + B) + 3·(n·B + R + F) + 6·P` —
≈ 9.7 ms at a 1 µs cadence / 1 Mb/s (measured 9.66 ms: tight), ≈ 21.7 ms at the superframe
cadence, ≈ 88.8 ms at the fallback rate and superframe cadence (measured 84 ms). Pinned by
two new cases in `tests/unit/test_link_master.cpp` (the FLAG-delimited adversary at both
cadences and both rates; the single-attempt hold pinned EXACTLY at `deadline + kMaxWire·B`).
The "≈17 ms" in the entry above is therefore the FLAG-free figure; the worst case at the
superframe cadence is ≈ 21.7 ms, and the "~10× later BUS_FAULT" trade-off becomes ~13×.
What is unchanged: `Failed{Timeout}` on the node's merits, never `CrcFailed`, never a Failed
synthesised from the bus state; three transmissions; `busy()` clear afterwards.
**Tension with trunk §6:** "host-visible within (backplane module-poll period) + (≤ 2 × T_poll),
independent of load" is not met against such a station — one transaction alone may span up to ~11
superframes (the bound; 6 measured at 1 Mb/s). §6 describes a conforming bus; a station transmitting outside its own response
window is a §3 violator and the spec's remedy is §7's BUS_FAULT, not a faster conclusion. If
the maintainers want a tighter worst case, the lever is the in-flight hold's length (today one
worst-case frame, i.e. the Deframer's `kMaxUnstuffed`), not the courtesy cap. Recommended:
accept the figure; note it against §6 in the trunk §10 item the entry above asks for.
**Ruling:** pending — folded into the "bounded courtesy" ruling above.
**Amends:** the 2026-09-05 "bounded courtesy" entry's ≈17 ms / ~10× figures (that entry's
reading and contract text are unchanged).
**Supersedes:** none — amends, not replaces, the 2026-09-05 "bounded courtesy" entry (its reading stays live; see **Amends:**).

## 2026-09-06 — a bit-rate change while another station's frame is still arriving: what does the wire model mean?

**Context:** red-team at `40355cf` (#137, LOW). `Master::poll()` computes every drained
byte's END as `start + byte_time_us(wire.bit_rate())` at DRAIN time; `ByteWire::receive()`
reports a byte's start instant only, not its duration. After `set_bit_rate()` upwards (trunk
§7's recovery from the fallback rate back to `TRUNK_bit_rate`), bytes another station put on
the wire at the OLD, slower rate are recorded as ending ~76 µs early, the engine perceives
gaps > `T_gap` between them, and a gap-deferred transmit can go out INSIDE that frame
(reproducer: frame spanning [494, 12706) at the fallback rate, rate raised at 494 + 10 slow
bytes, host transmits at 1414). `fire_pending()`'s "never transmits over an arriving frame"
holds by construction only at a CONSTANT rate; its comments and the T-4 case now say so
(rule 11). Nothing in the suite asserts the collision invariant across a rate change.
**Options:** (a) treat it as a modelling artefact — on real hardware a UART re-rated mid-frame
receives framing errors/garbage, not the sender's bytes, so `MockWire` handing the old-rate
bytes over intact after the change is the unrealistic part; the ByteWire contract
(`byte-wire-and-clock.md`) would state that bytes already queued at a rate change are
delivered as garbage / dropped, and no engine change is needed. (b) extend `ByteWire::receive()`
to report each byte's duration (or the rate it was received at) so the engine can compute true
ends — an interface change for F3/F4. (c) have the engine hold off for one worst-case frame at
the OLD rate after any upward rate change — simple, but re-introduces a fixed hold of the kind
this PR removed elsewhere. **Recommended:** (a), as a contract clarification; it matches the
physics and keeps the engine's inference honest without new state. Until ruled, the claim in
`master.cpp` is narrowed rather than the behaviour changed.
**Where it lives:** #138 (Master follow-ups), alongside the `begin()`-drain assumption, which
is the same shape of gap (a protection guarantee that holds only under an unstated operating
assumption).
**Ruling:** pending — human (ByteWire contract; F3/F4 interface).
**Supersedes:** none.

## 2026-09-06 — Master::set_bit_rate(0) is refused; the contract has no refusal clause

**Context:** review at `e3af74d` (#137, LOW). `contracts/link-cpp.md` specifies `set_bit_rate`
as "pass-through to the wire + `BusStats.rate_changes`" — no refusal, no return value. The
engine calls `byte_time_us(wire_.bit_rate())` on every `poll()`, and `byte_time_us` has a
nonzero precondition (`link/link_types.hpp`): a zero rate accepted here would make the engine
violate its own precondition from the inside (an assert in a debug build, a divide-by-zero
otherwise — red-team on #137, LOW). Implemented in #137: `bps == 0` returns without forwarding
and without bumping `rate_changes` (pinned by `tests/unit/test_link_master.cpp` "set_bit_rate(0)
is refused"). Every other divergence from that contract file in this PR was logged here; this one
was not (review, LOW) — recorded now.
**Options:** (a) refuse silently, as implemented — the caller (F3's HealthTracker, whose `Probe`
rates come from trunk §9's two nonzero constants) can never legitimately pass 0, so a silent
no-op is the least-surprise behaviour and keeps the `void` signature; (b) return a `Status`
(`ReservedAddress`-style refusal code, or a new one) so a misuse is observable — an interface
change for F3; (c) clamp to `TRUNK_bit_rate_fallback` — rejected: no rate is a defensible
stand-in for "no rate". **Recommended:** (a), with the contract text amended to say so (done in
#137, marked pending); revisit if F3 ever computes a rate rather than selecting one of §9's.
**Ruling:** pending — human (contract text).
**Supersedes:** none.

## 2026-09-06 — Deframer::in_frame() is a public member the contract's Deframer listing did not have

**Context:** review at `3a15d29` (#137, MEDIUM). `contracts/link-cpp.md` "Frame codec" listed
`Deframer`'s public surface as exactly `Deframer()`, `feed`, `reset`, `stats`. #137 added
`bool in_frame() const` (`state_ != Hunting`) so `Master::frame_arriving()` can pair the
parser's state with byte cadence for the trunk §3 T_resp in-flight test (a response whose
START BIT arrived inside the window must be allowed to finish). `frame.hpp` is US1's closed
deliverable (T027/#45), so this is drift in a settled artefact; every other contract divergence
in #137 was logged here and this one was not — recorded now, and the listing is amended in
#137 (marked pending).
**Options:** (a) accept the member as part of the codec's contract, documented as a STATE
predicate only (it never goes false on a quiet wire — the T_resp hold needs cadence on top of
it, which is why `Master` owns that pairing rather than the Deframer); (b) keep it out of the
contract and have `Master` track "a FLAG has been seen since the last discard" itself — a
duplicate of parser state the Deframer already holds, and a second place for the two to
disagree; (c) expose a richer query (e.g. the accumulator length) — more surface than any
caller needs today. **Recommended:** (a).
**Ruling:** pending — human (US1 contract text).
**Supersedes:** none.

## 2026-09-06 — Master::set_bit_rate refuses any rate whose byte time truncates to 0 µs (amends the set_bit_rate(0) entry above)

**Context:** red-team at `3a15d29` (#137, LOW). The entry above records the refusal of
`bps == 0` on the grounds that `byte_time_us()` has a nonzero precondition. The same hazard sits
one step out: any `bps > 10 000 000` passes that guard and makes `byte_time_us(bps) ==
10000000u / bps == 0`, at which point `frame_arriving()` degenerates to `now_us <= last_rx_us_`
(the T_resp in-flight hold disappears) and `max_frame_us()` becomes 0 (the courtesy cap
collapses to `defer_origin + T_gap`) — both protections this PR adds become silent no-ops.
Not reachable from trunk §9's own rates (1 Mb/s, 115 200), hence LOW. Implemented in #137: the
refusal is `bps == 0 || byte_time_us(bps) == 0`, i.e. "a byte must take at least 1 µs at this
rate" — the property the engine's timing arithmetic actually needs, stated once. Pinned by
`tests/unit/test_link_master.cpp` "set_bit_rate() refuses a rate whose byte time truncates to
zero".
**Options:** as the entry above — (a) refuse silently (implemented); (b) return a `Status`;
(c) clamp. A further option (d): restrict `set_bit_rate` contractually to trunk §9's two rates.
**Recommended:** (a) with the byte-time condition, not (d): §9's rate table is the protocol's,
and a generic "≥ 1 µs per byte" precondition does not need re-stating if it ever grows.
**Ruling:** pending — human (contract text; folds into the ruling on the entry above).
**Amends:** the 2026-09-06 "set_bit_rate(0)" entry's refusal condition (`bps == 0` becomes
`bps == 0 || byte_time_us(bps) == 0`); its options and recommendation stand.
**Supersedes:** none — amends, not replaces, the 2026-09-06 "set_bit_rate(0)" entry above (its reading stays live; see **Amends:**).

## 2026-09-06 — the protection claimed for the bounded courtesy was FALSE at 3a15d29: poll()'s drain loop stopped early

**Context:** red-team at `3a15d29` (#137, HIGH). The 2026-09-05 "bounded courtesy" entry and
`fire_pending()` claimed, by construction, that "a frame whose start bit lands at or before
`defer_origin + T_gap` is never transmitted over", resting on the drain loop recording EVERY
received byte's end in `last_activity_`. The recording was not unconditional across a `poll()`:
the loop `break`-ed on the byte that concluded an attempt (Answered, or an in-window CRC
failure), leaving bytes already due behind it unread, and `fire_pending()` at the bottom of the
same `poll()` — or a `begin()` the caller issued on the terminal event, the intended F3 loop —
judged the bus idle from a `last_activity_` that predated them. Three reproducers (the engine's
own retry after a CRC failure; the polled node's `Kind::Duplicate` second copy still arriving
when the host starts the next transaction; the F3 loop with a third station's frame) each
transmitted mid-frame at the `T_poll` cadence. Fixed in #137: the drain loop no longer breaks —
every byte due at `now_us` is read, and the attempt-ending byte's outcome is preserved because
the acceptance checks are gated on `awaiting` (re-evaluated per byte and false once the attempt
has ended). Pinned by three cases under "the drain loop reads every due byte". With that fix the
by-construction argument holds as stated (at a constant bit rate, see the "rate change" entry):
the last byte drained at `now` has start <= now < end, so `last_activity_ > now` whenever a
frame is arriving. The `begin()`-without-`poll()` gap tracked in #138 is unchanged by this: its
guarantee assumes `poll(now)` immediately precedes `begin()`, which — after this fix, and not
before it — is sufficient.
**Also changed:** `discards` attribution (review at `3a15d29`, LOW). A frame delivered while a
transaction is open but NOT awaiting a response (gap-deferred before its first or a retried
transmission) used to be charged to `dst_`; spec US2 AC6 scopes the per-destination `discards`
to frames "arriving during an open response window". Now charged to the frame's own claimed
`src` (when in range), as when fully idle. Diagnostic-only (`discards` does not feed trunk §7's
SUSPECT accounting). Contract text amended in #137, marked pending.
**Ruling:** none needed for the fix (a bug against the PR's own stated property); the
attribution change folds into the pending ruling on the amended contract text.
**Supersedes:** none.

---

## 2026-09-06 — Master's constructor does not validate host_addr; an out-of-range value fails silently

**Context:** review at `2056e56` (#137, LOW). `begin()` refuses `dst >= kAddrCount` and
`dst == 0xFF` (entries above), but the constructor's `host_addr` gets no guard. It reaches the
wire as every request's `src` and is the acceptance predicate `f.dst == host_addr_` in
`poll()`. `Master m(wire, clock, 0xFF)`: `encode_frame` validates only `f.dst`, so requests go
out with `src = 0xFF`; a conforming node mirrors it into its answer's `dst`, and the host's own
Deframer discards every such answer as `Discard::ReservedAddress` (trunk §5). Every transaction
then runs three attempts and concludes `Failed{Timeout}`, charging three timeouts per node
toward SUSPECT (trunk §7) with no diagnostic. (Other out-of-range values, `0x10..0xFE`, are
not discarded by anything — the engine works with them, in violation of trunk §5's address
range.) Proved by construction from `link/frame.cpp`'s reserved-address discard; that every caller in the repo passes `ADDR_host` is the current
contents of the repo — a control, not a guarantee. Same class as the `begin(dst == ADDR_host)`
entry above (misuse-hardening of a construction-time constant, not a defect in the engine as
used), so recorded rather than implemented speculatively; the precondition is now STATED on the
constructor in `master.hpp` (trunk §5: `0x00..0x0F`).
**Options:** (a) `begin()` returns `Status::ReservedAddress` while `host_addr_ >= kAddrCount`
(the constructor cannot return a status; this makes the misuse observable at the first call
that would put it on the wire, with a test at exactly `kAddrCount` and at `0xFF`); (b) the
constructor substitutes `ADDR_host` — silent, and a different kind of surprise; (c) leave it
stated-only, since F3 is the sole constructor caller and passes `ADDR_host`.
**Recommendation:** (a), together with the `begin(dst == host_addr_)` refusal above — both
are one ruling on what `begin()` refuses, and one contract clause.
**Ruling:** pending — human (contract-doc amendment + a small behaviour change to `begin()`).
**Supersedes:** none.

## 2026-09-06 — a discarded frame whose claimed src is out of range (0x10..0xFE) is counted nowhere; FR-011 says every discard MUST be counted

**Context:** review at `5a458c2` (#137, MEDIUM). `poll()` charges a discarded frame to `dst`'s
`AddrStats` while `dst`'s response window is open (spec US2 AC6), otherwise to the frame's
own claimed `src` — guarded by `f.src < kAddrCount`, because `AddrStats` is a
`kAddrCount`-entry table (FR-011a: "per trunk address") and an intact frame's `src` is
wire-derived: the Deframer refuses only `dst == 0xFF`, so `src` can be any byte
`0x00..0xFE`. A frame claiming `src = 0x40` while the host is idle or gap-deferred is
therefore discarded and counted in no counter at all — every `stats(a).discards` stays 0 and
`BusStats` has no discard field. FR-011 (`spec.md`) says such frames "MUST be counted
(FR-011a) so tests and the simulator can observe them", with no in-range qualifier; FR-011a's
counter block is per trunk address, so the two requirements can only both be met for
in-range sources. Proved by construction from the table size; `tests/unit/test_link_master.cpp`
"an unsolicited frame while idle whose claimed source is exactly kAddrCount…" pins the
uncounted outcome (it exists to catch the off-by-one write under ASan). `frame.cpp`'s
Deframer does not itself count either (its discards are visible only through `Master`).
**Options:** (a) a bus-level `discards` counter in `BusStats` (data-model.md `BusStats`
gains a field; FR-011a's "per bus" list gains "frames discarded with no attributable
address") — the reading that keeps FR-011 unconditional; (b) amend FR-011 to "counted
against the attributed address when there is one" — records the gap rather than closing it;
(c) have the Deframer refuse `src >= kAddrCount` as a reserved/invalid address, so the frame
never reaches `Master` as an intact frame (trunk §5's range is `0x00..0x0F`; but §5 reserves
only `0xFF`, so this widens the Deframer's refusal beyond the spec text and removes the
observability rather than adding it).
**Recommendation:** (a), folded into #138 item 3 (bus-level outcome/counter) — same field
family, same data-model amendment, and the CRC-attribution entry above also wants a bus-level
count. Not added in #137: a `BusStats` field is a data-model change and this PR's
contract amendments already await a ruling.
**Ruling:** pending — human (data-model amendment).
**Supersedes:** none.

## 2026-09-06 — the T_resp in-flight hold was bounded in BYTES, not time; a time cap was added (amends the FLAG-delimited babble entry above)

**Context:** red-team at `9547634` (#137, HIGH). The 2026-09-06 "FLAG-delimited babble" entry
says the in-flight hold is "exactly `kMaxWire·B` (= F), by construction from the Deframer's
`Discard::TooLong` limit plus `frame_arriving()`'s one-byte slack". That was true only for a
CONTIGUOUS stream. `frame_arriving()` is sampled at `poll()` instants against the MOST RECENT
byte (`now_us <= last_rx_us_ + B`); a station that puts one byte on the wire at every poll
instant satisfies that at every observation the engine makes, and the silence between polls
is never in view. The hold then ended only at `Discard::TooLong` — ~`kMaxWire` wire bytes,
i.e. ~`kMaxWire` POLL PERIODS: 283 850 µs per attempt at the superframe cadence for 143
attacker bytes (0.5 % duty), 852 000 µs for a three-attempt transaction, linear in the poll
period and independent of bit rate. `busy()` true throughout — a liveness/starvation defect
(it does terminate), and a falsified bound stated in four places (`master.hpp`,
`master.cpp`, `contracts/link-cpp.md`, the test's "this is the maximum, not merely an
instance"). Same class as the transmit-side deferral fixed with the courtesy cap; the
receive side had no symmetric cap.
**Fix (implemented in #137):** `frame_arriving()` additionally requires
`now_us <= resp_open_us_ + max_frame_us()` — one worst-case frame from the opening FLAG,
symmetric with the courtesy cap. Labelled: the cap is the MAXIMUM hold at any cadence **by
construction** of the conjunction; for a contiguous stall the cap and the cadence bound
release at the same instant (`deadline + F` when the FLAG lands at `deadline − 1`), so the
contiguous figure and its test are unchanged; no legitimate response can reach the cap (at
most `kMaxWire` contiguous bytes, SC-008's achievable maximum 140 — it closes ≥ 2B before
it). **Demonstrated** by three cases in `tests/unit/test_link_master.cpp` (red before the
cap, green after): one byte per superframe poll releases at the first poll past
`resp_open + F` at both rates; the boundary is exact (`resp_open + F` still holds, `+ 1 µs`
does not); a whole transaction under one byte per poll with a hostile FLAG in every window
concludes `Failed{Timeout}` inside `3·(F + 2G + B) + 3·(n·B + R + F) + 6·P` at both rates.
**Effect on the figures above:** the corrected bound in the entry above now holds at EVERY
poll cadence (before, only at cadences where a byte time exceeds the poll period). The
"~11 superframes" worst case and the ~13× BUS_FAULT trade-off stand; the lever remark
("the in-flight hold's length") now names a real knob: the cap term, not the Deframer limit.
**Alternative considered and rejected:** inter-byte gap detection in the drain loop
(`deframer_.reset()` when a byte starts more than one byte time after the previous ended).
More machinery, a tolerance choice that itself changes the bound (a lenient tolerance gives
2F), and FR-011 counting questions for the synthesised discard. The cap is one conjunct.
**Contract impact:** `contracts/link-cpp.md` (Master engine bound paragraph) and
`data-model.md` §4 "Response acceptance" now state the cap — amended in #137 and marked
pending, folded into the "bounded courtesy" ruling.
**Ruling:** pending — folded into the "bounded courtesy" ruling above.
**Amends:** the 2026-09-06 "FLAG-delimited babble" entry's "by construction … exactly F"
sentence (true of a contiguous stream; the cap makes it true at any cadence).
**Supersedes:** none — amends, not replaces, the 2026-09-06 "FLAG-delimited babble" entry (its reading stays live; see **Amends:**).

## 2026-09-06 — the set_bit_rate byte-time guard screens one caller; `wire_.bit_rate()` is the value the engine computes from (amends the two set_bit_rate entries above)

**Context:** red-team at `2627be9` (#137, LOW). The two entries above frame the refusal in
`Master::set_bit_rate` as "the property the engine's timing arithmetic actually needs, stated
once". It is stated on one door. `ByteWire::set_bit_rate` is public and unguarded, and
`max_frame_us()` / `frame_arriving()` divide by `wire_.bit_rate()` unconditionally — so the
rate the engine's two protections (the T_resp in-flight time cap, the courtesy cap) are
computed from is whatever the wire reports, not what the Master guard screened. Their
reproducer sets 20 Mb/s through `MockWire::set_bit_rate` directly while still delivering bytes
at a 10 µs cadence: `max_frame_us()` is 0, the courtesy cap collapses to `defer_origin + T_gap`,
and the engine transmits into a frame another station opened 50 µs earlier — the HIGH class
this PR fixed, re-reachable through the other door. That door is the one `MockWire`'s
`Kind::Rate` step (T030) and F4's virtual wire will use, since they drive the rate from
scenario data. **Honest scope (rule 11):** the reproducer's wire is internally inconsistent
(reports 20 Mb/s, delivers at 1 Mb/s); what it establishes is that the guard is a control on
one caller, not a guarantee about `bit_rate()`. **Not reachable today** — no in-repo `ByteWire`
reports a rate outside trunk §9 (a control: the current contents of the repo).
**Options:** (a) state the precondition where it belongs — on `ByteWire` itself, in
`contracts/byte-wire-and-clock.md`: "`bit_rate()` reports a rate at which a byte takes at least
1 µs (`byte_time_us(bit_rate()) >= 1`); the engine's timing is undefined otherwise" — and keep
the Master guard as the engine-side check of its own input; (b) re-check `byte_time_us(
wire_.bit_rate()) >= 1` on every poll and treat a violation as … something (there is no good
"something": refusing to poll is a hang, transmitting is the reproducer); (c) nothing beyond
the Master guard, documented as one-door.
**Recommended:** (a). A wire that reports a rate its own byte model cannot express is a
broken wire, and a precondition on the interface is the honest place to say so; the F4
virtual wire and the `Kind::Rate` step then carry the obligation explicitly, and (b) buys no
safe behaviour. Not implemented in #137: a `ByteWire` precondition is a contract change and
the maintainer's ruling. The `master.cpp` `set_bit_rate` comment now says the guard screens
this caller only.
**Ruling:** pending — human (contract text; folds into the set_bit_rate ruling above).
**Amends:** the "stated once" sentence in the two 2026-09-06 set_bit_rate entries.
**Supersedes:** none — amends, not replaces, the two 2026-09-06 set_bit_rate entries above (its reading stays live; see **Amends:**).

---

## 2026-09-06 — the third statement of the T_gap rule (`byte-wire-and-clock.md` "What the engines guarantee to the wire") was left unconditional by the bounded courtesy

**Context:** review at `0263d0f` (#137, MEDIUM). The bounded courtesy (2026-09-05 entry above)
lets the Master transmit past `defer_origin + max_frame + TRUNK_T_gap_us` while bytes are
still arriving — zero idle since the last received byte's stop bit — and
`tests/unit/test_link_master.cpp` "under continuous babble the request goes out exactly at
deferred_to + kMaxWire byte times + T_gap when polled every microsecond" demonstrates it
(green in the `native` job at `928877c`, run 34021853034). #137 amended the two places that
state the T_gap rule as a state-machine description (`contracts/link-cpp.md` "That push-out
is bounded", `data-model.md` §4 "Gap") but not the third, `contracts/byte-wire-and-clock.md`
§"What the engines guarantee to the wire": *"Never call `transmit()` … nor within
`TRUNK_T_gap_us` after the last received byte's final stop bit (Master)."* That is the
strongest of the three wordings — an engine→wire guarantee — and an F3/F4 implementer
reading it would rely on an invariant the engine no longer holds.
**Fix in #137:** the bullet is amended in place (marked *pending a ruling*) to state the
exception in the same terms as `link-cpp.md`: the unconditional rule holds until the
courtesy cap; past it the Master transmits on schedule (trunk §3: sole initiator, no CSMA).
No behaviour changes; no test changes. What the engine actually guarantees the wire is
therefore: never before the previous transmission's returned instant (unconditional), and
never within `T_gap` of received activity *unless* that activity has already deferred the
transmission by one worst-case frame plus `T_gap`.
**Recommended:** rule as one question with the 2026-09-05 "bounded courtesy" entry — if
the courtesy is accepted, all three statements read alike; if it is rejected (FR-010
"never transmit into activity" kept absolute), `byte-wire-and-clock.md` reverts to its
first wording with the other two, and the babble outcome question reopens.
**Ruling:** pending — folded into the "bounded courtesy" ruling above.
**Amends:** the 2026-09-05 "bounded courtesy" entry's list of amended contract text (it
named two of the three statements of the rule).
**Supersedes:** none — amends, not replaces, the 2026-09-05 "bounded courtesy" entry (its reading stays live; see **Amends:**).

---

## 2026-09-06 — Mull path filters are safe at RUN time; the "no include/exclude paths" rule was measured at compile time only

**Context:** `deep-verify` timed out at its 45-minute limit on every push of PR #137 from
`3a15d29` on (eight runs), and `attack-pr` three times at 50 with no verdict (`e1978ab` on
#128; `150d256`, `928877c` on #137). The mutation step was the cost:
`tools/mutate.sh` instruments every translation unit and the runner executed every mutant
it found — 16 906 across the five `link/` oracle binaries at #137's round 19, of which
`tools/mutate_report.py` kept **122** (only `l3/ link/ core/` on changed lines count; 2 671
per binary are Catch2's amalgamated source). `test_link_master` alone: 4 050 mutants,
14 m 30 s on 12 cores, and the runner has 4. The harness's recorded rule (research.md trap
(4), `contracts/tooling.md` step 3, the comments in `mutate.sh`) says `includePaths`/
`excludePaths` "must not be used at all" because excluding Catch2's `main()` TU removes the
mutant dispatch. That was measured with the keys in the **compile-time** config, where it
is true. Measured today on the same round-19 binaries with the keys in the **run-time**
config only (`MULL_CONFIG` → a file with `includePaths: [^<root>/(l3|link|core)/.*]`,
no rebuild): the runner discovers 4 050 mutants and executes 260 — exactly the set
`in_scope()` keeps — in 43 s instead of 14 m 30 s, and all 260 statuses are identical to
the full run (228 killed / 30 survived / 2 timeout both ways; `tools/mutate_diff_reports.py`
lists any mutant that differs, and listed none). Dispatch is unaffected because the filter
is applied after discovery, in the runner, not in the plugin. Demonstrated by that
comparison, on Mull 0.34.0 / LLVM 14. CI runs the LLVM 18 package of the same release;
the gate-budget PR's own `deep-verify` cannot check that (its diff has no scope-dir source,
so the mutation step is a no-op there — #141 review, HIGH), so it was checked in a
`ubuntu:24.04` container with the CI recipe (clang 18.1.3, the pinned Mull LLVM-18 `.deb`,
`--diff origin/main --require`) on #137's tree at `1057568`: the same gate line,
`mutants=122 killed=112 survived=10 labelled[equivalent=8 accepted=2] unlabelled=0`, PASS,
in 2 m 44 s on 12 cores; the unfiltered old-script run in the same container is the
LLVM-18 oracle for the per-mutant comparison (recorded on the PR). Labelled (rule 11): the
"executes only scope-dir mutants, same verdicts" claim is **demonstrated** on LLVM 14
locally and on LLVM 18 in that container; that the 4-core GitHub runner behaves as the
container is **assumed** (core count changes time, not verdicts) until the first T2 push
after merge runs it — the log would show the old ~4 000-mutant counts if the filter matched
nothing there.
**Recommendation:** (a) phase-2 `mull.yml` carries `includePaths` derived from
`mutate.cfg`'s `scope_dirs` (done in the gate-budget PR); the compile-time config stays
mutators-only; the report post-filter stays the gate for what it SEES — it can only remove
mutants, never restore ones the runner declined to execute, so a filter regex that silently
misses one dir would narrow the gate with no signal (#141 review, LOW): `mutate_report.py`
therefore fails a diff-scoped run in which a changed dir has no executed mutant at all (per
changed dir, any line; exempt only when every changed file there is
`mutation-exempt(no-body)`), and `mutate.sh` anchors the regexes on `pwd -P`. A regression
in the runner filter then either makes the run slower or fails it — never narrows it
silently. (b) Amend research.md trap (4)
and `contracts/tooling.md` step 3 to say "at compile time" (done in place, marked). (c) Do
NOT reach for the levers that would make the run faster by making the gate smaller:
`timeout_ms`/`--minimum-timeout` (Timeout ranks as killed), `--coverage-info` (uncovered →
`NoCoverage`, never triaged), a shorter oracle list, or Mull's `gitDiffRef` (drops added
files — the 2026-08-28 entry above). Each is listed so it is not mistaken for free.
**Ruling:** pending — human (tooling contract amendment; `tools/mutate.sh` is gate machinery).
**Supersedes:** none — amends the "corrected measurement" entry of 2026-08-28 (its
`gitDiffRef` finding stands; this entry narrows the *path-filter* finding to compile time).

---

## 2026-09-06 — T034's SC-004 loop bridges MockWire by hand instead of driving it through Kind::Duplicate/CrcError/Silence

**Context:** PR #145 review @ `9e28db5` (MEDIUM) flagged that
`tests/unit/test_link_loop.cpp` (T034, spec 002 issue #52) does not exercise SC-004's
fault matrix through `MockWire`'s own `Kind::Duplicate`, `Kind::CrcError` or a `Respond`
with `delay_us >= TRUNK_T_resp_us` — the mechanisms #52's own task text names ("the
mock's handler for node *n* is `Responder` *n*"). It instead builds a hand-rolled
bidirectional bridge (`host_wire`/`node_wire`, `run_transaction()`) from `MockWire`'s
public surface (`inject_bytes`/`transcript`/`advance_to`) and shapes each attempt's fault
by hand. The reason is real, not an oversight: `MockWire::schedule_respond()`
(`tests/support/mock_wire.cpp`) always fabricates the answer itself — it echoes the
request's own payload rather than invoking a node's `RequestHandler` — so it cannot
exercise a real `Responder`'s replay buffer, which is exactly the property SC-004 exists
to prove (`stats().replays_served`, "handler invocations == 1 per new sequence"). Wiring
`MockWire` itself to call through to a real `RequestHandler` is out of scope for #52
(named as such in the file's own header comment and in tasks.md), and is the same gap
`specs/002-trunk-link-layer/contracts/mock-wire.md:16` already documents as unmet.
**Recommended:** read #52's "each node's `MockWire` handler being that node's `Responder`"
as satisfied by the hand-built bridge (it still runs a real `Master` and a real
`Responder`, over `MockWire`'s existing byte-level RX/TX/transcript surface, with no new
`Kind` and no change to `MockWire`'s step semantics — the bridge supplies only what
`Kind::Respond` cannot yet do), rather than blocking #52 on a `MockWire` rewrite. File the
follow-up named in the PR #145 review/red-team ("make `MockWire::Kind::Respond` answer
via the node's `RequestHandler`, per `contracts/mock-wire.md:16`") as its own issue so
T039 (#57) and T042 (#60) can extend one shared loop instead of each hand-building their
own bridge.
**Ruling:** pending — human (accept the bridge as meeting #52's criterion, or amend the
criterion/file a blocking prerequisite issue instead).
**Supersedes:** none.

---

## 2026-09-06 — SC-004's "retry 1"/"retry 2" columns collapse to one case for Drop/CrcError at TRUNK_retries == 2

**Context:** PR #145 review @ `9e28db5` (MEDIUM) found that
`tests/unit/test_link_loop.cpp`'s "drop through retry 2" and "CRC-corrupted response
through retry 2" cases scripted the identical fault sequence as their "through retry 1"
siblings ({Drop,Drop,Clean} and {Corrupt,Corrupt,Clean} respectively) and asserted
nothing their sibling did not. This is not a copy-paste slip: with `TRUNK_retries == 2`
there are only 3 transmissions per transaction (attempts 0, 1, 2), so for a fault that
must clear before Master can succeed (Drop, CrcError — as opposed to Duplicate/
DelayPastTResp, where the *stale* copy is what "retry 2" delivers, genuinely
distinguishing it from "retry 1"), there are only three distinguishable outcomes:
recovers at attempt 1 ("at attempt 0"), recovers only at attempt 2 (the full retry
budget — labelled "through retry 1" in the file, since the fault persists through that
retry), or never recovers ("after give-up"). A fourth, distinct "through retry 2"
still-recovers case cannot exist for these two fault kinds: recovering after attempt 2
would require a 4th transmission, which `TRUNK_retries == 2` does not allow. #52's task
text (`specs/002-trunk-link-layer/tasks.md` T034) nonetheless states the matrix as
{drop, duplicate, delay-past-T_resp, corrupt} × {attempt 0, retry 1, retry 2, after
give-up} — 16 cells — naming 4 positions per fault uniformly.
**Recommended:** the safe default applied in PR #145: drop the two non-distinguishable
cells (14 SC-004 cases, not 16, for this reason alone — Duplicate and DelayPastTResp keep
all 4 positions, since their "retry 2" case is genuinely distinct) rather than keep
verbatim-duplicate tests solely to match a literal cell count, and record why here rather
than in a code comment. If a maintainer instead wants 16 always-distinct cells, that
requires either raising `TRUNK_retries` for this suite alone (not proposed — it is a
protocol constant, golden rule 1) or redefining what "retry 2" asserts for a
non-terminal-recovery fault when only 3 attempts exist.
**Ruling:** pending — human (accept 14 SC-004 cases for the Drop/CrcError rows, or amend
tasks.md's T034 matrix description).
**Supersedes:** none.

---

## 2026-09-06 — an idle-time discard is charged to the frame's CLAIMED in-range src; nothing authenticates it (amends the out-of-range entry above)

**Context:** review at `1a55116` (#137, LOW). The out-of-range entry above records that a
discarded frame claiming `src >= kAddrCount` is counted nowhere. Its in-range half was not
recorded: with no transaction open (or one gap-deferred), `poll()` charges the discard to
`stats_[f.src]` — spec US2 AC6 and issue #47 AC6 scope the per-destination counter to frames
"arriving during an open response window" (the `awaiting` branch, correct and tested); the
`else` branch is a design choice made in #137 beyond that criterion. `src` is wire-derived and
unauthenticated (trunk §5 reserves only `0xFF`), so any station injecting intact frames with
`src = 0x03` while the host is idle inflates `stats(0x03).discards` and node 3 never
transmitted; nothing downstream can tell the two apart. **Impact today:** diagnostics only —
no health or scheduling decision reads `AddrStats::discards` (a control: the current contents
of `core/` and `link/`); it becomes load-bearing the moment `discards` feeds trunk §7
SUSPECT/OFFLINE accounting, which is the same forgeability the "not attributable to a
specific `dst_`" CRC branch was written to avoid.
**Options:** (a) as the entry above — a bus-level `discards` counter in `BusStats` for every
discard with no open window, in-range or not, so an unattributable discard is never charged
to an address (per-address discards then mean "during that address's own window" and nothing
else); (b) keep the claimed-src attribution and document it as unauthenticated, forbidding
health logic from reading it; (c) drop the `else` branch and count idle-time discards nowhere
(worsens the FR-011 gap the entry above records).
**Recommendation:** (a), the same field and the same data-model amendment as the out-of-range
entry — one counter closes both halves. Folded into #138 item 3 (bus-level outcome/counter),
where the forgeability of the in-range half is now noted alongside the out-of-range half. Not
changed in #137: the same data-model ruling is pending.
**Ruling:** pending — human (data-model amendment; folds into the out-of-range entry's ruling).
**Amends:** the out-of-range entry above (its "otherwise to the frame's own claimed `src`"
description of the code is complete; this entry records that the in-range half is forgeable).
**Supersedes:** none — amends, not replaces, the 2026-09-06 out-of-range entry (its reading stays live; see **Amends:**).

---

## 2026-09-06 — correction to the claimed-src entry above: the counting requirement is FR-011/FR-011a, not AC6, and `awaiting` is wider than "the window"

**Context:** review at `9632347` (#142, LOW). The entry above attributes to "spec US2 AC6" a
scoping AC6 does not state. `specs/002-trunk-link-layer/spec.md` AC6 says only that such a
frame "is discarded and the window keeps running" — nothing about a counter. The counting
requirement is FR-011 ("frames arriving while no transaction is open" are among the frames
that MUST be discarded *and* MUST be counted) and FR-011a (the per-address counter block).
Two corrections to the record the ruling will be made from:
(a) the `else` branch is not a design choice "beyond" any criterion — FR-011 requires an
idle-time discard to be counted *somewhere*, so the branch answers a requirement; what it
chooses is *where* (the frame's claimed `src`). That is exactly why the entry's option (c)
(count nowhere) is a regression against FR-011, and why option (a) — a bus-level counter —
satisfies FR-011 without the forgeable attribution.
(b) "the `awaiting` branch, correct and tested" is wider than the quoted window: `awaiting`
is `open_ && sub_phase_ == AwaitResponse` (`link/master.cpp`), so a frame whose opening FLAG
fell *outside* the window but during an open transaction is also charged to `stats_[dst_]`
(the case "a matching answer whose FLAG opens INSIDE the host's own transmission is discarded
by the window's lower bound" asserts that shape), and its `src` is just as unauthenticated.
The forgeability recorded above is therefore not confined to the `else` branch; the
`awaiting` branch charges the *polled* address rather than the claimed one, which is the
attribution the spec's per-destination counter intends, but it too is driven by wire bytes.
**Options:** unchanged from the entry above; (a) remains the recommendation and now rests on
FR-011 explicitly.
**Recommendation:** (a), as above. No code change here.
**Ruling:** pending — human (same data-model amendment as the entry above).
**Amends:** the 2026-09-06 "claimed in-range src" entry above (its options, recommendation and
impact statement stand; its citation of AC6 as the counting criterion and its "beyond that
criterion" characterisation of the `else` branch are withdrawn in favour of FR-011/FR-011a).
**Supersedes:** none — amends, not replaces, the entry above (see **Amends:**).

## 2026-09-06 — Host T_resp acceptance window stays [tx_end, tx_end + T_resp); T_turn_min binds the node, not the host (PR #142 red-team @43ec6f5)

**Context:** trunk §3 gives the node a turnaround of `T_turn = 20 µs to 100 µs` after the
request's final stop bit (`TRUNK_T_turn_min_us` / `TRUNK_T_turn_max_us`, YAML `timing`).
`link/master.cpp` opens the host's acceptance window at `window_start_us_ = tx_end` and
closes it at `tx_end + T_resp` (`in_window`, `link/master.cpp:368`): a response whose FLAG
opens in `[tx_end, tx_end + T_turn_min)` — zero or sub-minimum turnaround, which §3 forbids a
node to produce — is accepted (delivered, or counted as a CRC failure) rather than
discarded. The `[link][timing:T_resp]` case "opening EXACTLY at tx_end — zero turnaround"
(`tests/unit/test_link_master.cpp:3677`) pins this permissive lower bound on purpose. The
red-team asked whether the host should instead enforce the node's minimum.
**Options:** (a) keep the window permissive — the host accepts any response after its own
transmission ends; §3's minimum is a node obligation (driver-enable timing against the host's
release within 10 µs) that the host cannot observe with its byte-level wire model anyway; a
too-early answer from a real transceiver is a collision, and §7 accounting (CRC failure /
timeout → SUSPECT) already covers what the host does see. (b) enforce `window_start_us_ =
tx_end + T_turn_min_us`: converts a sub-minimum answer into a silent discard plus a full
T_resp timeout — a faulty station's CRC failure becomes a timeout on a different counter, the
exact outcome the CRC lower-bound cases were written to prevent.
**Recommendation:** (a). No code change to `link/`; the test comment at the case above
WILL cite this entry (deferred to the next f2-link PR touching that file; at the time of
this entry it cites only the #142 red-team follow-up) so the permissiveness reads as ruled,
not accidental.
**Ruling:** ADOPTED (a) — human, 2026-09-06 (maintainer, Claude Code session: "keep
permissive, document it"). The host window is `[tx_end, tx_end + T_resp)`; `T_turn_min`
constrains the node under §3 and is not a host-side filter.
**Amends:** none. **Supersedes:** none.

## 2026-09-06 — #110 red-team of the protocol documents: rulings (index entry)

**Context:** issue #110 (protocol/spec red-team, 12 findings F1–F12 plus a backplane trust
class) was walked through finding-by-finding with the maintainer on 2026-09-06; the rulings
are recorded on #110 and, per this file's role as the ruling log, in the entries that
follow. Every finding is adopted as the red-team's defence proposes, with parameters fixed
here. Nothing in this entry or the twelve below changes code: the YAML/codec amendments are
#154 (T3, human-directed), the document amendments #155 (T3), `docs/THREAT-MODEL.md` #156
(T3); the host-side mitigations are `task` stories #157–#162 (`feature:f3-core`), blocked on
a `core/` spec that does not yet exist and therefore not to be released until it does.
**Recommendation:** as recorded per finding below.
**Ruling:** ADOPTED — human, 2026-09-06 (maintainer, via #110). #110 stays open as the index
until #154, #155 and #156 merge.
**Amends:** none. **Supersedes:** none.

## 2026-09-06 — #110 F1: a backplane that never answers, and a node that answers ERR_BUSY forever, are both bounded

**Context:** trunk §8 lets a backplane answer `ERR_BUSY` indefinitely while its module bus
is wedged (§10.5 names persistent busy as open); §7 counts "failed transactions" toward
SUSPECT without defining failure, and a valid `ERR_BUSY` response is not a failed
transaction, so a stuck slot never trips §7. L4 preset recall then waits forever.
**Recommendation:** (a) trunk §8 / spec §28: slot recovery is mandatory — three consecutive
module-bus timeouts on one slot ⇒ the backplane isolates or power-cycles that slot, raises a
backplane event, and the host marks the node `FAULT`; slots are serviced round-robin so one
wedged slot cannot starve the others. (b) trunk §7: "failed transaction" is defined as a
timeout, a CRC failure, or exhaustion of the `ERR_BUSY` budget — eight consecutive
`ERR_BUSY` answers to the same request ⇒ exponential back-off for that node and a report to
L4. (c) L4: every preset recall has a deadline and a partial-completion report.
**Ruling:** ADOPTED (a)(b)(c) — human, 2026-09-06 (via #110). Documents: #155. Host budget /
back-off: #157.
**Amends:** the 2026-09-03 entry "Persistent ERR_BUSY: no bound anywhere detects a wedged
bridge" — (b) is the bound that entry asked for; its pending Ruling is filled with a pointer
here. Its layering consequence (busy-detection is payload inspection, which `link/` must not
do) is not ruled here and is carried to #155/#157. **Supersedes:** none.

## 2026-09-06 — #110 F2: ERROR.detail is capped; the §6 status-poll budget is derived, not asserted

**Context:** `ERROR.detail` had no maximum, so an ERROR response could be as large as any
payload; trunk §6 asserted a fixed status-poll cadence "independent of load" for every
enrolled node, which no frame-time arithmetic supports at the enrolled-node maximum.
**Recommendation:** (a) YAML `l3_payloads.ERROR.response.detail.max: 4`, enforced by both
codecs (C++ and Python reference). (b) trunk §6: status polls are
round-robin with a stated staleness bound; the maximum satisfiable enrolled-node count is
derived from frame times at the rate in use; "independent of load" is withdrawn as written.
(c) the scheduler budgets by measured frame time and demotes a node that consistently
overruns its share.
**Ruling:** ADOPTED (a)(b)(c) — human, 2026-09-06 (via #110). YAML/codecs: #154. §6: #155.
Scheduler: #162.
**Amends:** none. **Supersedes:** none.

## 2026-09-06 — #110 F3: the descriptor cache is a latency optimisation, never a trust boundary

**Context:** protocol §4.1 keys the descriptor cache on `desc_crc` (CRC-16); the red-team
showed any module can present another module's `desc_crc` and inherit its cached descriptor,
including anything power-relevant the host had derived from it.
**Recommendation:** state in §4.1 that `desc_crc` detects corruption and staleness, not
forgery; nothing power- or safety-relevant is decided from a cache hit — rail approval is
re-derived from a descriptor read at this insertion and enforced by the per-slot eFuse (spec
§16). Accepted risk, named: on an open platform any module can impersonate any other at the
descriptor level. The hash is not strengthened (a stronger hash without a key changes
nothing about impersonation).
**Ruling:** ADOPTED — human, 2026-09-06 (via #110): "state the trust boundary, don't
strengthen the hash". Documents: #155; the accepted risk is listed in `docs/THREAT-MODEL.md`
(#156).
**Amends:** none. **Supersedes:** none.

## 2026-09-06 — #110 F4: the bit rate is a property of the trunk; the reference rate wins whenever any enrolled node answers at it (supersedes "the answering rate wins")

**Context:** the 2026-08-29 ruling "after BUS_FAULT, alternate probe rates; the answering
rate wins" lets a single node that answers only at the fallback rate pin the whole trunk to
the fallback — a one-node downgrade of every other node's `T_poll` and §6 budget, which the
red-team demonstrated on paper as a babbling or misconfigured node.
**Recommendation:** the reference rate is in use whenever any enrolled node answers at it;
the host falls back only while no enrolled node answers at the reference rate; while at the
fallback rate the host re-probes the reference rate on a fixed cadence and returns to it as
soon as a quorum of enrolled nodes answers there. `T_poll` and the §6 budget derive from the
rate in use. Babble containment is named in trunk §3 as a required node behaviour with a
test (driver-enable watchdog / transceiver fault detection). Consequences for open work: #61
(T043, `queued`) says "clear-and-pin on the first valid answer" and #59 (T041) rests on the
same ruling — both amended by comment on 2026-09-06; `specs/002-trunk-link-layer/
data-model.md` §7 and `contracts/link-cpp.md` carry the superseded text and are amended in
#155.
**Ruling:** ADOPTED — human, 2026-09-06 (via #110): "supersede: reference rate wins,
fallback re-probes". The superseded text, verbatim from the 2026-08-29 entry: "the first
valid response at either rate clears the fault exactly once (recovery notification) and that
rate becomes the rate in use until the layer above changes it."
**Amends:** none.
**Supersedes:** the 2026-08-29 entry "Trunk §7: after BUS_FAULT, alternate probe rates; the
answering rate wins" (spec FR-026) — its alternation of probe rates while BUS_FAULT is
declared stands; its "answering rate becomes the rate in use" clause is replaced by the rule
above.

## 2026-09-06 — #110 F5: a response's L3 node_id must belong to the answering L2 src

**Context:** nothing binds the L3 `node_id` inside a response to the L2 `src` that carried
it; a backplane (or any station) can answer on behalf of a node id it does not own and the
host attributes the answer to that node.
**Recommendation:** receiver rule in protocol §2 and trunk §5: a response is acceptable only
if its L3 `node_id` is the answering L2 `src` itself, or a node id the host assigned to a
slot of that `src`; anything else is discarded and counted.
**Ruling:** ADOPTED — human, 2026-09-06 (via #110). Documents: #155. Host check: #158.
**Amends:** none. **Supersedes:** none.

## 2026-09-06 — #110 F6: GET_EVENT drains are bounded per node per superframe; remaining_count is advisory (closes protocol §6 open question 4)

**Context:** protocol §3.1 (the `GET_EVENT` row: "repeat until empty", `u8 remaining_count`)
lets a node report `remaining_count` and the host drain until it reaches zero; a node that always reports more pending events holds the host in its drain loop
and starves the superframe. §6 open question 4 asked how many events a single drain may
take.
**Recommendation:** at most K = 1 `GET_EVENT` per node per superframe; a per-node event-rate
budget (events per second over a window, value fixed with the scheduler) beyond which the
host stops draining, marks the node `FAULT`, and reports to L4; `remaining_count` is
advisory and never sizes a buffer or bounds a loop.
**Ruling:** ADOPTED with K = 1 — human, 2026-09-06 (via #110). Protocol §6 open question 4 is
ruled with this K. Documents: #155. Host drain: #159.
**Amends:** none. **Supersedes:** none.

## 2026-09-06 — #110 F7: descriptor field ranges, uniqueness and referential integrity are enforced; power under-declaration is an accepted risk controlled by the eFuse

**Context:** descriptor TLVs carried physical quantities (rail currents, settle times, tube
supply parameters, audio levels) with no YAML range, no uniqueness rule for `CHANNEL.index`
/ `PARAM.param_id`, no consistency rule between a declared power class and declared
currents, and no check that `PARAM_ENUM` / scope references resolve.
**Recommendation:** (1) YAML ranges enforced by the codecs: `POWER_LV` per-rail maxima from
spec §15; `SWITCHING.settle_ms ≤ 5000`; `POWER_TUBE` bounds from §18/§19 including the 40 mA
v1 B+ cap; `AUDIO` bounds from §9. (2) YAML `limits.max_channels` and `limits.max_params`;
`CHANNEL.index` and `PARAM.param_id` MUST be unique within a descriptor (streaming
validator, bitmap). (3) §18: declared currents must be consistent with the declared class;
the class wins on disagreement. (4) a documented host referential-integrity pass before a
descriptor is admitted (dangling `PARAM_ENUM` / scope references rejected) — a second pass,
not part of the streaming validator. (5) §16 names the per-slot eFuse as the control for
power under-declaration (accepted risk).
**Ruling:** ADOPTED (1)–(5) — human, 2026-09-06 (via #110). YAML/codecs: #154. Documents:
#155. Integrity pass: #160. Accepted risk (5): `docs/THREAT-MODEL.md` (#156).
**Amends:** none. **Supersedes:** none.

## 2026-09-06 — #110 F8: a READ_DESC response must echo the requested offset and be non-empty while bytes remain; the host's read budget is fixed

**Context:** protocol §3.1/§4 let a module answer `READ_DESC` with an empty chunk, or a chunk
at a different offset, indefinitely; the host's read loop then never terminates and a
`desc_crc` change without re-insertion is silently re-read.
**Recommendation:** protocol: a `READ_DESC` response MUST carry the requested offset and MUST
be non-empty while bytes remain — violations are a module fault, not a retry. Host: a fixed
attempt budget per descriptor read ⇒ `descriptor-unreadable` state, reported, never retried
forever; one full read per physical attach; a `desc_crc` change without re-insertion is
itself a fault signal.
**Ruling:** ADOPTED — human, 2026-09-06 (via #110). Documents: #155. Host budget: #161.
**Amends:** none. **Supersedes:** none.

## 2026-09-06 — #110 F9: CHANNEL_SETTLED detail is (channel, seq) and the host cross-checks GET_STATUS before un-muting

**Context:** `CHANNEL_SETTLED` carried no detail, so a settle event could not be matched to
the `SELECT_CHANNEL` that caused it; a stale or forged settle would un-mute the wrong
channel.
**Recommendation:** protocol §3.4 + YAML: `CHANNEL_SETTLED` detail is `u8 channel, u8 seq`
(`seq` echoes the causing `SELECT_CHANNEL`); the host accepts a settle only for the channel
it asked for and cross-checks `GET_STATUS.active_channel` before un-muting.
**Ruling:** ADOPTED — human, 2026-09-06 (via #110): "define detail = channel + seq".
YAML/codecs: #154.
**Amends:** none. **Supersedes:** none.

## 2026-09-06 — #110 F10: the L3 message limit and the L3 payload limit are two symbols; an unforwardable module message is ERR_INTERNAL plus a module fault

**Context:** one `limits.max_l3_*` value served both as the trunk-frame budget and as the
payload budget, so `PAYLOAD_INFO`'s `*_max` columns could not be satisfied at the frame
limit; trunk §8 did not say what a backplane does with a module message it cannot fit into a
trunk frame.
**Recommendation:** YAML: `limits.max_l3_message: 64` (the trunk-frame budget) and derived
`limits.max_l3_payload: 59`; `PAYLOAD_INFO` `*_max` columns corrected to the payload limit.
Trunk §8: an unforwardable module message ⇒ `ERR_INTERNAL` to the host and a module fault
raised.
**Ruling:** ADOPTED — human, 2026-09-06 (via #110): "split the limits; bridge answers
ERR_INTERNAL". YAML/codecs: #154. §8: #155.
**Amends:** none. **Supersedes:** none.

## 2026-09-06 — #110 F11: IRQ# is sampled per slot and a slot whose alerts yield no event is masked

**Context:** a module holding IRQ# asserted with nothing to report makes the host poll the
whole backplane for events on every alert cycle, indefinitely.
**Recommendation:** spec §28 + trunk §8: IRQ# is sampled per slot; after N alert cycles in
which a slot's alert yields no event (N fixed with the backplane design; suggest 3) the
backplane masks that slot's IRQ#, raises a backplane event, and the host marks the node
`FAULT`.
**Ruling:** ADOPTED — human, 2026-09-06 (via #110). Documents: #155.
**Amends:** none. **Supersedes:** none.

## 2026-09-06 — #110 F12: slots per backplane are capped in the YAML and node ids are allocated from a per-backplane quota (constraint on the future BP_SLOT_MAP)

**Context:** `BP_SLOT_MAP` is not yet defined; without a cap a backplane can claim the whole
node-id space for its slots.
**Recommendation:** YAML caps slots per backplane (spec §5 reference: 8–16 FX, 4 preamp) as a
limit symbol now; the host allocates node ids from a fixed per-backplane quota. The
`BP_SLOT_MAP` format itself is defined later and must respect the cap.
**Ruling:** ADOPTED — human, 2026-09-06 (via #110). Limit symbol: #154; format: deferred to
the `BP_SLOT_MAP` definition.
**Amends:** none. **Supersedes:** none.

## 2026-09-06 — #110 backplane class: the trust boundary is the backplane; docs/THREAT-MODEL.md becomes an authoritative document

**Context:** several #110 attacks (rail control, routing, power readback, slot events) need
no protocol flaw — the backplane owns the rails and the module bus, and no L3 mechanism can
constrain it. The accepted risks named in F3, F4 and F7(5) had no single place to live.
**Recommendation:** new `docs/THREAT-MODEL.md`: the trust boundary is the backplane; modules
are untrusted; no L3 mechanism constrains a backplane; spec §22 hardware protection is the
control for power readback; the accepted risks from F3, F4, F7(5) and #110's backplane item
7 are listed there. Added to CLAUDE.md's authoritative-documents list in the same PR. From
the "attacks that failed" residue: the receiver rule "preserve reserved L3 flag bits, ignore
them" (FR-012) gets one sentence in `docs/protocol-l3.md` §3 (#155).
**Ruling:** ADOPTED — human, 2026-09-06 (via #110): "add docs/THREAT-MODEL.md". #156
(T3; CLAUDE.md is a ground-truth artefact).
**Amends:** none. **Supersedes:** none.

---

## 2026-09-06 — Responder replay buffer: keyed on sequence alone, `data-model.md` §5 is silent on the requester

**Context:** red-team on #149 (Responder engine, T033/T035) at `033182a`, escalated to
BLOCKING (an earlier pass at `907dfbe` had classified the same finding a FOLLOW-UP,
reasoning that closing it needed a spec change). `data-model.md` §5 states the replay
condition as `retry == 1 && valid && seq == buffer.seq -> retransmit buffer` — silent on
whether the retrying frame's `src` must match the station the buffered response was
actually encoded for. As written, a second station that happens to send `retry=1` with a
sequence colliding with another station's most-recently-buffered sequence is served that
other station's response, addressed to that other station (the replying frame's `dst` is
read back out of the buffer, not recomputed from the current request's `src`) — the
requester that actually asked gets no answer at all, and an uninvolved third party
receives an unsolicited frame in what looks like its own response window. Trunk §5
reserves only `0xFF`; `src` is otherwise wire-derived and unauthenticated, the same
forgeability class as the two `claimed-src` entries above, and, per the red-team pass, the
same class of hazard this PR already hardens against for `src == 0xFF` (`link/responder.cpp`
"Request acceptance") — rejecting one wire-forgeable `src` misuse while accepting another
in code added by the same PR was judged inconsistent enough to be blocking rather than
deferred.
**Impact today:** `sim/`/`core/` do not yet drive `Responder` (F4 wiring is a later
story), so nothing downstream observes the misdirected frame yet; it becomes load-bearing
the moment a virtual backplane or scenario puts more than one station on a trunk segment
sharing a `Responder`.
**Options:** (a) key the replay strictly on `{peer, seq}` — store the requester's `src`
alongside the buffered response and require it match before treating a retry as a replay;
any mismatch falls through to the "otherwise -> new" branch (handler invoked, buffer
replaced, addressed to whoever actually asked) rather than being discarded outright. (b)
same, but discard-and-count a colliding-sequence retry from a different station instead of
answering it as new. (c) leave `data-model.md` §5 exactly as written and accept the
cross-station replay as spec-literal.
**Recommendation:** (a) — it never withholds an answer from a station that legitimately
asked, never sends a stale answer to the wrong address, and degrades to exactly the
documented behaviour whenever there is only one station retrying (the case §5 was written
for). Safe default per CLAUDE.md ("implement nothing speculative … proceed only if a safe
default exists"): stricter than the literal §5 text, never contradicts it (identical
behaviour whenever `f.src == buffer.peer`, which is every single-master trunk case the
existing test suite exercises), and needs no `protocol/omgp-protocol.yaml` or golden-vector
change.
**Ruling:** adopted as the safe default per CLAUDE.md, pending a human look at whether
`data-model.md` §5 should be reworded to state the `{peer, seq}` key explicitly (a
documentation clarification only, no further behaviour change). Implemented in
`link/responder.hpp` (`ReplayBuffer::peer`) and `link/responder.cpp` (`on_request`'s
`is_replay` condition), with `tests/unit/test_link_responder.cpp` ("a retry-flagged
request from a different station is treated as new rather than replaying the previous
requester's buffered answer") written first and confirmed red before the fix.
**Supersedes:** none.

---

## 2026-09-06 — Responder: a request arriving while a response is Scheduled or Transmitting is held, not discarded; `data-model.md` §5 is silent on arrival while busy

**Raised by:** red team @e510b29 on PR #149 (finding 1, BLOCKING), confirmed by the review
at the same head (finding 1). **Affects:** `link/responder.cpp` (`Responder::poll`,
`Responder::on_request`), `specs/002-trunk-link-layer/data-model.md` §5, FR-014/FR-015/FR-016.
**Question:** `data-model.md` §5 gives the Responder three states (Listening, Scheduled,
Transmitting) and an acceptance rule for an intact request addressed to the node, but says
nothing about a request whose bytes arrive while a previous response is Scheduled (encoded,
not yet due) or Transmitting (occupying the half-duplex wire). The implementation at
`e510b29` discarded and counted such a request. The red team showed two consequences with
runnable reproducers: (1) two requests queued ahead of one late `poll()` — the second is
swallowed (at a 2 ms poll period, 2 of 8 well-spaced requests were answered), against
FR-014's "transmitted at once … counted, not dropped"; (2) a trunk §7 retry queued behind
its own original in the same batch is discarded rather than replayed, against FR-015's
unconditional MUST ("retry=1 with the same seq MUST be answered from the replay buffer").
Neither is a rogue station: both are one master obeying the spec, and one Responder polled
infrequently — the FR-014 situation by definition.
**Options:** (a) stop draining the wire while `state_ != Listening`: bytes already arrived
stay in the wire's receive queue (a UART's RX FIFO does the same) and are decoded, intact,
by the first `poll()` after the wire is free; the queued request then takes the ordinary
path — replay, new, late-counted — with the single replay buffer untouched. (b) keep
draining and keep the discard-and-count (the `e510b29` behaviour). (c) a second replay
buffer / queue of pending responses.
**Recommendation:** (a). It needs no new state or storage (rule 5: no allocation after
init), never overwrites a pending response, never transmits outside a window (FR-017), and
makes FR-014/015/016 hold uniformly for queued requests. The observable cost: a request
that arrives while a response is pending is answered after that response, from the wire's
own queue — its own turnaround window may then be missed, in which case it is counted
late exactly as FR-014 prescribes for any late poll. (b) violates FR-015 as shown. (c) is
speculative storage for a case the spec does not describe.
**Ruling:** (a) adopted as the safe default per CLAUDE.md (stricter than the literal §5
text, contradicts nothing in it; identical behaviour whenever no request arrives while
busy, which is every case the pre-`e510b29` suite exercised). Implemented in
`link/responder.cpp` (`poll()` breaks out of its drain loop while `state_ != Listening`;
the `on_request` busy-discard branch is removed), with four cases in
`tests/unit/test_link_responder.cpp` written first and red at `94f6424`. Pending a human
look at whether `data-model.md` §5 should state the "held in the receive queue" rule
explicitly (a documentation clarification only).
**Supersedes:** none.

---

## 2026-09-06 — Responder "held, not discarded" rests on the `ByteWire` receive-queue depth; no minimum depth is stated anywhere

**Context:** the 2026-09-06 entry above ("held, not discarded") adopts option (a): while a
response is Scheduled or Transmitting the Responder does not drain the wire, and bytes that
arrive meanwhile wait in the wire's receive queue. Both the red team and the review at
`70d7660` (PR #149) point out what that assumption depends on and that nothing pins it:
the engine declines to drain for up to `T_turn_max` plus one full response transmission,
so the queue must hold whatever a conformant peer can send in that interval. `MockWire`
holds `4 * kMaxWire` (568 B) and fails loudly on overflow — the native suite can never
lose a held request silently (six max-payload requests, 432 B, were queued behind a busy
Responder with `discards == 0`, red team @`70d7660`). The ESP32-S3 hardware UART FIFO is
128 B, smaller than one worst-case stuffed frame (`kMaxWire == 142`); on the target the
rule would rest on the IDF driver's ring buffer, not on the hardware FIFO. Unverified:
the target was not instrumented (both jobs state this).
**Options:** (a) state a minimum receive-queue depth in the `ByteWire` contract
(`contracts/link-cpp.md`) that any implementation must provide, e.g. at least
`2 * kMaxWire`, with an overrun counter the Responder's `stats()` can surface; (b) leave
the depth to each `ByteWire` implementation and re-open when the F4 hardware transport is
written; (c) have the Responder drain into a second buffer while busy (option (c) of the
2026-09-06 entry — rejected there as speculative storage).
**Recommendation:** (a), as a contract line only, at the point the hardware `ByteWire` is
specified; no code change in #149. Under a strict-poll master (trunk §3) nothing arrives
while the Responder is busy, so the depth only matters for the coarse-poll case the rule
exists for; a stated minimum makes the FR-015 claim "held, not discarded" checkable
against an implementation rather than assumed. Material to the human ruling the
2026-09-06 entry awaits (whether `data-model.md` §5 should state the held-in-queue rule).
**Ruling:** pending human. No code change; the Responder's behaviour and its tests are as
in the 2026-09-06 entry.
**Supersedes:** none (adds a dependency note to the 2026-09-06 entry; does not change it).

---

## 2026-09-06 — Responder `{peer, seq}` replay key (adopted above) lets one forged frame evict a pending station's replay entry and defeat FR-015 for that station's own retry

**Raised by:** red team on PR #145 @ `d0cc3bf` (finding 1, BLOCKING). **Affects:**
`link/responder.cpp` (`on_request`'s `is_replay` condition and its `buffer_` overwrite on
the "new" path), `link/responder.hpp` (`ReplayBuffer::peer`), the entry immediately above
this one ("Responder replay buffer: keyed on sequence alone..."), and
`tests/unit/test_link_responder.cpp:793` ("a retry-flagged request from a different
station is treated as new...").
**Question:** the entry above closed one hazard (a colliding-sequence retry from a
different station stealing another station's buffered answer) by keying replay on
`{peer, seq}` and, when the incoming frame's `src` does not match `buffer_.peer`, falling
through to "treated as new" — re-invoking the handler and overwriting `buffer_` with the
new frame's own `peer`. Trunk §3 states "the host is the only initiator", but `on_request`
does not check `f.src` against `ADDR_host` for either path (replay or new); nothing stops
an impostor frame with an arbitrary `src` from taking the "new" branch. Demonstrated
(`d0cc3bf`, finding 1): after the host's request seq 5 is answered (`buffer_.peer ==
ADDR_host`), a single retry-flagged frame `{src: 0x02, seq: 5}` is accepted as new
(`buffer_.peer` becomes `0x02`), so the host's own subsequent genuine retry of seq 5 no
longer matches `buffer_.peer` either — it is *also* treated as new, re-invoking the
handler a second time and violating FR-015's unconditional "retry MUST NOT invoke the
application" for the station that actually asked. One forged frame, from any address,
timed between a request and its own retry, defeats replay for that request.
**Options:** (a) validate `f.src == ADDR_host` for every accepted request (replay or new)
and discard-and-count anything else, per trunk §3's "the host is the only initiator" —
closes the hole outright, since an impostor's frame can then never reach either branch.
(b) leave `{peer, seq}` keying as the sole guard (today's code) and accept that any
station able to place one well-timed frame on the trunk can evict another station's
in-flight replay entry.
**Why not implemented directly as a safe default:** (a) is the FR-conformant fix, but it
requires reversing `tests/unit/test_link_responder.cpp:793-820`'s assertions (that test
currently REQUIREs a frame from a non-host `src` (`0x07`) to be treated as new, answered,
and counted — exactly the acceptance (a) would remove); that test itself resulted from an
explicit prior ruling (the entry above). CLAUDE.md and `docs/OPERATING-POLICY.md` both
prohibit an agent weakening, skipping or reversing an existing test's assertions without
an explicit human instruction naming it, so no code or test change is made here despite
(a) being the recommendation.
**Recommendation:** (a): add the `f.src == ADDR_host` check (mirroring the existing
`f.src == 0xFF` reserved-address discard already in `on_request`) and update
`tests/unit/test_link_responder.cpp:793` to assert the frame from `0x07` is discarded
(`stats().discards` increments, no transmission, no handler call) rather than answered as
a new station — a human decision, since it reverses that test's current, deliberately-set
expectation.
**Ruling:** pending — human (reverses an existing test's assertions; not a decision an
agent makes unilaterally per CLAUDE.md/OPERATING-POLICY §2).
**Amends:** none. **Supersedes:** none — narrows the entry above pending the ruling.

---

## 2026-09-06 — `RequestHandler`'s "called at most once per NEW sequence" doc claim overclaims: a request duplicated with the retry bit CLEAR re-invokes it

**Raised by:** red team on PR #145 @ `d0cc3bf` (finding 2, BLOCKING as a CLAUDE.md rule 11
unlabelled-claim violation); the same underlying gap was raised as a non-blocking spec
question ("FU-3") by an earlier red-team pass on this branch but was never recorded here.
**Affects:** `link/responder.hpp` (`RequestHandler`'s doc comment), `link/responder.cpp`
(`on_request`'s `is_replay` condition, `f.retry && ...`), spec 002 SC-004 ("the
application runs exactly once per new sequence"), trunk §7 line 70 ("a retry of a
sequence it already answered" — silent on a duplicate with the retry bit clear).
**Question:** `data-model.md` §5 and trunk §7 gate replay on the retry bit; `link/
responder.hpp`'s `RequestHandler` comment claims unconditionally "Called at most once per
NEW sequence ... a retried sequence is replayed from the buffer, never re-invoking this"
— true only for `f.retry == true`. A request-shaped frame duplicated on the wire with
`retry == 0` and a colliding `seq` (a reflection, a repeater, or `Kind::Duplicate` applied
to the request direction rather than the response) satisfies data-model.md §5's literal
replay condition being false, so `on_request` takes the "new" branch: the handler runs
again and a second response is transmitted. This is the same code path SC-004 pins for
the *response* direction (drop/duplicate/delay/corrupt); no cell in
`tests/unit/test_link_loop.cpp`'s matrix duplicates a *request*.
**Options:** (a) treat any frame matching `{seq == buffer_.seq, valid}` as a replay
regardless of the retry bit — closes the SC-004 gap but contradicts data-model.md §5's
literal condition and trunk §7's wording, and would also suppress a legitimately new
request that coincidentally reuses a sequence value 16 transactions later (the wrap case
already on file above). (b) leave the code as-is (matches the ratified design) and correct
only the doc comment's claim to state its actual, narrower scope. (c) both a comment fix
and a new pinning test recording today's behaviour, without changing it.
**Recommendation:** (c) as the immediate, safe-default step (no behaviour change, so
nothing this repo currently depends on can regress): reworded the `RequestHandler` comment
in `link/responder.hpp` to state the guarantee only holds when the repeat carries the
retry bit, and added a case to `tests/unit/test_link_responder.cpp` pinning the current
(gap) behaviour for a retry-bit-clear duplicate. Whether (a) should also be adopted — i.e.
whether SC-004's "exactly once per new sequence" or data-model.md §5's retry-bit-gated
replay condition governs — is a spec conflict for a human to resolve, same class as the
entry immediately above.
**Ruling:** pending — human (spec conflict between SC-004 and data-model.md §5 / trunk
§7). Comment relabelled and pinning test added as the non-behaviour-changing part.
**Amends:** none. **Supersedes:** none.

---

## 2026-09-06 — Responder "held, not discarded" makes the held-request queue unbounded and uncounted: one answer per `poll()`, oldest first; FR-014 and FR-017 conflict on a stale queued request

**Raised by:** red team @17554c8 on PR #149 (finding 1, MEDIUM; finding 2, LOW), confirmed
by the review at the same head (finding 1, MEDIUM, mechanism by construction from the diff).
**Affects:** `link/responder.cpp` (`Responder::poll`), `specs/002-trunk-link-layer/spec.md`
FR-014 / FR-017, the two 2026-09-06 entries above ("held, not discarded"; "rests on the
`ByteWire` receive-queue depth").
**Context:** the "held, not discarded" ruling above (option (a)) stops draining the wire
while a response is Scheduled or Transmitting. Independently, one accepted request sets
`state_ = Scheduled` and ends the drain loop, and simulated time does not advance inside a
`poll()`. Together: **one `poll()` answers exactly one queued request, oldest first**, and
nothing bounds how old a queued request may be, how deep the backlog grows, or counts a
request while it waits. Reproducer (now `tests/unit/test_link_responder.cpp`, "requests
arriving faster than poll() is called queue up …", CHECK-pinned as an open question, not
asserted as desired): a station at 0x07 sends one request to the node every 300 µs, the
engine is polled every 1 ms, the host sends its own request at t = 8 ms. At `17554c8` and at
this head: 21 polls, 20 answers, all 20 counted `late_responses`, the host **never answered**
12 ms after its request (host `T_resp` = 200 µs), `discards == 0`. Same run against the
pre-`70d7660` rule (drain always; discard-and-count a request decoded while busy; red team's
counterfactual): the host answered at the very next poll, `discards == 48`. On MockWire the
backlog eventually overflows the receive queue loudly (`RX queue capacity exceeded`, red
team finding 2); on the ESP32-S3 UART FIFO it would overflow silently, and no `AddrStats`
field moves either way — the Responder cannot observe an overrun through `ByteWire`.
**Two spec requirements pull opposite ways on a stale queued request.** FR-014: "If the
engine is not given control until after that window has closed … it MUST still transmit —
at once — and MUST count the occurrence as a late response"; the suite pins this at +1000 µs
after `T_turn_max` (three cases: the single late poll, the queued pair, the queued retry).
FR-017: the engine "MUST never transmit outside a response window". The spec's own Edge
Cases resolve the pair for a *single* late poll ("the late poll is the simulator's defect,
not the protocol's, and the counter is how a scenario notices") — not for a steady state in
which every answer is late and the freshest request is unreachable. Both red teams cited
the spec: @e510b29 asked for hold (FR-014/FR-015, "none dropped", which the ruling above
adopted and the case "eight well-spaced requests … whatever the poll cadence" pins);
@17554c8 asks for a bound (FR-017, and visibility). The traffic that reaches the state is
either out-of-spec (only the host originates requests, trunk §3; a second station's
requests are hostile or a bus fault) or a conformant host plus a poll cadence coarser than
`T_resp` — the simulator's defect FR-014 is written for, but continuous rather than one-off.
**Options:** (a) keep the rule (this head): unbounded, uncounted; FR-014 literal. (b) revert
to drain-always with discard-and-count while busy (`e510b29`): bounded by the poll cadence
and counted, but drops a queued trunk §7 retry against FR-015 (red team @e510b29 finding 1)
and drops a fresh request behind a stale pending response. (c) **bound the hold by
staleness**: a request decoded at `now_us > request_end_us + TRUNK_T_resp_us` is dropped and
counted — the host's own exclusive timeout is the one point the spec gives at which the
master has *provably* abandoned the transaction (spec.md Edge Cases: "a start bit at or
after `T_resp` is late"; "the host discards the late response"), so nothing the host could
still accept is ever dropped, and the backlog can never exceed one `T_resp` of arrivals per
poll. FR-014's late-transmit survives for lateness in `(T_turn_max, T_resp]`; beyond it the
counter, not the transmission, is what the scenario notices. Counter: a new `AddrStats`
field (`stale_requests`) — a `contracts/link-cpp.md` and `data-model.md` §5 change — or
reuse `discards` (no interface change, but conflates a stale request with a corrupt frame).
(d) cap the queue depth instead (e.g. answer at most N per poll, drop-and-count the rest):
bounds depth but not age, and N is a number the spec does not have.
**Recommendation:** (c), with a distinct counter. It is the smallest rule that answers every
request the host can still accept, never transmits a response the host is guaranteed to
discard (which on a shared half-duplex bus can land inside the *next* transaction's window
— the babble Edge Case — and fail another node's poll), and keeps the loss visible. It
bounds a queued retry the same way: a retry decoded within `T_resp` of its *own* stop bit
is replayed (FR-015); a staler one is dropped and counted like any stale request — the host
that sent it has already timed it out too. It is a **spec change**: FR-014's "MUST still
transmit" and FR-015's "MUST retransmit" as written have no upper bound, and three existing
cases pin +1000 µs (the single late poll, the queued pair, the queued retry); under (c)
those cases move to lateness ≤ `T_resp` and a fourth asserts drop-and-count beyond it.
Not implemented here: CLAUDE.md "documents win; implement nothing speculative" — a MUST is
being narrowed, and the two entries above already await the same human's look at
`data-model.md` §5. What lands in #149 is the pinning test, this entry, and a comment in
`Responder::poll` stating the consequence.
**Corrects two sentences in the "held, not discarded" entry above** (review @17554c8
finding 2): its recommendation says option (a) "never transmits outside a window (FR-017)"
and "makes FR-014/015/016 hold uniformly for queued requests". Neither is unconditionally
true: every held response answered after its window transmits outside it (that is what
`late_responses` counts — 20 of 20 in the reproducer), and FR-014's late-transmit clause
is what (a) leans on, not FR-017. The accurate form: (a) never transmits *over* another
transmission (trunk §3 half-duplex, by construction of the Transmitting state) and never
drops a request or a retry; it does transmit outside the window whenever the poll is late,
counted. The entry's Impact/Options text stands; only those two absolutes are withdrawn.
**Ruling:** pending — human. Options (a)–(d) above; the recommendation is (c) with a
`stale_requests` counter. Until ruled, behaviour is (a) and the pinning test records it.
**Amends:** the 2026-09-06 "held, not discarded" entry (two sentences of its recommendation,
as stated). **Supersedes:** none.

## 2026-09-07 — the Responder's replay entry has no age bound: after a seq wrap a retry can be answered from a stale buffer

**Context:** red team @`71caba0` finding 2 (#149). `link/responder.cpp`'s replay key is
`f.retry && buffer_.valid && f.seq == buffer_.seq && f.src == buffer_.peer`: no comparison
against the request bytes, and `buffer_.valid` is set once and never cleared. `seq` is 4 bits
(`link/frame.cpp:129`), so it wraps every 16 transactions. Reachable by a *conformant* host,
not only a forger: if this node accepts nothing for 16 of the host's transactions (a noise
burst, SUSPECT-rate polling, a deaf window), the buffer still holds `seq = 5` when the host's
counter comes round to 5 again — and if the host's first attempt at that new request is lost
and it retries, the responder replays the ancient answer. L3 then receives a stale reply: a
stale parameter value, or an ACK for a SET that was never executed. Reproducers A2 and A3 on
#149 (A3: still replayed 10 s on, 10× the §7 OFFLINE bound). CLAUDE.md rule 2 does not cover
it — idempotency makes *re-execution* safe; it does not make *not executing, and answering
from a stale buffer*, safe. trunk §7 scopes a replay to "a retry of a sequence **it already
answered**", which this is not.
**Recommendation:** invalidate the entry once the answered request's own response window has
passed (`request_end + T_resp`): a retry arriving after that is treated as new and re-invokes
the handler, which rule 2 makes safe. The alternative — keep the entry but compare the
buffered request's bytes — closes the same case without inventing a lifetime, at the cost of
storing the request alongside the response (fixed buffer, embedded-path budget to check).
Neither trunk §7 nor `data-model.md` §5 bounds a replay's lifetime, so no code was written
on #149: CLAUDE.md, "when a spec ambiguity blocks you, implement nothing speculative".
**Ruling:** PENDING — human. (Maintainer's direction 2026-09-07: record it, do not implement.)
**Amends:** none. **Supersedes:** none — distinct from the 2026-09-06 `{peer, seq}` entry,
which is about a FOREIGN `src` evicting an entry; this is the same peer served the wrong
answer, and that entry's recommended `f.src == ADDR_host` screen does not address it.

## 2026-09-07 — the Responder's late path defers for an idle bus, which FR-014 and data-model §5 do not describe

**Context:** review @`2efcb67` (#149), MEDIUM. FR-014 (`spec.md`) says a late poll "MUST still
transmit — at once", and `data-model.md` §5 says "transmitted at `now`". Since @`71caba0`'s
red-team HIGH the engine instead transmits at
`min(last_activity + T_gap, defer_origin + max_frame_us + T_gap)` — up to ~1.47 ms later —
because "at once" on a bus another station is occupying means keying down inside that
station's frame, corrupting a frame addressed to a THIRD node and driving it toward SUSPECT
under trunk §7. Both documents outrank the code (CLAUDE.md), so the divergence is recorded
rather than argued away. It is exactly the courtesy §4 already records for the Master, where
PR #137 carries the same "(Amended …)" marker and the 2026-09-05 "bounded courtesy" entry;
§5 had no such note until this entry.
**Recommendation:** read FR-014's "at once" as "on the first poll that reaches it, subject to
trunk §3's half-duplex media access", i.e. adopt the Master's bounded courtesy verbatim for
the Responder, and amend FR-014 and §5 to say so. The alternative — transmit unconditionally
at `now` — makes FR-014 override FR-017 ("never transmit outside a response window") and
trunk §3 for the one case where the engine can see the conflict, which is the reading the
red team falsified three rounds running.
**Ruling:** PENDING — human. Related and separate: the FR-014-vs-FR-017 starvation question
(2026-09-06) is about the AGE of a queued request; this is about the INSTANT a due response
may key down.
**Amends:** `specs/002-trunk-link-layer/data-model.md` §5 (marker added in PR #149).
**Supersedes:** none.

## 2026-09-07 — the Responder's acceptance screen refuses more than data-model §5 lists

**Context:** review @`2efcb67` (#149), MEDIUM. §5 lists acceptance as "intact frame,
`dst == my_addr`, `response == 0`". The engine also refuses `src == my_addr` (a station is
never its own peer), `src` outside trunk §5's `ADDR_host..ADDR_backplane_max`, and — since
red team @`71caba0` finding 3 — every request when the node's own `my_addr_` is outside that
range, so a misconfigured node never originates a non-L2 source address. Each came from a
red-team finding and each is demonstrated by a named test; none is in §5. `ReplayBuffer` in
§5 is likewise specified as `{valid, seq, len, bytes}` while the engine keys on `peer` too
(red team @`033182a` finding 3, on file 2026-09-06).
**Recommendation:** amend §5's acceptance list and `ReplayBuffer` to match, since trunk §5's
address range is the higher-authority document and the screens follow from it —
`link/master.cpp:85-88` and `link/health.cpp` already bound the same wire-derived class the
same way. No code change either way; this is a documentation debt, recorded so the next
reader of §5 is not misled.
**Ruling:** PENDING — human.
**Amends:** `specs/002-trunk-link-layer/data-model.md` §5 (marker added in PR #149).
**Supersedes:** none.

## 2026-09-07 — a Responder that stops reading to avoid dropping: latency, and the ESP32-S3 RX FIFO

**Context:** maintainer's ruling 2026-09-07 on #149, after red team @`b262d46` / @`2efcb67` /
@`7a80ec3` found the same defect three rounds running at successively relocated boundaries.
A late response must not key down on a bus the engine has not read, and the engine must not
destroy a byte it has taken off the wire. With any fixed buffer those two duties collide at
its capacity, so the engine now **decodes as it drains** and holds up to `kHeldRequests` (2)
completed requests; on the next one it **stops reading**, leaving everything behind it in the
wire's own receive queue, and reports its reading of the bus as partial — which makes the
wait fall back to the bounded cap rather than to a `last_activity` it knows is incomplete.
Nothing is dropped and nothing is destroyed; the cost is latency, and it is not small: a
backlog drains at roughly one cap (`max_frame + T_gap`, ~1.47 ms) per absorbed batch rather
than at wire speed. Measured on the suite's own cases: a 142-byte burst of back-to-back
requests, all answered, takes ~23 ms to clear; the starvation cell at `test_link_responder`
`:998` moves from 7 answers per 21 polls to 6.
**Consequence worth stating plainly:** while the engine is stopped it is not draining, and
the ESP32-S3's UART RX FIFO is 128 bytes — smaller than `kMaxWire` (142), already on file
2026-09-06. On target, a long enough stop overflows that FIFO and the hardware drops bytes
the model says are safely queued. The model's "nothing is lost" is therefore a statement
about this engine, **not** about the node it runs on.
**Recommendation:** accept the trade for now (it is the only one of the four considered that
loses no request and never transmits on a partial reading), and treat the FIFO bound as the
real limit: on target, size the poll cadence so a stop cannot outlast 128 bytes of arrivals,
or drain into a driver-level ring larger than `kMaxWire`. Both are firmware-side, outside
this PR. The alternative rulings, each rejected and why: transmit on a partial reading
(the collision this PR was reopened to fix, three times); destroy the byte at a buffer bound
(silently loses a request straddling it); discard-and-count the third request during a wait
(counted, but re-opens the defect @`e510b29`'s "eight well-spaced requests ... none
discarded" was written to close — measured at 6/8, 4/8 and 3/8 answered at 400 µs, 1 ms and
2 ms cadences).
**Ruling:** PENDING — human. The design is the maintainer's 2026-09-07 direction; what is
open is whether the latency is acceptable and how the target's FIFO bound is to be met.
**Amends:** the 2026-09-07 entry "the Responder's late path defers for an idle bus" — same
mechanism, this records what the deferral now costs. **Supersedes:** none.

## 2026-09-07 — a CRC-corrupt frame arriving with no transaction open moves no counter at all

**Context:** found while adding the CrcError row's missing "after give-up" cell to
`tests/unit/test_link_loop.cpp` (#145; review @`ef1ec22` MEDIUM). The cell was written
expecting the bad CRC to be charged to the node and measured otherwise: with no window open,
`stats(node).crc_failures` is unmoved, `stats(node).discards` is 0 and `bus_stats().bus_faults`
is 0 — the frame is invisible in `stats()` entirely. The mechanism is not a bug in this PR:
a corrupt frame never decodes, so there is no `f.src` to attribute it to, and
`link/master.cpp`'s discard accounting charges `dst_` only while awaiting a response. The
Duplicate row's late copy is counted precisely because it decodes cleanly and has a source.
**Why it may matter:** trunk §4 makes discards the bus's own health signal, and §7 escalates
on failure accounting. Corruption occurring *between* transactions — a marginal transceiver,
a babbling station whose bytes happen to form a bad frame — is exactly the condition that
signal exists for, and today it is unobservable: a rig could be visibly corrupting frames
with every counter reading zero.
**Recommendation:** count a frame-level discard with no window open against a bus-level
counter rather than a node one (`BusStats.bus_faults`, or a new `BusStats.orphan_discards`
if `bus_faults` is reserved for the §7 all-nodes-failing condition). It cannot be charged to
a node, and that is the point: it is bus-level evidence. Not implemented here — the code is
`link/master.cpp` (T031/#49, closed), outside #52's diff, and #145 has pinned today's
observable rather than changed it.
**Ruling:** PENDING — human. If adopted, it wants its own issue against T031.
**Amends:** none. **Supersedes:** none.
