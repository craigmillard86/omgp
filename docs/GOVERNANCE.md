# OMGP Governance — AI-DLC Level 3

The single reference for how this repository is governed: decision rights,
gates, risk tiering, and where each control is enforced. Detailed operating
rules live in `docs/OPERATING-POLICY.md`; this document is the map.
Amendments to either are Tier 3 changes (see §3).

## 1. Decision rights

| Decision | Who | How recorded |
|---|---|---|
| Merge to main | Human only | PR approval + `ci-gate` |
| Protocol change (YAML + docs) | Human ruling | T3 PR, CODEOWNERS review |
| Spec ambiguity resolution | Human ruling (agent may recommend) | OPEN-QUESTIONS.md entry |
| Golden-vector regeneration | Human ruling with written justification | commit message + T3 review |
| Releasing backlog to agents | Human, OR auto-release for stories whose enrichment-predicted tier <= `auto_ready_max_tier` (.github/agent-config.yml) | `ready` label (+ auto-release comment) |
| Revoking an agent claim | Any human | remove `in-progress` |
| Weakening/removing any test | Human instruction naming the test | PR review |
| New dependency / toolchain bump | Human | `needs-human` issue then T2 PR |
| Governance/policy amendment | Human | T3 PR to this doc or the policy |

Agents decide everything else within OPERATING-POLICY §2, and their
decisions are always expressed as reviewable artefacts (branches, PRs,
issues, comments) — never as direct changes to main.

## 2. Gates (all mechanical, none trust-based)

| Gate | Enforces | Mechanism |
|---|---|---|
| `ci-gate` required check | evidence before merge | branch protection |
| One human review | judgement before merge | branch protection |
| CODEOWNERS review on protected paths | human ruling on ground truth | branch protection ("require review from Code Owners") |
| Codegen drift guard | YAML/code/docs coherence | CI fails on divergence |
| Differential check | independent verification of codecs | CI (C++ vs Python reference) |
| Immutable golden vectors | tests can't be quietly bent | CLAUDE.md rule 9 + T3 scoring on `tests/vectors/` |
| Sanitizers + cross-compile | memory safety + portability | CI jobs |
| Format + static analysis (quality stage) | code quality on every merge | pipeline stage in CI |
| CodeQL (C++/Python/Actions) + dependency review | security on every PR + weekly | security workflow |
| Deep-verify: focused fuzz + diff-scoped mutation | pre-merge deep testing on T2/T3; fails on any fuzz finding or on mutation survivors above the `tools/mutate.cfg` threshold (stubs exit 0 until feature 001 lands the real harnesses) | conditional CI job in ci-gate |
| Claude review on T2/T3 | spec-conformance + security review pass | claude-review workflow (advisory) |
| Red team: PR attack on T2/T3 + monthly hostile-module protocol attack | falsification with runnable reproducers | red-team workflow (advisory; findings need evidence) |
| WIP cap = 1 | review capacity governs autonomy | dispatch workflow |
| `ready`-only pull | humans release all autonomous work | dispatch workflow |
| Tool allow-lists + minimal permissions | agent blast radius | workflow definitions |
| Timeouts + claim release | no runaway/stuck autonomy | workflow definitions |

## 3. Risk tiers

Applied automatically to every PR by the `risk-score` workflow
(label `risk:t0`–`risk:t3`) from touched paths and change shape.

| Tier | Definition | Review expectation |
|---|---|---|
| T0 | docs, tests, scenarios only | light review; verify tests test the right thing |
| T1 | host-only code (sim/, cli/, transport/, tools/) | normal review |
| T2 | portable protocol-critical code (core/, link/), pipeline/toolchain definition, any new dependency, or >800 lines | careful review; check against spec sections cited |
| T3 | ground-truth artefacts (protocol YAML, vectors, spec docs), governance (policy, workflows, CLAUDE.md, constitution), or any reduction of test content | CODEOWNERS-gated; agent-authored T3 is a policy breach signal — it should have escalated `needs-human` instead |

Any T3 signal outranks lower signals (weakest-dimension floor rule applied
to change risk). Tier is advisory for review depth; the CODEOWNERS gate is
the enforcement.

## 4. Autonomy governance

- Humans release work (`ready`), agents claim it visibly, one item in
  flight, every claim revocable. Escalation (`needs-human`) is always
  cheaper for an agent than guessing — the policy is written so the safe
  action is also the easy one.
- Kill switches, fastest first: remove `ready` labels; disable
  `agent-dispatch` workflow; revoke `CLAUDE_CODE_OAUTH_TOKEN` secret;
  uninstall the Claude GitHub App.
- Standing loops (nightly, triage, audit, dispatch, metrics) are enumerated
  in OPERATING-POLICY §4 with their human touchpoints.

## 4b. Public-repo posture

This repository is public. Consequences enforced here: agent workflows
that execute code with secrets never run for fork PRs (same-repo guard);
@claude mention responses are restricted to users with write access (the
action's default — keep it); risk labelling of fork PRs uses
pull_request_target WITHOUT any code checkout; no third-party trademarks
in module names or examples; LICENSE (Apache-2.0) present from first
commit — a public repo without one forbids the contributions it invites.

## 5. Audit trail

Every consequential event leaves a queryable artefact: claims and releases
(issue comments linking run IDs), escalations (`needs-human` +
analysis comment), rulings (OPEN-QUESTIONS.md, commit messages on T3),
delivery evidence (CI runs per PR), and outcomes
(`metrics/delivery-log.jsonl`, `metrics/task-log.jsonl`). Nothing the
system does is reconstructible only from memory or chat history.

## 6. Review cadence

Monthly governance review: scorecard from `tools/metrics-report.py`,
auto-release prediction accuracy (auto-released stories whose actual PR
scored above the predicted tier, or that escalated/reworked — sustained
misses mean lowering `auto_ready_max_tier`),
escalation rate (`needs-human` per agent task), any agent-authored T3
occurrences (target: zero), converge-audit findings and dispositions, red-team findings and their
dispositions (every finding gets one: fixed, spec-amended, or accepted
risk recorded in the issue), and
whether autonomy should widen (WIP cap, batch `ready` releases) or narrow.
The metrics inform the throttle; they never justify weakening a gate.

## 6a. Incidents

A defect merged to main is an incident: revert-first via PR, `incident`
issue naming the gate that should have caught it, regression test in the
fix, and — if a gate failed — the gate repair is in-scope for the incident.
Operational detail: docs/RUNBOOK.md.

## 7. Setup obligations (governance is live only when these are on)

1. Branch protection on `main`: require `ci-gate`, require 1 review,
   require review from Code Owners, no force pushes, admins included.
2. `.github/CODEOWNERS`: replace `@OWNER` with the maintainer's handle.
3. Run `tools/gh-setup.sh` (labels including `risk:*`, project board).
4. Secrets: `CLAUDE_CODE_OAUTH_TOKEN`. App: Claude GitHub App on this repo.
