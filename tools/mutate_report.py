#!/usr/bin/env python3
r"""Merge Mull "Mutation Testing Elements" reports and apply the triage gate (spec 001
FR-027; ruling docs/OPEN-QUESTIONS.md 2026-08-29: the mutation gate is a triage of every
survivor, never a percentage).

Called by tools/mutate.sh after the runner; standalone:

  python3 tools/mutate_report.py --reports build/mutate/reports --root . \
      --scope-dirs "l3 link core" --ranges build/mutate/scope_ranges.json \
      --source-ext "cpp hpp h cc" --ref origin/main --out build/mutate/report.json

Policy (tools/mutate.cfg [policy] — T3 constants, never relaxed to get green):

  * A surviving mutant is LABELLED when the source line it sits on carries

        // mutant-ok(<category>[, <mutator>, ...]): <one-line justification>

    with <category> in `label_categories` (equivalent | accepted). Naming mutators restricts
    the label to those mutators on that line; a label without names covers every mutant on
    the line. An unknown category, or an empty justification, is a malformed label and
    fails the run in diff and trend mode (it is a policy syntax error, not a score). The one
    exception is an attestation (--attest-*, below): it has no changed source line, so every
    label it can read was already on main and gating on one would fail a PR for debt it did
    not touch — there it is reported and recorded, never gated.
  * --ref given (diff-scoped; CI deep-verify): survivors on lines the diff added or changed
    must all be labelled — exit 1 when the unlabelled count exceeds
    `max_unlabelled_survivors` (0).
  * no --ref (whole tree; nightly): the score is a trend, reported (and appended to
    --trend-log when given) and never gated. Exit 1 only for tool failures: no reports,
    no mutants in a non-empty scope, malformed labels.
  * --attest-* given (a test-only PR's attestation; tools/mutate.sh, #146, ruling
    docs/OPEN-QUESTIONS.md 2026-09-14 "Option A — attest, never gate"): trend mode over the
    dirs whose unit tests the diff changed. The three flags are one unit and must be given
    together — any one of them alone would switch the label rule below off. NO finding of the
    run gates — not a survivor, not a label, not the kill rate — because the diff changed no
    source line, so every finding is pre-existing debt and gating on it is the Option B that
    ruling rejected. What still exits 1 is the run failing to happen: no reports, no mutants
    in a non-empty scope, or no mutant executed under an attested dir (the per-dir blind spot
    below, which the diff-mode guard cannot reach here). Those say the attestation is empty,
    and exit 0 would be a false green.
  * A label whose line has no surviving mutant it could cover is STALE (the test was
    written or the code moved) — reported as a warning, never counted as a survivor.
  * "No mutants in a non-empty scope" is a tool-failure signal (blind spot: instrumentation
    not reaching code that exists), UNLESS every changed file in the diff scope is one of:
      - it carries

            // mutation-exempt(no-body): <one-line justification>

        anywhere in the file — for sources with no function body at all (e.g. a pure abstract
        interface: only `= 0` declarations), where Mull structurally cannot produce a mutant.
        Reviewed like mutant-ok, at file granularity: a human sees the marker in the PR diff
        and judges whether the file truly has no mutable code (docs/OPEN-QUESTIONS.md
        2026-08-30); or
      - the diff added at least one line in it, and every line it added OR deleted there is
        blank or a `//` comment that ends at its own newline. Phase 2 splices backslash-newline
        and phase 3 replaces what is then a comment by a space, both before anything Mull
        mutates exists, so no mutator can place a mutant on such a line whatever the
        instrumentation does — the same structural argument as the marker above, at line
        granularity, which is why it needs no marker (docs/OPEN-QUESTIONS.md 2026-09-14, and
        the amendment of the same date). Both halves of the diff are read, because a hunk that
        deletes a guard and puts a comment in its place has a comment-only added half; the
        deleted lines arrive as --removed (tools/mutate_ranges.py), each with the post-image
        line of its surviving predecessor, and are judged by the same per-line test plus the
        predecessor splice check the added half gets (a `//` comment deleted from after a
        `\`-ended line re-splices the line below into the macro; round-8 review on #558).
        See changed_lines_all_comments for the shapes it fails closed on: each
        is a case where the predicate would otherwise certify what it did not read, and there
        zero mutants is the blind spot again.
    Because a comment-only diff is exactly the shape of a PR that rewrites `mutant-ok`
    justifications, the malformed-label check below runs on every changed line before this
    exemption can return 0 — the exemption is from the blind-spot rule, never from the policy
    syntax.

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

# A file-level opt-out from the "no mutants in a non-empty scope" blind-spot check below,
# for sources Mull can never generate a mutant for (e.g. a pure abstract interface: only
# `= 0` declarations, no function body to mutate). Explicit and reviewed like mutant-ok —
# an empty report is otherwise indistinguishable from "instrumentation didn't reach the
# code", which is exactly the failure mode this check exists to catch.
NO_BODY_EXEMPT = re.compile(r"//\s*mutation-exempt\(no-body\)\s*:\s*(\S.*)")


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


def rel_of(root: str, path: str) -> str:
    """A report's file key relative to --root (Mull records the absolute path CMake compiled
    from; a key already relative is returned as is). Shared with mutate_diff_reports.py so
    the evidence tool keys mutants exactly as this gate does."""
    return path[len(root) + 1:] if path.startswith(root + "/") else path


def in_scope(rel: str, line, scope_dirs: list[str], ranges: dict) -> bool:
    """Embedded-path source (tests/tools carry mutants too but never count) and — with
    --ref — on a line the diff added or changed (new files are whole-file ranges)."""
    if not any(rel.startswith(d + "/") for d in scope_dirs):
        return False
    if not ranges:
        return True
    return any(a <= (line or -1) <= b for a, b in ranges.get(rel, []))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--reports", required=True, help="directory of Mull Elements JSON reports")
    ap.add_argument("--root", required=True, help="repository root (report paths are made relative to it)")
    ap.add_argument("--scope-dirs", required=True, help="space-separated embedded-path directories")
    ap.add_argument("--ranges", required=True, help="JSON {rel_path: [[start, end], ...]}; {} = whole tree")
    ap.add_argument("--removed", default="",
                    help="JSON {rel_path: [{\"text\": <deleted line>, \"after\": <post-image line of "
                         "its surviving predecessor; -1 = that line was deleted too; 0 = top of "
                         "file>}, ...]} (tools/mutate_ranges.py). Omitted, or any other shape "
                         "(an older text-only list) = the deleted half of the diff was never "
                         "read, so the comment-only blind-spot exemption is unavailable")
    ap.add_argument("--source-ext", required=True,
                    help="space-separated source extensions (tools/mutate.cfg source_ext, passed by mutate.sh)")
    ap.add_argument("--ref", default="", help="diff ref; empty = whole-tree trend mode")
    # Provenance of a test-derived attestation (#146, ruling docs/OPEN-QUESTIONS.md
    # 2026-09-14): mutate.sh passes these when the diff changed a dir's unit tests and no
    # source, so report.json records where the run came from instead of looking like an
    # ordinary whole-tree trend run. They never change what is counted or what gates.
    ap.add_argument("--attest-ref", default="", help="the diff ref the attestation was derived from")
    ap.add_argument("--attest-tests", default="", help="space-separated changed test files that named the dirs")
    ap.add_argument("--attest-dirs", default="", help="space-separated dirs attested")
    ap.add_argument("--out", required=True, help="report.json path")
    ap.add_argument("--max-unlabelled", type=int, default=0)
    ap.add_argument("--categories", default="equivalent accepted")
    ap.add_argument("--trend-log", default="", help="JSONL file to append the trend line to (trend mode)")
    ap.add_argument("--list-limit", type=int, default=200)
    args = ap.parse_args(argv)
    # All three or none: the flags describe ONE attestation, and `any` would let a stray
    # --attest-dirs on an ordinary whole-tree run switch the malformed-label gate off while
    # recording `origin: changed-tests` with no ref and no tests — a provenance false on its
    # face (#593 red-team round 3). A partial set is a usage error, never a suppressed gate.
    attest_flags = (args.attest_ref, args.attest_tests, args.attest_dirs)
    if any(attest_flags) and not all(attest_flags):
        ap.error("--attest-ref/--attest-tests/--attest-dirs describe one test-derived "
                 "attestation and must be given together (a partial set would suppress the "
                 "malformed-label gate and record a provenance that never happened)")
    attest = all(attest_flags)
    if attest and args.ref:
        # An attestation IS the trend mode (it has no changed line to gate on): the two
        # cannot be asked for at once, or the caller would get a gate it did not intend.
        ap.error("--attest-ref/--attest-tests/--attest-dirs describe a test-derived attestation, "
                 "which runs in trend mode — they cannot be combined with --ref")

    root = str(pathlib.Path(args.root).resolve())
    scope_dirs = args.scope_dirs.split()
    categories = args.categories.split()
    # mutate.sh builds the ranges from `git diff -U0 -- <scope dirs>` unfiltered, while its
    # SCOPE (and so the oracle) keeps only source extensions; a README/CMakeLists touch under a
    # scope dir would otherwise reach the per-dir blind-spot rule as a "changed file" and red
    # the gate for a dir whose sources did not change (#141 review, MEDIUM). A non-source file
    # can carry no mutant, so nothing that is counted changes. The extension list is the cfg's
    # one line, handed over by mutate.sh — no copy here to drift from it (#141 review @d1fc20b).
    source_re = re.compile(r"\.(" + "|".join(re.escape(e) for e in args.source_ext.split()) + r")$")
    ranges = {rel: r for rel, r in json.load(open(args.ranges)).items() if source_re.search(rel)}
    # None (not {}) when --removed is absent: "no file had a line deleted" and "nobody looked"
    # are different answers, and only the first can support an exemption.
    removed_lines = None
    if args.removed:
        removed_lines = {rel: t for rel, t in json.load(open(args.removed)).items()
                         if source_re.search(rel)}
        # Each entry is {"text", "after"} (tools/mutate_ranges.py). Anything else — an older
        # text-only list, a hand-written file — is a deleted half without the position the
        # splice check needs, and the exemption below fails closed on it.
        if any(not (isinstance(e, dict) and "text" in e and "after" in e)
               for entries in removed_lines.values() for e in entries):
            removed_lines = None
    reports = sorted(pathlib.Path(args.reports).glob("*.json"))
    diff_mode = bool(args.ref)

    def _rel(path: str) -> str:
        return rel_of(root, path)

    def _in_scope(rel: str, line) -> bool:
        return in_scope(rel, line, scope_dirs, ranges)

    best: dict[tuple, tuple[int, str]] = {}
    # Mutants the runner EXECUTED per scope dir, on any line: the per-changed-dir blind-spot
    # rule below reads this, not `best` (which is already line-filtered).
    executed_by_dir: dict[str, int] = {d: 0 for d in scope_dirs}
    for r in reports:
        try:
            doc = json.loads(r.read_text())
        except json.JSONDecodeError:
            print(f"mutation: could not parse {r.name}")
            continue
        for path, f in (doc.get("files") or {}).items():
            rel = _rel(path)
            top = rel.split("/", 1)[0]
            for m in f.get("mutants", []):
                if top in executed_by_dir:
                    executed_by_dir[top] += 1
                loc = (m.get("location") or {}).get("start") or {}
                if not _in_scope(rel, loc.get("line")):
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

    def exempt_reason(rel: str) -> str | None:
        for text in source_lines(rel):
            m = NO_BODY_EXEMPT.search(text)
            if m:
                return m.group(1).strip()
        return None

    def carries_no_code(text: str) -> bool:
        r"""One physical line that translation phases 2-3 leave empty, judged from the line
        itself: blank, or a `//` comment that ends at its own newline and does not end a block
        comment. The two exclusions are the shapes where a line READS as a comment and is not
        one:
          * `*/` anywhere on it — in the ordinary `/* disabled: ... \n// */ code` toggle the
            `*/` closes the block comment and what follows on that same line is executable
            (#558 red-team B2). `-Wcomment` does not fire on it (it warns about `/*` inside a
            comment), so nothing outside this predicate catches the shape;
          * a trailing `\` — phase 2 splices the NEXT line into the comment, so the line
            deletes mutable code rather than containing none (#558 red-team B3). In this repo
            -Wall's -Werror=comment fails such a build first, but that is a control in
            CMakeLists.txt, not a property of this predicate."""
        text = text.strip()
        if not text:
            return True
        return text.startswith("//") and not text.endswith("\\") and "*/" not in text

    def changed_lines_all_comments(rel: str) -> bool:
        r"""True when the diff added at least one line in `rel` and every line it added or
        deleted there carries no code. Such a line cannot carry a mutant by construction of
        C++: phase 2 splices backslash-newline, phase 3 replaces what is then a comment by a
        space, and only after that does anything Mull mutates exist. So an empty in-scope
        count over those lines is not a blind spot. Fails closed on every other shape, each a
        case where the predicate would otherwise certify what it did not read:
          * --removed absent — the deleted half of the diff was never handed over, so a
            deletion cannot be ruled out (#558 red-team B1);
          * a deleted line that carries code — a hunk that removes a guard and adds a `//`
            comment in its place has a comment-only ADDED half, and certifying it would read
            less of the diff than the empty-range-list case below rejects (same finding);
          * a deleted line whose surviving predecessor ends with `\` — the mirror of the
            added-line case below: the pre-image's comment ended that logical line, and with
            it gone the line after it is spliced into the macro (round-8 review on #558).
            Judged from `after`, the predecessor's post-image line, or from the previous
            deleted entry when that predecessor was deleted too; a deleted half without
            positions (an older text-only list) is treated as never handed over;
          * no added line at all — mutate.sh emits an empty range list for a file whose hunks
            are all deletions (`@@ -a,b +c,0 @@`); nothing there was analysed;
          * an added line whose predecessor in the tree ends with `\` — inserting a comment
            into a spliced logical line (a multi-line macro) truncates it, commenting out the
            continuation without deleting any line for the check above to see;
          * `R"` anywhere in the file — inside a raw string literal a `//` line is string
            data, the literal may open on a line the diff never touched, and its delimiters
            (`R"x( ... )x"`) are not decidable line by line. Never parsed, never exempt;
          * anything else on an added line — code, a `/* */` continuation, a line the ranges
            name but the checked-out tree does not have."""
        src = source_lines(rel)
        spans = ranges.get(rel, [])
        if not spans or removed_lines is None:
            return False
        deleted = removed_lines.get(rel, [])
        if any('R"' in t for t in src) or any('R"' in e["text"] for e in deleted):
            return False
        if not all(carries_no_code(e["text"]) for e in deleted):
            return False
        for i, e in enumerate(deleted):
            k = e["after"]
            if k == -1:
                pred = deleted[i - 1]["text"] if i > 0 else None
                if pred is None:
                    return False
            elif k == 0:
                pred = ""
            elif 1 <= k <= len(src):
                pred = src[k - 1]
            else:
                return False
            if pred.rstrip().endswith("\\"):
                return False
        for a, b in spans:
            for ln in range(a, b + 1):
                if ln > len(src):
                    return False
                if not carries_no_code(src[ln - 1]):
                    return False
                if ln >= 2 and src[ln - 2].rstrip().endswith("\\"):
                    return False
        return True

    def no_mutant_reason(rel: str) -> str | None:
        """Why `rel` can legitimately contribute no mutant on a changed line, or None."""
        marker = exempt_reason(rel)
        if marker is not None:
            return f"mutation-exempt(no-body): {marker}"
        if changed_lines_all_comments(rel):
            return "every line the diff added or deleted there is blank or a // comment"
        return None

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
    # Changed files join the scan even when no in-scope mutant put them in `best`: a diff that
    # only rewrites justifications touches exactly the lines the policy lives on, and the
    # comment-only exemption below would otherwise let a malformed label through unread.
    files_seen = sorted({k[0] for k in best} | (set(ranges) if diff_mode else set()))
    for rel in files_seen:
        for idx, text in enumerate(source_lines(rel), start=1):
            if not LABEL_ANY.search(text):
                continue
            target = idx + 1 if comment_only(text) else idx   # a comment-only label governs the next line
            # Malformed is policy syntax wherever the label sits, so the label's OWN line being
            # changed is enough to read it; staleness is a claim about the mutants on `target`
            # and stays gated on `target` (a label line changed while its target was not has no
            # in-scope mutant to be stale against, and reporting one would be noise).
            if not (_in_scope(rel, target) or _in_scope(rel, idx)):
                continue
            lab = parse_label(text, idx, categories)
            if lab is None or lab.error:
                if lab is not None and f"{rel}:{idx}: {lab.error}" not in malformed:
                    malformed.append(f"{rel}:{idx}: {lab.error}")
                continue
            if not _in_scope(rel, target):
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
    if attest:
        report["attest"] = {"origin": "changed-tests", "diff_ref": args.attest_ref,
                            "tests": args.attest_tests.split(), "dirs": args.attest_dirs.split()}
        print(f"mutation: attested from changed tests ({args.attest_tests or '?'}) at "
              f"{args.attest_ref or '?'}: {mode} mode over {args.attest_dirs or '?'} — no finding of "
              f"this run gates (survivors, labels and the kill rate are all listed, never exit 1); "
              f"a run that did not happen still does (#146)")
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n")

    if not reports:
        print("mutation: no Mull reports produced — failing (blind spot: the runner did not execute)")
        return 1
    if malformed:
        # Before the count-based rules: a malformed label is a policy syntax error in every
        # mode and at every count, including the zero-mutant exemptions just below.
        # Not on an attestation, though: with no changed source line, `in_scope` is
        # directory-wide, so this scan reads every label in the attested dirs — all of them
        # already on main. Failing there would red a test-only PR for debt it never touched,
        # which is exactly the Option B docs/OPEN-QUESTIONS.md 2026-09-14 rejected (#146).
        # Reported and recorded in report.json either way; the gate is the next PR that
        # changes a line under it, or the nightly whole-tree run.
        if not attest:
            print(f"mutation: {len(malformed)} malformed mutant-ok label(s) — failing (policy syntax, see ERROR lines)")
            return 1
        print(f"mutation: {len(malformed)} malformed mutant-ok label(s) in the attested dir(s) — reported, "
              f"NOT gated: this diff changed no source line, so they are pre-existing (see ERROR lines, #146)")
    if total + not_covered == 0:
        # Every changed file in scope (ranges' keys — diff mode only; whole-tree has no
        # single file list to check) structurally unable to carry a mutant on a changed line —
        # `mutation-exempt(no-body)`, or a comment-only change — means the diff genuinely
        # offers nothing to mutate, not that instrumentation missed mutable code.
        if diff_mode and ranges:
            reasons = {rel: no_mutant_reason(rel) for rel in ranges}
            unexempted = [rel for rel, reason in reasons.items() if reason is None]
            if not unexempted:
                for rel, reason in reasons.items():
                    print(f"mutation: {rel}: no mutants — {reason}")
                print("mutation: scope is non-empty but no changed line in it can carry a mutant "
                      "(see above) — not a blind spot")
                return 0
            print("mutation: scope is non-empty but Mull generated no mutants, and the following changed "
                  "file(s) added or deleted a line that is neither blank nor a `//` comment and carry no "
                  "`mutation-exempt(no-body)` marker — failing (blind spot: instrumentation is not "
                  "reaching the code): " + ", ".join(sorted(unexempted)))
            return 1
        print("mutation: scope is non-empty but Mull generated no mutants — failing (blind spot: instrumentation is not reaching the code)")
        return 1
    if diff_mode and ranges:
        # Per changed dir (diff mode): the runner must have executed at least one mutant under
        # it, on ANY line. mutate.sh's run-time includePaths filter is one regex per scope dir
        # against the absolute source path; a regex that matches `link/` but not `core/`
        # leaves total > 0, so the rule above never fires while every core/ mutant silently
        # goes unexecuted (#141 review, LOW). The report post-filter can only REMOVE mutants,
        # never restore ones the runner declined to run — so this is the check that the
        # filter reached every changed dir. Exempt: a dir whose changed files can none of them
        # carry a mutant on a changed line (`mutation-exempt(no-body)`, or a comment-only
        # change) — there is nothing under it for the filter to have hidden.
        silent = []
        for d in sorted({rel.split("/", 1)[0] for rel in ranges}):
            if executed_by_dir.get(d, 0):
                continue
            if all(no_mutant_reason(rel) is not None for rel in ranges if rel.startswith(d + "/")):
                continue
            silent.append(d)
        if silent:
            print("mutation: the runner executed no mutants under " + ", ".join(f"{d}/" for d in silent)
                  + " although the diff changes sources there — failing (blind spot: the run-time path "
                  "filter or instrumentation is not reaching that directory)")
            return 1
    if attest:
        # The same rule, per ATTESTED dir, and the one place it is reachable on this path: an
        # attestation is trend mode with no ranges, so the block above (guarded on
        # `diff_mode and ranges`) never runs — while this is the only mode that NAMES specific
        # directories in report.json. The oracle binaries of a dir's unit tests carry sibling
        # scope dirs' mutants too, so one mutant anywhere in scope keeps total > 0 and the
        # global "no mutants" rule stays silent while the attested dir itself ran nothing:
        # report.json would then claim a kill rate for a directory that was never mutated
        # (#593 red-team round 3). mutate.sh has already failed closed on an attested dir with
        # no source file at all, so zero here is the run-time path filter or the
        # instrumentation, not an empty directory. This is a blind spot, not a finding: it
        # says the attestation did not happen, which is what an attestation still gates on.
        silent = [d for d in args.attest_dirs.split() if not executed_by_dir.get(d, 0)]
        if silent:
            print("mutation: the attestation names " + ", ".join(f"{d}/" for d in silent)
                  + " but the runner executed no mutant there — failing (blind spot: the run-time "
                  "path filter or instrumentation is not reaching that directory, so a kill rate "
                  "would be recorded for a directory that was never mutated; #146)")
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
