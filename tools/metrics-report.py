#!/usr/bin/env python3
"""Summarise metrics/delivery-log.jsonl into AI-DLC scorecard numbers."""
import json, pathlib, statistics as st
log = pathlib.Path("metrics/delivery-log.jsonl")
if not log.exists():
    raise SystemExit("no delivery log yet")
recs = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
def block(name, rs):
    if not rs: return
    ct = [r["cycle_time_h"] for r in rs]
    print(f"\n== {name} ({len(rs)} PRs) ==")
    print(f"  cycle time h: median {st.median(ct):.1f}  p90 {sorted(ct)[int(len(ct)*0.9)-1] if len(ct)>1 else ct[0]:.1f}")
    print(f"  first-pass CI green: {sum(r['first_pass_green'] for r in rs)}/{len(rs)}")
    print(f"  mean CI runs to merge: {st.mean([r['ci_runs'] for r in rs]):.1f}")
    print(f"  review rework (changes requested): {sum(r['changes_requested'] for r in rs)}/{len(rs)}")
    print(f"  size: median {st.median([r['additions']+r['deletions'] for r in rs]):.0f} lines changed")
block("ALL", recs)
block("agent-authored", [r for r in recs if r["author_type"] == "agent"])
block("human-authored", [r for r in recs if r["author_type"] == "human"])
by_feat = {}
for r in recs: by_feat.setdefault(r["feature"], []).append(r)
for f, rs in sorted(by_feat.items()): block(f"feature:{f}", rs)

tlog = pathlib.Path("metrics/task-log.jsonl")
if tlog.exists():
    ts = [json.loads(l) for l in tlog.read_text().splitlines() if l.strip()]
    print(f"\n== tasks ({len(ts)} closed) ==")
    lead = [t["lead_time_h"] for t in ts]; act = [t["active_time_h"] for t in ts if t["active_time_h"]]
    print(f"  lead time h: median {st.median(lead):.1f}")
    if act: print(f"  active time h: median {st.median(act):.1f}  (flow efficiency {100*st.median(act)/max(st.median(lead),0.1):.0f}%)")
    for o in ("planned","nightly","audit"):
        n = sum(1 for t in ts if t["origin"]==o)
        if n: print(f"  origin {o}: {n}")
    ag = sum(1 for t in ts if t["assignee_type"]=="agent")
    print(f"  agent-executed: {ag}/{len(ts)}")
