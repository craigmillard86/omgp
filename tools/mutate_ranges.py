#!/usr/bin/env python3
r"""Split `git diff -U0` into the two per-file line sets the mutation gate reads.

  git diff -U0 <ref> -- <scope dirs> | python3 tools/mutate_ranges.py \
      --ranges-out build/mutate/scope_ranges.json --removed-out build/mutate/scope_removed.json

* ranges  {rel_path: [[start, end], ...]} — the lines the diff ADDED or CHANGED (new files are
  whole-file ranges). tools/mutate_report.py scopes survivors to these lines; a file whose
  hunks are all pure deletions keeps an empty list, which that gate reads as "nothing here was
  analysed" and never exempts.
* removed {rel_path: [text, ...]} — the text of the lines the diff DELETED, in hunk order. The
  comment-only blind-spot exemption needs it: the added half of a hunk that deletes a guard and
  puts a `//` comment in its place is a comment, and deciding on that half alone certifies a
  file whose diff took mutable code away (#558 red-team B1).

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
    removed: dict[str, list[str]] = {}
    cur, in_hunk = None, False
    for line in diff.splitlines():
        if line.startswith("diff --git "):
            cur, in_hunk = None, False
        elif line.startswith("+++ "):
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
        elif in_hunk and cur and line.startswith("-"):
            # Inside a hunk every `-` line is deleted content, including one whose own text
            # starts with `--` (`--count;` at column 0); the `---`/`+++` headers precede the
            # first `@@` and so are never seen here.
            removed[cur].append(line[1:])
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
