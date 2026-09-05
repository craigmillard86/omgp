# OMGP Governance — AI-DLC Level 3

The single reference for how this repository is governed: decision rights,
gates, risk tiering, and where each control is enforced. Detailed operating
rules live in `docs/OPERATING-POLICY.md`; this document is the map.
Amendments to either are Tier 3 changes (see §3).

## 1. Decision rights

| Decision | Who | How recorded |
|---|---|---|
| Merge to main | Autonomous for `agent-authored` PRs at or below `auto_merge_max_tier` (T2; ruling 2026-09-02): approval comes from a clean machine-readable verdict at the exact head (`auto_approve_max_tier`, ruling 2026-08-31) and the merge itself is made by `agent-merge` when every check is green at that same head. A human clicks merge for everything else: T3 always, any CODEOWNERS-owned path except `docs/OPEN-QUESTIONS.md` and `specs/**/tasks.md`, any human-authored PR, and anything labelled `needs-human` | PR approval + `ci-gate`; agent-approve then agent-merge workflow |
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
| Autonomous merge ≤ T2 | merge only when the tier, the verdicts, every check and the changed paths all clear at ONE head; T3 and human-owned paths never | agent-merge workflow — the merge API call is pinned to that head's sha, so a push landing mid-run makes GitHub refuse (409) rather than merge an unreviewed head; `auto_merge_max_tier: -1` returns the click to a human |
| Verdict-gated auto-approval ≤ T2 | approval only on a clean review (and, at T2, red-team) verdict for the exact head; stale bot approvals self-dismissed; fail-closed on any unresolved input | agent-approve workflow — `issue_comment` trigger, so the DEFAULT-BRANCH definition and inputs run and a PR cannot rewrite the gate in its own diff (ruling 2026-08-31; hardened per Copilot review on #103) |
| Red team: PR attack on T2/T3 + monthly hostile-module protocol attack | falsification with runnable reproducers | red-team workflow (advisory; findings need evidence) |
| WIP cap (`wip_cap` in .github/agent-config.yml, currently 2; stories in flight = open agent PRs ∪ claimed tasks, deduped per story; ruling 2026-09-03) | review capacity governs autonomy; `wip_cap: 1` restores single-slot | dispatch workflow |
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
  - **Bound — `auto_fix_max_attempts` (agent-config; 4 since ruling
    2026-09-03, was 2), then a human.** Attempts are the `auto-fix-<n>` PR
    labels, one attempt per commit (written to the first free index, so the
    bound stays reachable after a human removes a label); a failure after
    the last releases `in-progress` and applies `needs-human` with the
    branch's non-successful runs listed — capped at 10 links from the
    newest 100 runs, every cut disclosed explicitly (never presented as
    complete when it is not). The labels are the bound: nothing resets
    them but a human. Knob semantics: 0 (or < 1) disables auto-fix —
    INCLUDING escalation and claim release: a disabled router takes no
    action at all, the PR keeps `in-progress`, and the disable window is
    operator-owned (a distinct disabled marker keeps the sweep quiet;
    restoring the knob re-routes on the next sweep). An unreadable value
    (any non-digit suffix: `1e3`, `0x10`, `4.9`) fails closed to 2; an
    inline comment or quotes around the value are tolerated; values above
    10 are clamped. The sweep carries NO bound: it re-delivers at every
    attempt count, since exhaustion/escalation needs the delivery
    backstop most (red-team on #120, all rounds).
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

- Review-finding auto-resolution (`review-fix` workflow, ruled 2026-09-02).
  `claude-review` and `red-team` are read-only, and the only consumer of
  their verdict (`agent-approve`) merely withholds approval — so an
  agent PR with green CI and open findings had no agent able to act on
  it and stalled until a human intervened. This loop closes that edge:
  - **Trigger and scope.** A `claude[bot]` verdict comment of the form
    `VERDICT(review|red-team): findings @ <current head>` on an open
    `agent-authored` `task/*` PR. The trigger is `issue_comment`, so the
    loop runs from the DEFAULT-branch definition and a PR cannot rewrite
    its own bounds. Fork heads, human PRs and `needs-human` PRs are never
    touched.
  - **Scope policy (ruled 2026-09-05, #134; supersedes the 2026-09-02
    severity policy).** Findings are routed by SCOPE, not severity.
    *Blocking* findings — a defect in the changed code, a security hole, a
    weakened/narrowed test, a spec divergence, an unmet acceptance
    criterion of the linked issue, or a false claim the PR makes — are
    fixed here, and a fix may SHRINK the diff (drop an out-of-scope claim
    or its machinery), not only add code. *Follow-up* findings — real
    improvements or pre-existing gaps OUTSIDE the linked issue's
    acceptance criteria — are NOT fixed here; the reviewer proposes each
    as an issue and the loop leaves the code. `claude-review`'s verdict is
    `clean` iff there are no blocking findings, so a PR whose only findings
    are follow-ups is auto-approvable below `auto_approve_max_tier` (the
    merge stays human for T3 and CODEOWNERS paths per §1). Never
    deferrable: defects, security, weakened tests, spec divergence and
    unmet criteria always block, at every severity. The rule rests on the
    linked issue carrying precise, testable acceptance criteria — weak
    criteria make "in scope" weak. (`red-team` still emits `findings` for
    out-of-scope weaknesses today; the loop triages those as follow-ups,
    and aligning red-team's own verdict is a tracked fast-follow.)
  - **Bound — `review_fix_max_attempts` attempts, then a human.** Labels
    `review-fix-1..N`, at most one attempt per head commit (a red-team
    verdict arriving after a review verdict on the same commit is the same
    attempt). The bound is 4 (raised from 2, ruling 2026-09-03: PR #116
    spent both attempts while still fixing real findings each round). It
    lives in `.github/agent-config.yml`, not in the workflow, so retuning
    it needs no workflow-scope push — the fixer agent cannot edit
    `.github/workflows/*` itself. `0` disables the loop. Exhaustion
    releases `in-progress` and applies `needs-human`, exactly as the CI
    router does. Nothing resets the labels but a human.
  - **Never.** No approval, no merge, no weakened test or gate, no edit to
    `.github/workflows/` — the loop may not touch its own bounds.
  - **Kill switch:** disable the `review-fix` workflow. The general
    switches above also stop it.
- Autonomous merge (`agent-merge` workflow, ruled 2026-09-02). With the
  review, fix and approval loops closed, the merge click was the last
  human step in the cycle; it is now made by the agent for work that is
  complete, clean and low-risk:
  - **Every condition is evaluated at ONE head.** Tier label plus a
    completed `score` check, clean `VERDICT(review)` (and `VERDICT(red-team)`
    at T2+), every check run finished green, no failing commit status, and
    the PR mergeable. Any one unresolved refuses the merge — this gate
    never merges more because it read less.
  - **The merge is pinned to that head.** `sha` is passed to the merge
    API, so a push landing between the checks and the call makes GitHub
    refuse (409) instead of merging a head nobody reviewed. This is what
    makes autonomous merge safe while branch protection has
    `dismiss_stale_reviews` off.
  - **Never merged by an agent:** T3 (any tier above
    `auto_merge_max_tier`), human-authored PRs, forks, drafts, `needs-human`
    or `blocked`, and any CODEOWNERS-owned path other than
    `docs/OPEN-QUESTIONS.md` and `specs/**/tasks.md` — the two OPERATING-POLICY
    §2 already sanctions agents to write. Ground truth and governance keep
    their owner regardless of the tier the diff happens to score.
  - **The claim is released by the merger, not by the PR's prose.** After a
    successful merge, `agent-merge` removes `in-progress` from and closes
    the issues the PR closes (`Closes/Fixes/Resolves #n`) plus the branch's
    own `task/<n>`. GitHub's auto-close only honours a reference directly
    after the keyword, and on #114 a body reading "Closes T021 (issue #39)"
    closed nothing: the issue kept `in-progress`, held the WIP cap, and
    dispatch pulled no work for ~10 hours (2026-09-02). The loop must not
    depend on an agent writing the right sentence.
  - **Trigger.** The verdict comment, plus a 20-minute sweep, because the
    moment a PR becomes merge-ready is usually the last check going green
    rather than any event this workflow can subscribe to.
  - **Kill switch:** `auto_merge_max_tier: -1`, or disable the
    `agent-merge` workflow. Note that revoking the Claude token or
    uninstalling the App does NOT stop this one: it merges with
    `GITHUB_TOKEN` and runs no agent.
  - **Not yet in the OPERATING-POLICY §4 table** — same as the CI router
    above: a human needs to add its row, as agents must not edit that
    document.
- Weekly governance feedback (`continuous-improvement` workflow, ruled 2026-09-04).
  Mondays 07:00 UTC, Claude reads the last 14 days of PR review feedback, groups it
  into patterns, and proposes — never applies — changes to `CLAUDE.md`,
  `.specify/memory/constitution.md` or this document.
  - **Read-only by construction.** The job holds `contents: read`, so nothing the
    agent writes to its ephemeral workspace can reach the repository — a proposal
    cannot become a change. The agent does hold a `Write` tool (for the two output
    files below); that is a workspace edit, not a repository one, and both
    governance files stay CODEOWNERS-owned, with §3 making an agent-authored T3
    change a breach signal regardless.
  - **The issue is filed by the workflow, not the agent**, with the labels fixed
    in YAML (`converge-audit` + `needs-human`). The agent emits the title and body
    as data in `continuous-improvement.json`. This is deliberate: the loop ingests
    a public repository's PR stream, so an unconstrained `--label` would have put
    attacker-authorable text one step from `ready`, the label by which humans
    release autonomous work (§2).
  - **Untrusted input.** PR and comment text is data, never instructions (prompt
    binding 7); apparent steering is an OPERATING-POLICY §7 escalation and a
    reportable finding, not something to act on.
  - **"No update required" is a success**, and a run producing no output files is
    a failure — the two must never share one observable outcome.
  - **Kill switch:** disable the `continuous-improvement` workflow. As with
    `agent-merge`, revoking the Claude token stops the analysis but the workflow
    itself runs with `GITHUB_TOKEN`.
  - **Not yet in the OPERATING-POLICY §4 table** — same as the loops above; a
    human must add its row.
- Both fix loops push with `persist-credentials: false` so the agent's
  commit carries the Claude App token, not `GITHUB_TOKEN`: pushes made
  with the latter suppress the follow-on `workflow_run`/re-review delivery,
  which is how the CI router lost its own attempts' failures
  (docs/OPEN-QUESTIONS.md 2026-08-31). The router additionally runs a
  2-hourly `sweep` that re-dispatches failures no router run ever saw.

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
