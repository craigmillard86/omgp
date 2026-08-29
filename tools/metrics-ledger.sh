#!/usr/bin/env bash
# Append-only metrics ledger on the unprotected `metrics` branch (RUNBOOK "Routine
# operations"). Used by .github/workflows/delivery-metrics.yml and task-metrics.yml.
#
# Why a branch: main's branch protection rejects the bot's push (GH006). Until 2026-08-29
# the workflows ran `git push || echo "nothing to commit"`, so every rejected push produced
# a green run that recorded nothing (incident PR fix/metrics-branch). Two rules here:
#   * the no-op case ("nothing to commit", exit 0) and the rejected-push case (exit 1) are
#     distinguished — there is no fallback on the push;
#   * after a push, the remote branch head is read back and must equal HEAD.
#
#   tools/metrics-ledger.sh checkout                  # switch to metrics (create from HEAD if absent)
#   tools/metrics-ledger.sh commit <bot-name> <msg>   # add metrics/, commit, push, verify remote
#
# The workflows copy this file to $RUNNER_TEMP before `checkout`, so the copy that runs
# `commit` is main's, not whatever revision the metrics branch happens to carry.
# METRICS_REMOTE (default origin) and METRICS_BRANCH (default metrics) override the target;
# the tests in tools/refimpl/test_metrics_ledger.py drive this against a temporary bare remote.
set -euo pipefail
REMOTE=${METRICS_REMOTE:-origin}
BRANCH=${METRICS_BRANCH:-metrics}

branch_on_remote() {
  # 0 = present, 2 = absent, anything else = ls-remote itself failed (network, auth, no remote)
  git ls-remote --exit-code --heads "$REMOTE" "refs/heads/$BRANCH" >/dev/null 2>&1
}

case "${1:-}" in
  checkout)
    if branch_on_remote; then
      git fetch -q "$REMOTE" "refs/heads/$BRANCH"
      git checkout -q -B "$BRANCH" FETCH_HEAD
      echo "metrics: on $BRANCH at $(git rev-parse --short HEAD) (from $REMOTE)"
    else
      rc=$?
      if [ "$rc" -ne 2 ]; then
        echo "metrics: git ls-remote $REMOTE failed (rc=$rc) — that is not a missing branch; refusing to guess" >&2
        exit 1
      fi
      git checkout -q -B "$BRANCH"
      echo "metrics: $REMOTE/$BRANCH absent — created $BRANCH from $(git rev-parse --short HEAD)"
    fi
    ;;
  commit)
    NAME=${2:?usage: commit <bot-name> <message>}
    MSG=${3:?usage: commit <bot-name> <message>}
    git config user.name "$NAME"
    git config user.email "metrics@users.noreply.github.com"
    if ls metrics/* >/dev/null 2>&1; then
      git add -A -- metrics
    fi
    if git diff --cached --quiet; then
      echo "metrics: nothing to commit"
      exit 0
    fi
    git commit -q -m "$MSG"
    # No fallback: a rejected push (protected branch, non-fast-forward, auth) fails the run.
    git push "$REMOTE" "HEAD:refs/heads/$BRANCH"
    remote_sha=$(git ls-remote "$REMOTE" "refs/heads/$BRANCH" | cut -f1)
    local_sha=$(git rev-parse HEAD)
    if [ "$remote_sha" != "$local_sha" ]; then
      echo "metrics: push returned success but $REMOTE/$BRANCH is at ${remote_sha:-<absent>}, expected $local_sha" >&2
      exit 1
    fi
    echo "metrics: $REMOTE/$BRANCH -> $(git rev-parse --short HEAD) ($MSG)"
    ;;
  *)
    echo "usage: $0 checkout | commit <bot-name> <message>" >&2
    exit 2
    ;;
esac
