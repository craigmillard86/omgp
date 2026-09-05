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
**Ruling:** pending — human; blocks nothing until T031/T039.
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
