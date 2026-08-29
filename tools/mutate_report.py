#!/usr/bin/env python3
"""Merge Mull "Mutation Testing Elements" reports and apply the triage gate (spec 001
FR-027; ruling docs/OPEN-QUESTIONS.md 2026-08-29: the mutation gate is a triage of every
survivor, never a percentage).

Called by tools/mutate.sh after the runner; standalone:

  python3 tools/mutate_report.py --reports build/mutate/reports --root . \
      --scope-dirs "l3 link core" --ranges build/mutate/scope_ranges.json \
      --ref origin/main --out build/mutate/report.json

Policy (tools/mutate.cfg [policy] — T3 constants, never relaxed to get green):

  * A surviving mutant is LABELLED when the source line it sits on carries

        // mutant-ok(<category>[, <mutator>, ...]): <one-line justification>

    with <category> in `label_categories` (equivalent | accepted). Naming mutators restricts
    the label to those mutators on that line; a label without names covers every mutant on
    the line. An unknown category, or an empty justification, is a malformed label and
    fails the run in every mode (it is a policy syntax error, not a score).
  * --ref given (diff-scoped; CI deep-verify): survivors on lines the diff added or changed
    must all be labelled — exit 1 when the unlabelled count exceeds
    `max_unlabelled_survivors` (0).
  * no --ref (whole tree; nightly): the score is a trend, reported (and appended to
    --trend-log when given) and never gated. Exit 1 only for tool failures: no reports,
    no mutants in a non-empty scope, malformed labels.
  * A label whose line has no surviving mutant it could cover is STALE (the test was
    written or the code moved) — reported as a warning, never counted as a survivor.

Every claim here is about the current report: "labelled" means a label was found on the
reported line, nothing more. The label's justification is reviewed by humans in the PR.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import pathlib
import re
import subprocess
import sys

# Status precedence when the same mutant appears in several binaries' reports: killed if
# ANY binary kills it, survived only if every binary that reached it let it live,
# not-covered if no binary reached it.
RANK = {"Killed": 3, "Timeout": 3, "RuntimeError": 3, "CompileError": 3, "Survived": 2,
        "NoCoverage": 1, "Ignored": 0, "Pending": 0}

LABEL = re.compile(r"//\s*mutant-ok\(\s*([A-Za-z_]+)\s*((?:,\s*[A-Za-z0-9_]+\s*)*)\)\s*:\s*(.*\S)")
LABEL_ANY = re.compile(r"//\s*mutant-ok\b")


class Label:
    __slots__ = ("category", "mutators", "justification", "line", "error")

    def __init__(self, category, mutators, justification, line, error=None):
        self.category, self.mutators, self.justification = category, mutators, justification
        self.line, self.error = line, error

    def covers(self, mutator: str) -> bool:
        return not self.mutators or mutator in self.mutators


def parse_label(text: str, line: int, categories: list[str]) -> Label | None:
    """The label on one source line, or None. A present-but-malformed label carries `error`."""
    if not LABEL_ANY.search(text):
        return None
    m = LABEL.search(text)
    if not m:
        return Label(None, [], "", line, "malformed label (expected `// mutant-ok(<category>[, mutator...]): <justification>`)")
    category = m.group(1)
    mutators = [x.strip() for x in m.group(2).split(",") if x.strip()]
    if category not in categories:
        return Label(category, mutators, m.group(3), line, f"unknown label category '{category}' (allowed: {', '.join(categories)})")
    return Label(category, mutators, m.group(3).strip(), line)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--reports", required=True, help="directory of Mull Elements JSON reports")
    ap.add_argument("--root", required=True, help="repository root (report paths are made relative to it)")
    ap.add_argument("--scope-dirs", required=True, help="space-separated embedded-path directories")
    ap.add_argument("--ranges", required=True, help="JSON {rel_path: [[start, end], ...]}; {} = whole tree")
    ap.add_argument("--ref", default="", help="diff ref; empty = whole-tree trend mode")
    ap.add_argument("--out", required=True, help="report.json path")
    ap.add_argument("--max-unlabelled", type=int, default=0)
    ap.add_argument("--categories", default="equivalent accepted")
    ap.add_argument("--trend-log", default="", help="JSONL file to append the trend line to (trend mode)")
    ap.add_argument("--list-limit", type=int, default=200)
    args = ap.parse_args(argv)

    root = str(pathlib.Path(args.root).resolve())
    scope_dirs = args.scope_dirs.split()
    categories = args.categories.split()
    ranges = json.load(open(args.ranges))
    reports = sorted(pathlib.Path(args.reports).glob("*.json"))
    diff_mode = bool(args.ref)

    def rel_of(path: str) -> str:
        return path[len(root) + 1:] if path.startswith(root + "/") else path

    def in_scope(rel: str, line) -> bool:
        """Embedded-path source (tests/tools carry mutants too but never count) and — with
        --ref — on a line the diff added or changed (new files are whole-file ranges)."""
        if not any(rel.startswith(d + "/") for d in scope_dirs):
            return False
        if not ranges:
            return True
        return any(a <= (line or -1) <= b for a, b in ranges.get(rel, []))

    best: dict[tuple, tuple[int, str]] = {}
    for r in reports:
        try:
            doc = json.loads(r.read_text())
        except json.JSONDecodeError:
            print(f"mutation: could not parse {r.name}")
            continue
        for path, f in (doc.get("files") or {}).items():
            rel = rel_of(path)
            for m in f.get("mutants", []):
                loc = (m.get("location") or {}).get("start") or {}
                if not in_scope(rel, loc.get("line")):
                    continue
                key = (rel, loc.get("line"), loc.get("column"), m.get("mutatorName"))
                rank = RANK.get(m.get("status"), 0)
                if rank > best.get(key, (-1, ""))[0]:
                    best[key] = (rank, m.get("status", "?"))

    # --- labels: read once per source file --------------------------------------------------
    lines_cache: dict[str, list[str]] = {}

    def source_lines(rel: str) -> list[str]:
        if rel not in lines_cache:
            try:
                lines_cache[rel] = pathlib.Path(root, rel).read_text(errors="replace").splitlines()
            except OSError:
                lines_cache[rel] = []
        return lines_cache[rel]

    def comment_only(text: str) -> bool:
        return text.lstrip().startswith("//")

    def label_at(rel: str, line) -> Label | None:
        """The label governing `line`: on the line itself, else on a comment-only line
        immediately above it (clang-format reflows long trailing comments, so a label
        that does not fit in the column limit goes on its own line above the code)."""
        src = source_lines(rel)
        if not line or line > len(src):
            return None
        lab = parse_label(src[line - 1], line, categories)
        if lab is None and line >= 2 and comment_only(src[line - 2]):
            lab = parse_label(src[line - 2], line - 1, categories)
        return lab

    killed = sum(1 for r, _ in best.values() if r == 3)
    not_covered = sum(1 for r, _ in best.values() if r == 1)
    survivors, malformed = [], []
    labelled_counts = {c: 0 for c in categories}
    for key, (rank, _) in sorted(best.items(), key=lambda kv: (kv[0][0], kv[0][1] or 0, kv[0][2] or 0)):
        if rank != 2:
            continue
        rel, line, col, mutator = key
        lab = label_at(rel, line)
        entry = {"file": rel, "line": line, "column": col, "mutator": mutator, "label": None}
        if lab is not None and lab.error:
            malformed.append(f"{rel}:{line}: {lab.error}")
        elif lab is not None and lab.covers(mutator):
            entry["label"] = {"category": lab.category, "justification": lab.justification}
            labelled_counts[lab.category] += 1
        survivors.append(entry)
    survived = len(survivors)
    unlabelled = [s for s in survivors if s["label"] is None]

    # Stale labels: any label on an in-scope line that covers no surviving mutant there.
    stale = []
    covered_lines: dict[tuple[str, int], set[str]] = {}
    for s in survivors:
        covered_lines.setdefault((s["file"], s["line"]), set()).add(s["mutator"])
    files_seen = sorted({k[0] for k in best})
    for rel in files_seen:
        for idx, text in enumerate(source_lines(rel), start=1):
            if not LABEL_ANY.search(text):
                continue
            target = idx + 1 if comment_only(text) else idx   # a comment-only label governs the next line
            if not in_scope(rel, target):
                continue
            lab = parse_label(text, idx, categories)
            if lab is None or lab.error:
                if lab is not None and f"{rel}:{idx}: {lab.error}" not in malformed:
                    malformed.append(f"{rel}:{idx}: {lab.error}")
                continue
            here = covered_lines.get((rel, target), set())
            if not any(lab.covers(m) for m in here):
                stale.append(f"{rel}:{idx} mutant-ok({lab.category}{', ' + ', '.join(lab.mutators) if lab.mutators else ''}) covers no surviving mutant on line {target}")

    total = killed + survived
    rate = (100.0 * killed / total) if total else 0.0
    mode = "diff" if diff_mode else "trend"
    ref = args.ref or "full tree"
    lab_str = " ".join(f"{c}={n}" for c, n in labelled_counts.items())
    print(f"mutation: mode={mode} diff_ref={ref} reports={len(reports)} mutants={total + not_covered} "
          f"killed={killed} survived={survived} not_covered={not_covered} kill_rate={rate:.1f}% "
          f"labelled[{lab_str}] unlabelled={len(unlabelled)} max_unlabelled={args.max_unlabelled}")
    for s in unlabelled[:args.list_limit]:
        print(f"  UNLABELLED survivor: {s['file']}:{s['line']}:{s['column']} {s['mutator']}")
    for s in [x for x in survivors if x["label"]][:args.list_limit]:
        print(f"  labelled ({s['label']['category']}): {s['file']}:{s['line']}:{s['column']} {s['mutator']} — {s['label']['justification']}")
    for w in stale:
        print(f"  warning: stale label: {w}")
    for e in malformed:
        print(f"  ERROR: {e}")

    report = {"mode": mode, "diff_ref": ref, "mutants_total": total + not_covered, "killed": killed,
              "survived": survived, "not_covered": not_covered, "kill_rate": round(rate, 1),
              "labelled": labelled_counts, "unlabelled": len(unlabelled),
              "max_unlabelled": args.max_unlabelled, "survivors": survivors, "stale_labels": stale,
              "malformed_labels": malformed}
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n")

    if not reports:
        print("mutation: no Mull reports produced — failing (blind spot: the runner did not execute)")
        return 1
    if total + not_covered == 0:
        print("mutation: scope is non-empty but Mull generated no mutants — failing (blind spot: instrumentation is not reaching the code)")
        return 1
    if malformed:
        print(f"mutation: {len(malformed)} malformed mutant-ok label(s) — failing (policy syntax, see ERROR lines)")
        return 1
    if diff_mode:
        if len(unlabelled) > args.max_unlabelled:
            print(f"mutation: {len(unlabelled)} unlabelled survivor(s) on changed lines — triage each: "
                  f"(a) write the killing test, (b) `// mutant-ok(equivalent): why`, (c) `// mutant-ok(accepted): why`")
            return 1
        return 0
    # Trend mode: the whole-tree score is information, never a gate.
    try:
        commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True,
                                cwd=root, check=False).stdout.strip() or "?"
    except OSError:
        commit = "?"
    trend = {"measured_at": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
             "commit": commit, "mutants": total + not_covered, "killed": killed, "survived": survived,
             "not_covered": not_covered, "kill_rate": round(rate, 1), "labelled": labelled_counts,
             "unlabelled": len(unlabelled)}
    print(f"mutation-trend: {json.dumps(trend, sort_keys=True)}")
    if args.trend_log:
        p = pathlib.Path(args.trend_log)
        p.parent.mkdir(parents=True, exist_ok=True)
        with p.open("a") as fh:
            fh.write(json.dumps(trend, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
