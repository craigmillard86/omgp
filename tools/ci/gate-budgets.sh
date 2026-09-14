#!/usr/bin/env bash
# Gate budgets from .github/agent-config.yml, in GITHUB_OUTPUT shape (key=value per line).
#
#   bash tools/ci/gate-budgets.sh [path/to/agent-config.yml]   (default: .github/agent-config.yml)
#
# Ruling 2026-09-14 (#153 items 1-2; docs/GOVERNANCE.md §4 "Gate budgets"): the judgement
# gates' timeouts, the red team's posting deadline and the two Claude actions' turn caps are
# config, read by a `budgets` job that checks out the DEFAULT branch — never the PR's own
# copy — so a PR cannot widen the gate that judges it. Fail closed: an absent, unreadable,
# fractional, hex, negative or out-of-range value falls back to the value the workflow
# carried hard-coded before the ruling (30 / 50 / 45 minutes; 150 turns) — never to a wider
# budget. The deadline is DERIVED (timeout - 10 min) so it cannot drift from the timeout.
# Always exits 0: a broken config narrows nothing and stops nothing; it only restores the
# old literals, and says so on stderr.
# Pinned by test_gate_budgets_* in tools/refimpl/test_workflow_scripts.py.
CFG="${1:-.github/agent-config.yml}"

read_key() {   # name default min max
  local v
  v=$(sed -n "s/^$1: *\"\{0,1\}\([0-9][0-9]*\)\"\{0,1\}[[:space:]]*\(#.*\)\{0,1\}$/\1/p" "$CFG" 2>/dev/null | head -1)
  if [ -z "$v" ] || [ "$v" -lt "$3" ] || [ "$v" -gt "$4" ]; then
    echo "gate-budgets: $1 absent, unreadable or outside $3..$4 in $CFG — using $2 (fail closed)" >&2
    v=$2
  fi
  printf '%s=%s\n' "$1" "$v"
  LAST=$v
}

read_key review_timeout_minutes      30  5 120
read_key red_team_timeout_minutes    50  5 120; RT=$LAST
read_key deep_verify_timeout_minutes 45  5 120
read_key review_max_turns            150 10 500
read_key red_team_max_turns          150 10 500
printf 'red_team_deadline_minutes=%s\n' "$((RT - 10))"
exit 0
