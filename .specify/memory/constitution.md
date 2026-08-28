<!--
Sync Impact Report
- Version change: (none — unratified template) → 1.0.0
- Rationale: initial ratification. The prior file on disk was the unfilled
  Spec Kit scaffold (all placeholder tokens, no adopted content), so this
  is treated as first adoption, not an amendment.
- Modified principles: n/a (initial)
- Added sections: Core Principles I–IX; Verification & Enforcement;
  Development Workflow; Governance
- Removed sections: none
- Templates requiring follow-up: none modified by this command (out of
  scope per the constitution workflow's scope guard); plan/spec/tasks
  templates consume this constitution at runtime and were not edited.
- Re-review against docs/omgp-spec-v0.7.md, docs/protocol-l3.md and
  docs/trunk-link-layer.md (2026-08-28, after the spec was added to the
  working tree). Version kept at 1.0.0: the first draft was never
  committed or ratified, so this is a revision of the initial adoption,
  not an amendment.
  - RESOLVED(EMBEDDED_TARGET): the user-supplied Principle IV text named
    "arm-none-eabi cross-compile". The spec names ESP32-S3 as the
    reference host (Document Control table, "Reference host: ESP32-S3";
    §29 "Reference Host MCU"; §46 "Real ESP32-S3 Host"), matching
    CLAUDE.md rule 10 and CI. The string
    "arm-none-eabi" appears nowhere in any authoritative document.
    Principle IV therefore uses ESP32-S3 via the pinned ESP-IDF toolchain.
  - RESOLVED(SPEC_SECTION_44): Spec §44 is "Virtual Fault Injection" and
    the citation stands. However, the first draft's illustrative list
    (timeout, garbage, CRC error, silence, babble) was mis-attributed to
    §44 — those are trunk-level (L2) failure modes from CLAUDE.md, whereas
    §44 enumerates system-level faults (B+ overcurrent, heater faults,
    overtemperature, trunk timeout / I2C lockup / bridge failure, slot
    removal, invalid descriptor, power-budget exceeded, channel-switch
    timeout, stuck relay, firmware/protocol mismatch). Principle VI now
    cites §44 for its actual contents and trunk-link-layer.md §7 for the
    L2 error/retry/node-health modes separately.
  - Citations added: Principle V → protocol-l3.md §3 ("SET operations are
    absolute, never relative"); Principle VII → trunk-link-layer.md §8
    (bridging rules) and §9 (T_resp = 200 µs), protocol-l3.md §3.1 (error
    0x04 busy/settling); Principle VI → Spec §41–§42 (transport
    abstraction, VirtualTransport).
- Flagged for human ruling (not changed — user-stated principle kept):
  - LICENCE (needs-human): Principle VIII and docs/GOVERNANCE.md §4b
    state Apache-2.0, and §4b asserts "LICENSE (Apache-2.0) present from
    first commit". Neither is currently true on disk. Spec §50 "Licensing
    Direction" says licensing "remains to be formally selected" (Apache-2.0
    or MIT plausible for software). The committed LICENSE file (101 bytes
    at HEAD; 0 bytes in the first commit) does not contain licence text at
    all — it contains a sandbox network-egress error message that was
    evidently captured when a fetch of the Apache-2.0 text failed, and
    committed as if it were the licence. For a public repository this
    means no licence is actually granted. Principle VIII keeps Apache-2.0
    as the user-stated intent; the ruling and the LICENSE file repair are
    human actions outside this workflow's scope (see Next Actions in the
    command summary).
-->

# OMGP Constitution

## Core Principles

### I. Spec-First Authority
`docs/protocol-l3.md`, `docs/trunk-link-layer.md`, and
`protocol/omgp-protocol.yaml` are authoritative for protocol behaviour.
Code MUST conform to them. Any discrepancy between code and these
documents MUST be raised — as an issue or a `docs/OPEN-QUESTIONS.md`
entry — and MUST NOT be silently resolved by picking an interpretation in
code. Protocol constants, opcodes, TLV types, and timing symbols MUST
exist in code only via generation from `protocol/omgp-protocol.yaml`;
generated headers are never hand-edited.

Rationale: hardware does not exist yet to physically expose a drifted
protocol. A single machine-readable source of truth, enforced by codegen
rather than convention, is the only thing standing between the spec and
an incompatible implementation.

### II. Test-First Development (NON-NEGOTIABLE)
Every new behaviour lands as failing tests before implementation — unit
tests, and a scenario file where the behaviour is observable at rig
level. Every bug fix MUST land with a regression scenario that reproduces
it, committed before the fix. Golden byte vectors under `tests/vectors/`
are immutable evidence and MUST NOT be edited to make a test pass; a
genuinely wrong vector is regenerated from the Python reference
implementation with the reason recorded in the commit message.

### III. Dual, Independent Verification
Every protocol codec has an independent Python reference implementation.
Differential tests MUST show the C++ and Python implementations agree on
generated corpora before a codec change merges. Parsers MUST be
fuzz-tested, and MUST NOT crash, hang, or over-read on arbitrary byte
input.

Rationale: two independent implementations catch specification
misreadings that a single implementation's own tests cannot; fuzzing
accepts that, on an open hardware platform, module firmware may be
malicious or simply broken (see Principles VI and VII).

### IV. Portability and Determinism
`core/` and `link/` remain portable, OS-independent C++17: no wall-clock
reads (`std::chrono::system_clock::now()`, `sleep()`, or any other
wall-clock access is prohibited — all time flows through the injected
`Clock` interface), no dynamic allocation after init, no exceptions, no
RTTI. Every merge MUST pass both the native build with
AddressSanitizer/UndefinedBehaviorSanitizer and the ESP32-S3 firmware
build (Xtensa LX7, via the pinned ESP-IDF toolchain; ESP32-S3 is the
reference host per Spec §29 and §46) — neither may be merged red.

