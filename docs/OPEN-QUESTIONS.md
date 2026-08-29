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
