#!/usr/bin/env bash
# One-time repo setup: label taxonomy + project board. Requires gh auth.
set -euo pipefail
L() { gh label create "$1" --color "$2" --description "$3" --force; }
# authorship (telemetry depends on these)
L agent-authored  "1D76DB" "Change authored by an AI agent"
L human-authored  "0E8A16" "Change authored by a human"
# workflow states
L task            "C2E0C6" "Work item from a Spec Kit tasks.md"
L in-progress     "FBCA04" "Actively being worked (timing marker)"
L blocked         "D93F0B" "Waiting on something"
L ready           "0E8A16" "Released for autonomous agent pickup"
L queued          "0E8A16" "Released pending dependencies; auto-promotes to ready when they close"
L enrich          "D4C5F9" "Queue for agent PRD population"
L enriched        "D4C5F9" "PRD populated by agent; review or auto-released"
L needs-human     "B60205" "Requires a human ruling per OPERATING-POLICY"
# origins
L nightly-failure "5319E7" "Filed automatically by nightly CI"
L converge-audit  "5319E7" "Filed by the weekly spec-drift audit"
L security        "B60205" "Security-relevant; see policy §7"
L red-team        "B60205" "Adversarial finding; falsification evidence attached"
L incident        "B60205" "Bad merge reached main; postmortem required"
L ci-failure      "D93F0B" "Filed by ci-failure-router for a failed ci/security run on main; triaged like nightly-failure"
L auto-fix-1      "FBCA04" "CI auto-fix attempt 1 of 2 taken on this agent PR (ci-failure-router)"
L auto-fix-2      "FBCA04" "CI auto-fix attempt 2 of 2 taken; the next failure escalates to needs-human"
L dependencies    "0366D6" "Dependency currency PR (Dependabot)"
L spec-question   "0052CC" "Ambiguity needing a spec ruling"
# risk tiers (applied automatically by risk-score workflow)
L "risk:t0"       "E6E6E6" "Docs/tests/scenarios only"
L "risk:t1"       "C5DEF5" "Host-only code"
L "risk:t2"       "FBCA04" "Protocol-critical or toolchain change"
L "risk:t3"       "B60205" "Ground-truth/governance artefacts - human ruling"
# features
for f in f1-codecs f2-link f3-core f4-simrig f5-cli; do
  L "feature:$f" "BFD4F2" "OMGP feature $f"
done
# Project board (Projects v2). Set OWNER before running, e.g. OWNER=craig...
gh project create --owner "${OWNER:?set OWNER}" --title "OMGP Delivery" || true
echo "setup: labels done; add the project's built-in workflows (auto-add on"
echo "label 'task', move to Done on close) in the project UI."