### V. Idempotency Invariant (NON-NEGOTIABLE)
Every L3 operation is safe to retry. SET operations carry absolute
values, never deltas (`docs/protocol-l3.md` §3), so that retransmission
at L2 (`docs/trunk-link-layer.md` §7: same sequence, `ctrl.retry` set)
can never double-apply an effect. A design that cannot preserve this
property MUST be rejected in review — it is not merged with a caveat, and
it is not an acceptable tradeoff for convenience or performance.

### VI. Simulator as a First-Class Product
The real host-core runs against virtual backplanes and virtual modules
over `VirtualTransport` (Spec §41–§42: the host-core MUST NOT know
whether messages travel over the real trunk, a virtual bus, or a
development transport) — the simulator is not a mock of the host-core,
it is the host-core under test. Scenarios are data (YAML), executed by a
table-driven runner, never bespoke code per scenario. Every fault mode
enumerated in Spec §44 "Virtual Fault Injection" (B+ overcurrent, heater
faults, overtemperature, trunk timeout / I2C lockup / bridge failure,
slot removal, invalid or incompatible descriptor, power-budget exceeded,
channel-switch timeout, stuck relay, firmware/protocol mismatch) and
every trunk error/retry/node-health mode in `docs/trunk-link-layer.md`
§7 (response timeout, CRC-failed response, SUSPECT, OFFLINE, BUS_FAULT)
MUST be expressible as scenario configuration, not a special-cased code
path.

### VII. Bridge Discipline (NON-NEGOTIABLE)
Backplane bridging never stalls the trunk (`docs/trunk-link-layer.md`
§8). Any code modelling or implementing bridging MUST respond within
`T_resp` (200 µs, `docs/trunk-link-layer.md` §9) or return `ERR_BUSY`
(error `0x04` busy/settling, `docs/protocol-l3.md` §3.1) rather than
block waiting on the module bus. This is enforced by a permanent scenario
test that MUST NOT be weakened, narrowed, or removed without an explicit
human instruction naming it.

### VIII. Minimal, Vetted Dependencies
Dependencies are the standard library plus an explicit allow-list of
third-party libraries (for example: a test framework, or YAML/JSON
parsing confined to host-only code). Nothing networked by default, and
nothing whose licence is incompatible with this project's Apache-2.0
licence. Introducing a new dependency is a human decision — an agent MUST
NOT add one unilaterally (see `docs/OPERATING-POLICY.md` §2).

### IX. Traceability
Code comments cite the spec section they implement. Scenario files name
the requirement they verify. Commit messages reference the feature spec
or issue they address. A reviewer MUST be able to walk from any line of
protocol-critical code back to the spec text that justifies it.

## Verification & Enforcement

Compliance with these principles is mechanically gated, not trust-based.
`./pipeline.sh` (`codegen build unit refimpl diffcheck scenarios esp32`)
is the single build/test definition run identically by CI, developers,
and agents. The native build carries ASan/UBSan (Principle IV);
`tools/diffcheck.py` enforces the C++/Python agreement required by
Principle III; a permanent bridge-busy scenario enforces Principle VII;
`tools/codegen.py` is the only path by which protocol constants reach
code, enforcing Principle I. The full gate inventory, risk tiers, and
required checks are defined in `docs/GOVERNANCE.md` §2–3, which this
constitution does not duplicate.

## Development Workflow

Work is tracked as GitHub issues labelled `task` + `feature:<id>`; a task
is claimed by applying `in-progress` and closed only via the merging PR
(`Closes #n`). Spec ambiguities are never resolved silently in code:
record the question and a recommended default in
`docs/OPEN-QUESTIONS.md` (append-only and dated; a decision is changed by
appending a superseding entry, never by editing history), and proceed
only when a safe default exists. Agents MUST NOT edit
`protocol/omgp-protocol.yaml`, `tests/vectors/`, the three documents in
`docs/` named in Principle I, or this constitution — these are
human-ruling artefacts (`docs/OPERATING-POLICY.md` §2). Full workflow,
autonomy limits, and escalation paths are defined in
`docs/OPERATING-POLICY.md`, which this constitution does not duplicate.

## Governance

This constitution is the highest-level statement of non-negotiable
engineering principles for OMGP. `docs/protocol-l3.md`,
`docs/trunk-link-layer.md`, and `protocol/omgp-protocol.yaml` remain the
authoritative technical source of truth for protocol details (Principle
I); `CLAUDE.md`, `docs/GOVERNANCE.md`, and `docs/OPERATING-POLICY.md`
operationalize these principles into concrete rules, gates, and agent
permissions. Where an operational document appears to conflict with a
principle stated here, the conflict MUST be raised — as an issue or a
`docs/OPEN-QUESTIONS.md` entry — and MUST NOT be silently resolved by
either document choosing its own reading.

Amendments to this constitution are a governance change: human-ruled,
proposed via pull request, scored risk T3 under `docs/GOVERNANCE.md` §3,
and requiring CODEOWNERS review. An agent MUST NOT merge a change to this
file; it may only propose one for human review. Versioning follows
semantic rules: MAJOR for a backward-incompatible principle removal or
redefinition; MINOR for a new principle added or existing guidance
materially expanded; PATCH for clarification, wording, or typo fixes with
no semantic change. Compliance with these principles is reviewed at every
pull request through the gates in `docs/GOVERNANCE.md` §2 (CI, Claude
review and red team on T2/T3) and monthly through the governance review
cadence in `docs/GOVERNANCE.md` §6.

**Version**: 1.0.0 | **Ratified**: 2026-08-28 | **Last Amended**: 2026-08-28
