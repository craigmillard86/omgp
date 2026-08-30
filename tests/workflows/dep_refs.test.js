// Direct unit tests for tools/ci/dep-refs.js — the shared blocking-dependency parser meant to
// replace the duplicated inline copies in ready-gate.yml and promote-queued.yml (#92 review: the
// two copies are byte-for-byte duplicated and only cross-tested for 2 of 6 dependency rules).
// story_gate_harness.js exercises the same module through the wired workflows; this file tests it directly.
// Run: node tests/workflows/dep_refs.test.js
'use strict';
const path = require('path');
const { depRefs } = require(path.join(__dirname, '..', '..', 'tools', 'ci', 'dep-refs.js'));

const results = [];
const check = (name, cond) => { results.push([name, !!cond]); if (!cond) process.exitCode = 1; };
const refsOf = text => JSON.stringify(depRefs(text).refs);

// --- regression: same behaviour as the 27 cases in story_gate_harness.js for the rules that
//     carry over unchanged ---------------------------------------------------------------------
check('simple list item ref', refsOf('- #1 (T001)') === '[1]');
check('prose non-blocking mention with a preceding list item: prose ignored, list ref kept',
  refsOf('- #1 (T001) -- open\n\nNot a blocking dependency, but directly relevant: #4 (T009).') === '[1]');
check('"not blocked by" list item ignored',
  refsOf('- #1 (T001)\n- Not dependent on, and not blocked by, its sibling test tasks #4 (T016)') === '[1]');
check('"expected to remain open" item ignored',
  refsOf('- **#1 (T001)** closed\n- **#4 (T035)** OPEN and expected to remain so while this test is written') === '[1]');
check('ref on a continuation line counts',
  refsOf('- Must land first (Phase 1/2): #1 (T001),\n  #4 (T003)') === '[1,4]');
check('prose-only section counts every ref',
  refsOf('Blocked on the full Setup phase, all currently open:\n#1 (T001), #4 (T003).') === '[1,4]');
check('#A-#B range (both hashed) expands',
  refsOf('- #1–#4 (every preceding task)') === '[1,2,3,4]');

// --- fixes shipped in this pass -----------------------------------------------------------------
check('#A-B range (bare second endpoint) expands too',
  refsOf('- #1–4 (every preceding task)') === '[1,2,3,4]');
check('mixed bullet: a real blocker survives an excused ref on the same list item (semicolon-separated)',
  refsOf('- #5 (T010) must land first; #6 is a sibling test, not blocking') === '[5]');
check('mixed bullet, second phrasing',
  refsOf('- #9 (T020) blocks this story; unrelated to its sibling module #10, which is same PR') === '[9]');
check('a very large Dependencies section is capped, not silently truncated-and-passed', (() => {
  let deps = ''; let n = 1;
  while (deps.length < 40000) { deps += `- #${n}-${n + 200} (batch)\n`; n += 201; }
  return depRefs(deps).capped === true;
})());
check('a normal-sized range is not capped', depRefs('- #1–#4 (every preceding task)').capped === false);

for (const [n, ok] of results) console.log((ok ? 'ok   ' : 'FAIL ') + n);
console.log(`${results.filter(r => r[1]).length}/${results.length} cases passed`);
