#!/usr/bin/env python3
"""How many adversarial rounds a PR has already had, and whether that exceeds the budget.

Ruling 2026-09-07 (docs/GOVERNANCE.md §4, "Adversarial round budget"). The scope ruling
(#134/#136) bounded findings by SCOPE — a defect in the changed code blocks, an adjacent
improvement is filed as a follow-up. This bounds them by TIME as well: the search space a
red team draws from is the set of properties the code does NOT check, which is unbounded,
so a PR that answers every round faithfully can still be examined forever. #149 took six
rounds and #145 five, each costing a full deep-verify + attack-pr cycle.

Reads the PR's comments (JSON, as `gh api .../issues/<n>/comments --paginate` emits them)
on stdin and prints GITHUB_OUTPUT lines:

    round=<n>            1 on the first pass; +1 per DISTINCT earlier head already judged
    budget=<n>           the effective budget after clamping (0 = off)
    budget_exceeded=<bool>
    previous_head=<sha>  the head of the most recent verdict at another commit ("" if none)

A round is a head, not a comment: a red-team verdict following a review verdict on the same
commit is one round, the same rule the review-fix loop already uses for its attempts.

Fail closed: anything unreadable leaves the budget OFF, which is the stricter, pre-ruling
behaviour (every in-scope finding blocks). A parse failure must never widen what an agent
may wave through.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

# The verdict shape agent-approve.yml and red-team.yml's own guard parse: the LAST non-blank
# line of a claude[bot] comment, trimmed, and nothing after it. A verdict quoted mid-comment
# is not a verdict (#103) — otherwise a PR could spend its own budget by quoting itself.
VERDICT = re.compile(r"^VERDICT\((?:review|red-team)\):\s*(?:clean|findings)\s*@\s*([0-9a-f]{40})$")
BUDGET_KEY = re.compile(r"^adversarial_round_budget:\s*(\d+)\s*(?:#.*)?$", re.MULTILINE)
MAX_BUDGET = 10


def verdict_head(comment: dict) -> str | None:
    """The head sha a claude[bot] comment gives a verdict for, or None."""
    if (comment.get("user") or {}).get("login") != "claude[bot]":
        return None
    lines = [ln for ln in (comment.get("body") or "").split("\n") if ln.strip()]
    if not lines:
        return None
    m = VERDICT.match(lines[-1].strip())
    return m.group(1) if m else None


def budget_of(config_text: str) -> int:
    """The configured budget, clamped. 0 (or unreadable) means the budget is off.

    A value above MAX_BUDGET is clamped DOWN to MAX_BUDGET rather than off: the failure a
    clamp guards against is a typo making the budget unreachable, which would silently
    restore the unbounded loop this ruling exists to end (auto_fix_max_attempts' own rule).
    """
    m = BUDGET_KEY.search(config_text)
    if not m:
        return 0
    return min(int(m.group(1)), MAX_BUDGET)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--head", required=True, help="the PR's current head sha (40 chars)")
    ap.add_argument("--config", default=".github/agent-config.yml")
    args = ap.parse_args()

    data = sys.stdin.read()
    comments: list[dict] = []
    try:
        decoder = json.JSONDecoder()
        pos = 0
        while True:
            while pos < len(data) and data[pos].isspace():
                pos += 1
            if pos >= len(data):
                break
            obj, pos = decoder.raw_decode(data, pos)
            if isinstance(obj, list):
                comments.extend(x for x in obj if isinstance(x, dict))
            elif isinstance(obj, dict):
                comments.append(obj)
    except (json.JSONDecodeError, ValueError, TypeError):
        comments = []
    seen: list[str] = []
    for c in comments if isinstance(comments, list) else []:
        head = verdict_head(c) if isinstance(c, dict) else None
        # The current head's own verdicts are this round's, not a previous one's.
        if head and head != args.head and head not in seen:
            seen.append(head)

    try:
        budget = budget_of(pathlib.Path(args.config).read_text())
    except OSError:
        budget = 0

    rounds = len(seen) + 1
    exceeded = budget >= 1 and rounds > budget
    # Comments arrive oldest-first from the API, so the last distinct head seen is the most
    # recent one judged — the base for "what did the fix rounds change since then".
    print(f"round={rounds}")
    print(f"budget={budget}")
    print(f"budget_exceeded={'true' if exceeded else 'false'}")
    print(f"previous_head={seen[-1] if seen else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
