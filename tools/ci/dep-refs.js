'use strict';
// Shared blocking-dependency parser for ready-gate.yml and promote-queued.yml
// (docs/DEFINITION-OF-READY.md "Dependencies rule"). Both workflows currently carry a
// byte-for-byte copy of this logic inline (github-script has no import mechanism of its own);
// this module is the fix for that duplication — each workflow should
// `require(`${process.env.GITHUB_WORKSPACE}/tools/ci/dep-refs.js`)` instead. Not yet wired in:
// see the #92 PR thread for why (this session cannot push to .github/workflows/*.yml).
//
// Two correctness fixes over the inline copies, both driven by reproducers from the #92 review:
//
// 1. A list item may name more than one issue with different blocking status on one bullet
//    ("- #5 (T010) must land first; #6 is a sibling test, not blocking") — nothing in the
//    enrichment prompt (story-enrich.yml) or the PRD scheme rules this out. The old code tested
//    NONBLOCK against the whole joined item and discarded every ref on the item if it matched,
//    silently dropping the real, open, blocking #5 alongside the excused #6. This version splits
//    each item on ';' and applies NONBLOCK per clause, so an excuse only excuses its own clause.
// 2. The range regex required a literal '#' before both endpoints (`#19-#66`); the second '#' is
//    now optional (`#19-66` expands too) — a human is at least as likely to write the bare form.
//
// MAX_REFS caps how many distinct issue numbers one Dependencies section can resolve to (after
// range expansion). This is a fail-safe, not a truncation: callers MUST treat `capped: true` as
// "unresolved" (a gate failure for `ready`, a "still queued" hold for the promoter), never as
// "the refs beyond the cap don't matter" — a huge disjoint-range flood must not be able to make
// a real open dependency pass by exhausting it (#92 red-team Finding 2).
const MAX_REFS = 500;
const MAX_RANGE = 200;

const NONBLOCK = /\bnot\s+(?:a\s+)?(?:blocking|blocked|dependent)\b|\bnon-blocking\b|\bno ordering dependency\b|\bexpected to remain (?:open|so)\b|\bsame PR\b|\bsiblings?\b/i;

function depRefs(text) {
  const items = []; let cur = null;
  for (const line of (text || '').split('\n')) {
    if (/^\s*[-*]\s/.test(line)) { cur = [line]; items.push(cur); }
    else if (cur && line.trim() && /^\s+\S/.test(line)) cur.push(line);
    else cur = null;
  }
  const sources = items.length
    ? items.flatMap(i => i.join(' ').split(';').filter(clause => !NONBLOCK.test(clause)))
    : [text || ''];
  const refs = new Set();
  let capped = false;
  loop:
  for (const s of sources) {
    for (const m of s.matchAll(/#(\d+)\s*[–-]\s*#?(\d+)/g)) {
      const a = Number(m[1]), b = Number(m[2]);
      if (b > a && b - a <= MAX_RANGE) {
        for (let n = a; n <= b; n++) {
          if (refs.size >= MAX_REFS) { capped = true; break loop; }
          refs.add(n);
        }
      }
    }
    for (const m of s.matchAll(/#(\d+)/g)) {
      if (refs.size >= MAX_REFS) { capped = true; break loop; }
      refs.add(Number(m[1]));
    }
  }
  return { refs: [...refs].sort((x, y) => x - y), capped };
}

module.exports = { NONBLOCK, depRefs, MAX_REFS, MAX_RANGE };
