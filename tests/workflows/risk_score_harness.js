// Executes the tiering github-script body of .github/workflows/risk-score.yml VERBATIM against a
// mocked GitHub API. Driven by tools/refimpl/test_workflow_scripts.py, which extracts the script
// into a JSON file (argv[2]).
//
// Why this exists (round-1 red team + review on #420). risk-score had NO tests, and its tier is
// what `agent-approve` and `agent-merge` gate on — so every autonomy decision in the repo rests on
// an untested regex. The specific hole: `removedTests` is anchored at `^tests/`, so deleting
// anything under `tools/refimpl/` — including the 441-test Python suite and the floor ratchet that
// #420 leans on — scored T1, inside `auto_merge_max_tier: 2`, i.e. autonomously mergeable. And a
// one-line change to the floor datum is `+1/-1`, so `deletions > additions` is false and it scored
// T0. Both are pinned below.
'use strict';
const fs = require('fs');
const S = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));

const results = [];
const check = (name, cond) => { results.push([name, !!cond]); if (!cond) process.exitCode = 1; };

// files: [{filename, additions, deletions, status, patch}] — status defaults to 'modified'.
function world(files, {additions = 10, deletions = 10, labels = []} = {}) {
  const log = [], applied = [];
  const F = files.map(f => Object.assign({additions: 1, deletions: 0, status: 'modified', patch: ''}, f));
  const github = {
    paginate: async () => F,
    rest: {
      pulls: {listFiles: async () => ({data: F})},
      issues: {
        addLabels: async ({labels: l}) => { applied.push(...l); log.push(`+${l.join('+')}`); },
        removeLabel: async ({name}) => log.push(`-${name}`),
        createComment: async ({body}) => log.push(`comment: ${body.replace(/\n+/g, ' | ').slice(0, 160)}`),
      },
    },
  };
  const chain = {addHeading: () => chain, addList: () => chain, write: async () => {}};
  const core = {info: () => {}, notice: m => log.push(`notice: ${m}`), summary: chain};
  const context = {
    repo: {owner: 'o', repo: 'r'},
    payload: {pull_request: {number: 7, additions, deletions, labels: labels.map(name => ({name}))}},
  };
  return {github, core, context, log, applied};
}

const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
const run = w => new AsyncFunction('github', 'context', 'core', 'require', S.score)(w.github, w.context, w.core, require);
const tierOf = w => (w.applied.find(l => /^risk:t[0-3]$/.test(l)) || '').replace('risk:t', '');

(async () => {
  const F = (filename, additions, deletions, status) => ({filename, additions, deletions, status});

  // --- controls: the rules that already worked --------------------------------------------
  let w = world([F('sim/rig.cpp', 20, 2)]);          await run(w);
  check('host-only code -> T1', tierOf(w) === '1');
  w = world([F('.github/workflows/ci.yml', 3, 1)]);  await run(w);
  check('governance artefact -> T3', tierOf(w) === '3');
  w = world([F('link/frame.cpp', 5, 1)]);            await run(w);
  check('portable protocol-critical code -> T2', tierOf(w) === '2');
  w = world([F('pipeline.sh', 2, 2)]);               await run(w);
  check('the pipeline definition -> T2', tierOf(w) === '2');
  w = world([F('tests/unit/test_link_master.cpp', 0, 300, 'removed')]); await run(w);
  check('deleting a C++ test -> T3 (reduces test content)', tierOf(w) === '3');
  w = world([F('docs/RUNBOOK.md', 4, 1)]);           await run(w);
  check('docs only -> T0', tierOf(w) === '0');
  w = world([F('.github/workflows/ci.yml', 3, 1)]);  await run(w);
  check('T3 also carries needs-human', w.applied.includes('needs-human'));

  // --- the holes round 1 found --------------------------------------------------------------
  // The Python suite is test content too: it drives every workflow harness in the repo.
  w = world([F('tools/refimpl/test_floor_datum.py', 0, 106, 'removed')]); await run(w);
  check('deleting the floor ratchet -> T3, not an autonomously mergeable T1', tierOf(w) === '3');

  w = world([F('tools/refimpl/test_workflow_scripts.py', 0, 560, 'removed')]); await run(w);
  check('deleting the workflow-harness driver -> T3', tierOf(w) === '3');

  w = world([F('tools/refimpl/test_test_set_gate.py', 2, 200)]); await run(w);
  check('gutting a Python test file (more deleted than added) -> T3', tierOf(w) === '3');

  // The floor datum is a gate threshold: changing it must not be a T0 "docs/tests only" PR.
  w = world([F('tests/unit-test-floor.txt', 1, 1)]); await run(w);
  check('changing the unit-test floor -> at least T2, never T0', Number(tierOf(w)) >= 2);

  // ...but ADDING a Python test must stay cheap, or the rule punishes new coverage.
  w = world([F('tools/refimpl/test_new_thing.py', 80, 0, 'added')]); await run(w);
  check('adding a Python test is not escalated', Number(tierOf(w)) <= 1);

  // --- round 2: a rename removes a test from the suite exactly as much as `git rm` does -------
  // `removedTests` matches f.filename and requires deletions > 0. GitHub reports a pure rename as
  // status 'renamed', additions 0, deletions 0, with the old path in previous_filename — the very
  // field this PR added to the agent-merge guard three files away. pytest collects test_*.py, so
  // renaming the ratchet out of that glob deletes it from the suite with no deletion anywhere.
  const R = (filename, previous_filename) => ({filename, previous_filename, additions: 0, deletions: 0, status: 'renamed'});

  w = world([R('tools/refimpl/floor_datum_check.py', 'tools/refimpl/test_floor_datum.py')]); await run(w);
  check('renaming the ratchet out of pytest\'s glob -> T3', tierOf(w) === '3');

  w = world([R('tools/refimpl/floor_datum_check.py', 'tools/refimpl/test_floor_datum.py'),
             F('tests/unit-test-floor.txt', 1, 1)]); await run(w);
  check('rename the ratchet AND lower the floor in one PR -> T3, never an autonomous T2', tierOf(w) === '3');

  w = world([R('tests/unit/disabled_link_master.cpp', 'tests/unit/test_link_master.cpp')]); await run(w);
  check('renaming a C++ test out of the glob -> T3', tierOf(w) === '3');

  // ...but a rename that KEEPS the file in the suite is ordinary refactoring, not a reduction.
  w = world([R('tools/refimpl/test_floor_ratchet.py', 'tools/refimpl/test_floor_datum.py')]); await run(w);
  check('renaming a test that stays a test is not escalated', Number(tierOf(w)) <= 1);

  w = world([R('tests/unit/test_link_loop2.cpp', 'tests/unit/test_link_loop.cpp')]); await run(w);
  check('renaming a C++ test that stays a test is not escalated', Number(tierOf(w)) <= 1);

  for (const [n, ok] of results) console.log((ok ? 'ok   ' : 'FAIL ') + n);
  console.log(`${results.filter(r => r[1]).length}/${results.length} cases passed`);
})();
