#!/usr/bin/env python3
r"""Split `git diff -U0` into the two per-file line sets the mutation gate reads.

  git diff -U0 <ref> -- <scope dirs> | python3 tools/mutate_ranges.py \
      --ranges-out build/mutate/scope_ranges.json --removed-out build/mutate/scope_removed.json

* ranges  {rel_path: [[start, end], ...]} — the lines the diff ADDED or CHANGED (new files are
  whole-file ranges). tools/mutate_report.py scopes survivors to these lines; a file whose
  hunks are all pure deletions keeps an empty list, which that gate reads as "nothing here was
  analysed" and never exempts.
* removed {rel_path: [{"text": str, "after": int}, ...]} — the lines the diff DELETED, in hunk
  order. The comment-only blind-spot exemption needs them: the added half of a hunk that
  deletes a guard and puts a `//` comment in its place is a comment, and deciding on that half
  alone certifies a file whose diff took mutable code away (#558 red-team B1). `after` is the
  POST-image line number of the deleted line's surviving predecessor — the line before the
  hunk, identical in both images since -U0 hunks start at the first changed line — or -1 when
  that predecessor was deleted too (judge it against the previous entry), or 0 at the top of
  the file. The predicate reads it for the splice check on the deleted half: a `//` comment
  deleted from right after a `\`-ended line re-splices the line below into that logical line
  (round-8 review on #558), the mirror of the added-line case, and text alone cannot see it.

Both maps key on the post-image path (`+++ b/<path>`), so a file the diff deletes outright
(`+++ /dev/null`) appears in neither — it has no line for the gate to scope to. Parsing lives
here, in one place, so tools/refimpl/test_tooling.py can drive it from a real `git diff`.
"""
from __future__ import annotations

import argparse
import json
import re
import sys

HUNK = re.compile(r"@@ -\S+ \+(\d+)(?:,(\d+))? @@")


def parse(diff: str) -> tuple[dict, dict]:
    """(ranges, removed) for `diff`, the output of `git diff -U0`."""
    ranges: dict[str, list[list[int]]] = {}
    removed: dict[str, list[dict]] = {}
    cur, in_hunk = None, False
    after = 0          # the surviving predecessor of the NEXT deleted line in this hunk
    first_in_hunk = True
    for line in diff.splitlines():
        if line.startswith("diff --git "):
            cur, in_hunk = None, False
        elif not in_hunk and line.startswith("+++ "):
            # `not in_hunk` for the same reason the `-` branch below has it: an ADDED line whose
            # own text starts with `++ ` renders as `+++ new note`, and read as a header it
            # re-keys the file's remaining hunks under a fabricated path — the changed lines
            # after it leave the gate's scope and a survivor on them is never counted (#558
            # red-team B-A). A real `+++` header always precedes its file's first `@@`.
            p = line[4:].strip()
            cur = None if p == "/dev/null" else (p[2:] if p.startswith("b/") else p)
            in_hunk = False
            if cur:
                ranges.setdefault(cur, [])
                removed.setdefault(cur, [])
        elif line.startswith("@@") and cur:
            m = HUNK.match(line)
            if not m:
                continue
            start = int(m.group(1))
            count = int(m.group(2)) if m.group(2) is not None else 1
            if count:
                ranges[cur].append([start, start + count - 1])
            in_hunk = True
            # Unified diff: for a zero-count post range the number is the line BEFORE the
            # deletion point; otherwise the hunk's first post line is the first changed one, so
            # the surviving predecessor is the line before it. 0 = deleted from the top.
            after = start if count == 0 else start - 1
            first_in_hunk = True
        elif in_hunk and cur and line.startswith("-"):
            # Inside a hunk every `-` line is deleted content, including one whose own text
            # starts with `--` (`--count;` at column 0); the `---`/`+++` headers precede the
            # first `@@` and so are never seen here.
            removed[cur].append({"text": line[1:], "after": after if first_in_hunk else -1})
            first_in_hunk = False
    return ranges, removed


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ranges-out", required=True)
    ap.add_argument("--removed-out", required=True)
    args = ap.parse_args(argv)
    ranges, removed = parse(sys.stdin.read())
    with open(args.ranges_out, "w") as fh:
        json.dump(ranges, fh)
    with open(args.removed_out, "w") as fh:
        json.dump(removed, fh)
    return 0


if __name__ == "__main__":
    sys.exit(main())
