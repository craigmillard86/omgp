#!/usr/bin/env python3
"""Compare two Mull "Mutation Testing Elements" reports mutant by mutant.

Evidence tool for changes to tools/mutate.sh that must not alter what the gate sees: a
run-time path filter, a runner flag, a build change. Two reports for the same binary at
the same commit are compared on every mutant under the scope dirs, keyed exactly as
tools/mutate_report.py keys them (path, line, column, mutator); any mutant present in one
report and not the other, or with a different status, is listed and the exit status is 1.

    tools/mutate_diff_reports.py BEFORE.json AFTER.json [--scope-dirs "l3 link core"]

What the output would be if the claim were false: a non-empty list and exit 1. An empty
list with exit 0 is the evidence; identical totals alone are not (two swaps cancel).
"""
import argparse
import collections
import json
import sys


def load(path: str, scope_dirs: list[str]) -> dict[tuple, str]:
    doc = json.load(open(path))
    out: dict[tuple, str] = {}
    for src, f in (doc.get("files") or {}).items():
        rel = next((src[src.index("/" + d + "/") + 1:] for d in scope_dirs if "/" + d + "/" in src), None)
        if rel is None:
            continue
        for m in f.get("mutants", []):
            loc = (m.get("location") or {}).get("start") or {}
            out[(rel, loc.get("line"), loc.get("column"), m.get("mutatorName"))] = m.get("status", "?")
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--scope-dirs", default="l3 link core")
    args = ap.parse_args(argv)
    dirs = args.scope_dirs.split()
    a, b = load(args.before, dirs), load(args.after, dirs)
    diffs = [(k, a.get(k, "-"), b.get(k, "-")) for k in sorted(set(a) | set(b), key=str) if a.get(k) != b.get(k)]
    print(f"in-scope mutants: before={len(a)} {dict(collections.Counter(a.values()))}")
    print(f"                  after ={len(b)} {dict(collections.Counter(b.values()))}")
    for (rel, line, col, mut), sa, sb in diffs:
        print(f"  {rel}:{line}:{col} {mut}: {sa} -> {sb}")
    print(f"differences: {len(diffs)}")
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
