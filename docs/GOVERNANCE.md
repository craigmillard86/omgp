# OMGP Governance — AI-DLC Level 3

The single reference for how this repository is governed: decision rights,
gates, risk tiering, and where each control is enforced. Detailed operating
rules live in `docs/OPERATING-POLICY.md`; this document is the map.
Amendments to either are Tier 3 changes (see §3).

## 1. Decision rights

| Decision | Who | How recorded |
|---|---|---|
| Merge to main | Human clicks merge. The required PR approval may be satisfied by a clean machine-readable agent verdict for `agent-authored` PRs at or below `auto_approve_max_tier` (T2; ruling 2026-08-31) — CODEOWNERS paths always need the owner, T3 always needs a human review | PR approval + `ci-gate`; agent-approve workflow |
| Protocol change (YAML + docs) | Human ruling | T3 PR, CODEOWNERS review |
| Spec ambiguity resolution | Human ruling (agent may recommend) | OPEN-QUESTIONS.md entry |
| Golden-vector regeneration | Human ruling with written justification | commit message + T3 review |
| Releasing backlog to agents | Human, OR auto-release for stories whose enrichment-predicted tier <= `auto_ready_max_tier` (.github/agent-config.yml) | `ready` label (dispatch now; dependencies closed) or `queued` label (batch release; `promote-queued` swaps it to `ready` when dependencies close) — both pass `ready-gate`; (+ auto-release comment) |
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
| CodeQL (C++/Python/Actions) + dependency review | security on every PR + weekly | security workflow; the `CodeQL` results check, `codeql` and `dependency-review` are required status checks on `main` (ruled 2026-08-28) |
| Deep-verify: focused fuzz + diff-scoped mutation | pre-merge deep testing on T2/T3; fails on any fuzz finding or on any surviving mutant on a changed line that is neither killed by a test nor labelled `// mutant-ok(equivalent\|accepted): <why>` on its source line (triage gate, ruled 2026-08-29; the whole-tree kill rate is a nightly trend, never a gate). `tools/mutate.cfg [policy]` constants are T3 — never relaxed to get green | conditional CI job in ci-gate |
| Claude review on every agent PR | spec-conformance + security review pass, per pushed head | claude-review workflow (advisory findings; machine-readable verdict) |
| Verdict-gated auto-approval ≤ T2 | approval only on a clean review (and, at T2, red-team) verdict for the exact head; stale bot approvals self-dismissed; fail-closed on any unresolved input | agent-approve workflow — `issue_comment` trigger, so the DEFAULT-BRANCH definition and inputs run and a PR cannot rewrite the gate in its own diff (ruling 2026-08-31; hardened per Copilot review on #103) |
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
- CI-failure auto-resolution (`ci-failure-router` workflow, ruled
  2026-08-30). When a `ci` or `security` run fails, the router acts by the
  failed run's branch and nothing else:
  - **Scope — agent branches only.** A `task/*` branch with an open
    `agent-authored` PR gets an auto-fix: a Claude Code run that reads the
    failed logs, fixes on that branch only (TDD where a test was missing,
    pipeline green before pushing) and comments root cause + evidence; an
    environmental failure is re-run, not patched.
  - **Bound — two attempts, then a human.** Attempts are the PR labels
    `auto-fix-1` and `auto-fix-2`, one attempt per commit; a failure after
    the second releases `in-progress` and applies `needs-human` with the
    failed-run links. The labels are the bound: nothing resets them but a
    human.
  - **`main` goes to triage.** A failure on `main` files one open
    `ci-failure` + `task` issue per workflow, which `agent-triage` handles
    exactly like `nightly-failure`.
  - **Human branches are never touched.** At most one comment per commit
    naming the first failed step; no push, no label. Fork runs get nothing.
  - **Kill switch:** disable the `ci-failure-router` workflow. The general
    switches above (revoke the token, uninstall the App) also stop it.
  - **Not yet in the OPERATING-POLICY §4 table.** This loop is ruled and
    implemented here, but OPERATING-POLICY.md is a human-ruling artefact
    agents must not edit (OPERATING-POLICY §2); a human needs to add its
    row so "standing loops are enumerated in OPERATING-POLICY §4" above is
    true of all of them, not just the five named there (review on #96).

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

1. Branch protection on `main`: require `ci-gate`, `CodeQL`, `codeql` and
   `dependency-review` (branch up to date), require 1 review, require review
   from Code Owners, no force pushes, admins included. (As of 2026-08-28
   `enforce_admins` is off, which is what lets the sole maintainer merge past
   the self-review rule with `--admin`; turning it on requires a second
   reviewer.)
2. `.github/CODEOWNERS`: replace `@OWNER` with the maintainer's handle.
3. Run `tools/gh-setup.sh` (labels including `risk:*`, project board).
4. Secrets: `CLAUDE_CODE_OAUTH_TOKEN`. App: Claude GitHub App on this repo.
