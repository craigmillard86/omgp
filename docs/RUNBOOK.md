# OMGP Runbook — activation, operations, incidents

## Activation checklist (one-time, in order)
1. Push repo; `specify init` with Claude Code integration; run the
   constitution prompt from speckit-prompts.md.
2. CODEOWNERS: already set to @craigmillard86.
3. Branch protection on main: require `ci-gate`, 1 review, Code Owners
   review, no force pushes, include admins.
4. Repo settings: enable secret scanning + push protection; enable
   Dependabot alerts.
5. Secrets: `CLAUDE_CODE_OAUTH_TOKEN` (`claude setup-token`). Install the
   Claude GitHub App on this repo.
6. `OWNER=<you> tools/gh-setup.sh` (labels, project board); enable the
   board's auto-add/auto-done workflows in the UI.
7. Scaffold `esp32-host/` (minimal IDF hello-world) so the esp32 CI job is
   truthfully green.
8. Verify: `./pipeline.sh` green locally; open a trivial PR and watch
   risk-score, ci-gate and (if labelled) claude-review fire.

## Activation order for autonomy (deliberately staged)
- Week 1 (F1): interactive speckit only. No `ready` labels. Calibrate.
- Week 2+: enrichment on (`enrich` labels), auto-release at
  auto_ready_max_tier: 0, dispatch enabled. Review every agent PR closely.
- When telemetry shows first-pass-green and prediction accuracy you trust:
  raise auto_ready_max_tier to 1; consider batching `ready`.
- Deep-verify and red-team bite from F1/F4 delivery onward; nightly loops
  are live throughout.

## Routine operations
- Daily: glance at board + open agent PRs (WIP cap means at most one).
- Weekly: converge-audit + red-team + nightly issues — disposition each.
- Monthly: GOVERNANCE §6 review with `tools/metrics-report.py`.
- Metrics ledger: `delivery-metrics` and `task-metrics` append to
  `metrics/*.jsonl` on the unprotected `metrics` branch (append-only;
  `tools/metrics-ledger.sh`), because branch protection rejects a bot push
  to `main`. A rejected push fails those runs loudly — if the report says
  "no `metrics` branch yet" after PRs have merged, look at their runs, not
  at the branch. Merge `metrics` into `main` only if a mainline history of
  the ledger is wanted; nothing reads it from `main`.

## Incident response (bad merge on main)
1. Revert first, diagnose second: `git revert` via PR (goes through
   ci-gate like everything else; a revert of an agent PR is labelled
   human-authored).
2. File an issue labelled `incident`: what shipped, which gate should have
   caught it, and whether any gate was weakened to let it through.
3. The fix PR must include the regression test/scenario that would have
   blocked the original (CLAUDE.md rule 8 applies with teeth here).
4. Gate postmortem: if a gate SHOULD have caught it, fixing the gate is
   part of the incident, not a follow-up.

## Escalation quick reference
- Runaway/wrong agent behaviour: remove `in-progress` (revokes claim) →
  disable the workflow → revoke CLAUDE_CODE_OAUTH_TOKEN.
- Suspected prompt injection via issue/PR content: policy §7 — stop, file
  `security` + `needs-human`, don't quote the payload.
- Trunk-of-truth doubt (vectors/YAML suspected wrong): freeze T3 merges,
  re-derive from the Python reference + published check values, document
  the ruling in OPEN-QUESTIONS.md.
- CI auto-fix misbehaving (the router's agent pushing bad fixes to its own
  branch): disable the `ci-failure-router` workflow. The loop is bounded to
  two attempts per PR regardless (`auto-fix-1`/`auto-fix-2`); remove those
  labels only to grant a fresh pair of attempts. A router `needs-human`
  means both attempts failed — its comment links every failed run.
- Stop autonomous merging (a bad change reached main, or you want the
  click back): set `auto_merge_max_tier: -1` in `.github/agent-config.yml`,
  or disable the `agent-merge` workflow. NOTE the general agent kill
  switches do NOT cover this one — revoking `CLAUDE_CODE_OAUTH_TOKEN` or
  uninstalling the App stops the agents that produce PRs, but `agent-merge`
  runs no agent and merges with `GITHUB_TOKEN`, so already-clean PRs would
  keep merging.
- Review findings not being fixed (the `review-fix` agent pushing bad
  fixes, or looping): disable the `review-fix` workflow, or set
  `review_fix_max_attempts: 0`. It is bounded to `review_fix_max_attempts`
  attempts per PR (4; labels `review-fix-1..4`), one per head commit;
  remove those labels only to grant fresh attempts. Raising the knob also
  frees an already-escalated PR: remove `needs-human` and the spent labels
  below the new bound stay, so it resumes at the next attempt number.
  Findings route by SCOPE, not severity (#134): BLOCKING findings
  (defects in the change, security, weakened tests, spec divergence,
  unmet acceptance criteria) are fixed and keep the verdict at
  `findings`; FOLLOW-UP findings (real improvements or pre-existing gaps
  OUTSIDE the linked issue's acceptance criteria) are proposed as issues,
  not fixed, and do NOT keep the verdict at `findings`. A PR whose only
  findings are FOLLOW-UPs reads `clean` and is auto-approvable below
  `auto_merge_max_tier`; T3 / CODEOWNERS PRs are still yours to merge.
- `ci-failure` issue on `main`: agent-triage takes it like
  `nightly-failure`. If the cause was environmental, close the issue with
  a note; the router files at most one open issue per workflow, so a
  closed one lets the next failure file again.
