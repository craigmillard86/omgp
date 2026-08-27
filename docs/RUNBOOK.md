# OMGP Runbook — activation, operations, incidents

## Activation checklist (one-time, in order)
1. Push repo; `specify init` with Claude Code integration; run the
   constitution prompt from speckit-prompts.md.
2. Replace `@OWNER` in .github/CODEOWNERS.
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
