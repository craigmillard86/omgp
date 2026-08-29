#!/usr/bin/env python3
"""Summarise the metrics ledger into AI-DLC scorecard numbers.

The ledger lives on the unprotected `metrics` branch (append-only; RUNBOOK "Routine
operations"), so this fetches origin/metrics and reads `origin/metrics:metrics/*.jsonl`,
falling back to the working tree's metrics/ only when that branch does not exist yet.
The two empty cases are reported differently on purpose:
  * "no `metrics` branch on origin yet"  — nothing has ever been recorded: either no PR has
    merged since the ledger moved to the branch, or the recording workflows are failing
    (check delivery-metrics / task-metrics runs — a rejected push fails them loudly now);
  * "origin/metrics exists but has no metrics/<file> yet" — recording works; that log
    simply has no entries of its kind.

  python3 tools/metrics-report.py                  # fetch origin/metrics, report
  python3 tools/metrics-report.py --source local   # read metrics/ from the working tree only
  python3 tools/metrics-report.py --source branch  # the branch or nothing (no local fallback)
"""
import argparse
import json
import pathlib
import statistics as st
import subprocess
import sys

REMOTE, BRANCH = "origin", "metrics"


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True)


def branch_state():
    """'present' | 'absent' | 'error: <why>' for REMOTE/BRANCH (ls-remote --exit-code)."""
    r = git("ls-remote", "--exit-code", "--heads", REMOTE, f"refs/heads/{BRANCH}")
    if r.returncode == 0:
        return "present"
    if r.returncode == 2:
        return "absent"
    return f"error: {r.stderr.strip() or r.stdout.strip() or f'rc={r.returncode}'}"


def load(name, source):
    """Return (lines | None, where). `where` says where the data came from or why not."""
    note = None
    if source in ("auto", "branch"):
        state = branch_state()
        if state == "present":
            # Explicit refspec: works in shallow / single-branch clones too.
            f = git("fetch", "-q", REMOTE, f"+refs/heads/{BRANCH}:refs/remotes/{REMOTE}/{BRANCH}")
            if f.returncode:
                return None, f"fetch of {REMOTE}/{BRANCH} failed: {f.stderr.strip()}"
            r = git("show", f"{REMOTE}/{BRANCH}:metrics/{name}")
            if r.returncode:
                return None, (f"{REMOTE}/{BRANCH} exists but has no metrics/{name} yet "
                              f"(recording works; no entries of this kind)")
            return r.stdout.splitlines(), f"{REMOTE}/{BRANCH}:metrics/{name}"
        note = (f"no `{BRANCH}` branch on {REMOTE} yet" if state == "absent"
                else f"could not query {REMOTE} ({state})")
        if source == "branch":
            return None, note
    p = pathlib.Path("metrics") / name
    if p.exists():
        where = f"local metrics/{name}" + (f" ({note}; local fallback)" if note else "")
        return p.read_text().splitlines(), where
    if note:
        return None, (f"{note} — and no local metrics/{name} either: nothing has been recorded. "
                      f"Check the delivery-metrics / task-metrics workflow runs.")
    return None, f"no local metrics/{name}"


def records(lines):
    return [json.loads(l) for l in lines if l.strip()]


def block(name, rs):
    if not rs:
        return
    ct = [r["cycle_time_h"] for r in rs]
    print(f"\n== {name} ({len(rs)} PRs) ==")
    print(f"  cycle time h: median {st.median(ct):.1f}  p90 {sorted(ct)[int(len(ct)*0.9)-1] if len(ct)>1 else ct[0]:.1f}")
    print(f"  first-pass CI green: {sum(r['first_pass_green'] for r in rs)}/{len(rs)}")
    print(f"  mean CI runs to merge: {st.mean([r['ci_runs'] for r in rs]):.1f}")
    print(f"  review rework (changes requested): {sum(r['changes_requested'] for r in rs)}/{len(rs)}")
    print(f"  size: median {st.median([r['additions']+r['deletions'] for r in rs]):.0f} lines changed")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--source", choices=["auto", "branch", "local"], default="auto",
                    help="auto: origin/metrics, else local metrics/ (default); branch: no fallback; local: working tree only")
    args = ap.parse_args(argv)

    lines, where = load("delivery-log.jsonl", args.source)
    if lines is None:
        print(f"metrics-report: {where}")
        return 1
    print(f"metrics-report: delivery log from {where}")
    recs = records(lines)
    if not recs:
        print("metrics-report: delivery log is present but empty")
        return 1
    block("ALL", recs)
    block("agent-authored", [r for r in recs if r["author_type"] == "agent"])
    block("human-authored", [r for r in recs if r["author_type"] == "human"])
    by_feat = {}
    for r in recs:
        by_feat.setdefault(r["feature"], []).append(r)
    for f, rs in sorted(by_feat.items()):
        block(f"feature:{f}", rs)

    tlines, twhere = load("task-log.jsonl", args.source)
    if tlines is None:
        print(f"\nmetrics-report: task log: {twhere}")
        return 0
    ts = records(tlines)
    print(f"\n== tasks ({len(ts)} closed; from {twhere}) ==")
    if ts:
        lead = [t["lead_time_h"] for t in ts]
        act = [t["active_time_h"] for t in ts if t["active_time_h"]]
        print(f"  lead time h: median {st.median(lead):.1f}")
        if act:
            print(f"  active time h: median {st.median(act):.1f}  (flow efficiency {100*st.median(act)/max(st.median(lead),0.1):.0f}%)")
        for o in ("planned", "nightly", "audit"):
            n = sum(1 for t in ts if t["origin"] == o)
            if n:
                print(f"  origin {o}: {n}")
        ag = sum(1 for t in ts if t["assignee_type"] == "agent")
        print(f"  agent-executed: {ag}/{len(ts)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
