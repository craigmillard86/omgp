// Direct unit tests for tools/ci/agent-noop.js — the shared "did the agent actually produce
// anything?" check required by agent-dispatch.yml (implement) and review-fix.yml (fix).
// Run: node tests/workflows/agent_noop_harness.js
//
// Why this exists (#361). Five runs on 2026-09-07/08 and 2026-09-11 exited `success` having
// pushed no branch, opened no PR and posted no comment — #54 twice, #342's fix twice, and #58
// after 156 turns, 26 permission denials and $9.07. Each held its claim until a human noticed
// days later. Both workflows already carried a release/report step, and neither could ever fire:
// `agent-dispatch.yml:134` and `review-fix.yml:278` are `if: failure()`, and a silent no-op is
// not a failure. The detection therefore has to run on SUCCESS, which is what these cases pin.
//
// One implementation, tested directly here; the wiring (step ids, no `failure()` gate, the
// artifact upload, agent_retry_max) is pinned in tools/refimpl/test_workflow_scripts.py.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { detect, finalize } = require(path.join(__dirname, '..', '..', 'tools', 'ci', 'agent-noop.js'));

const results = [];
const check = (name, cond) => { results.push([name, !!cond]); if (!cond) process.exitCode = 1; };

// A mock of the slice of the API these two functions touch. Label removal 404s when the label is
// absent, as the real API does — finalize must treat that as a non-event.
function world({ prs = [], labels = [], comments = [], head = 'a'.repeat(40) } = {}) {
  const log = [], outputs = {};
  const state = { labels: [...labels], comments: [...comments], head };
  const github = {
    paginate: async (fn, args) => (await fn(args)).data,
    rest: {
      pulls: {
        list: async () => ({ data: prs }),
        get: async () => ({ data: { head: { sha: state.head } } }),
      },
      issues: {
        listComments: async () => ({ data: state.comments }),
        addLabels: async ({ labels: l }) => { state.labels.push(...l); log.push(`+${l.join('+')}`); },
        removeLabel: async ({ name }) => {
          if (!state.labels.includes(name)) throw Object.assign(new Error('Not Found'), { status: 404 });
          state.labels = state.labels.filter(x => x !== name); log.push(`-${name}`);
        },
        createComment: async ({ body }) => { state.comments.push({ body, user: { login: 'github-actions[bot]' } }); log.push(`comment: ${body.replace(/\n+/g, ' | ')}`); },
      },
    },
  };
  const core = {
    info: () => {}, notice: m => log.push(`notice: ${m}`), warning: m => log.push(`warning: ${m}`),
    setOutput: (k, v) => { outputs[k] = String(v); },
    setFailed: m => { log.push(`FAILED: ${m}`); outputs.__failed = String(m); },
  };
  const context = { repo: { owner: 'o', repo: 'r' }, runId: 4242, serverUrl: 'https://gh' };
  return { github, core, context, log, outputs, state };
}

const TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'noop-'));
let seq = 0;
const execFile = obj => { const p = path.join(TMP, `exec-${seq++}.json`); fs.writeFileSync(p, JSON.stringify(obj)); return p; };
const said = (w, re) => w.log.some(m => re.test(m));

// The run's clock: comments before T0 predate the run, comments after it are its output.
const T0 = '2026-09-12T10:00:00Z';
const before = '2026-09-12T09:59:00Z';
const during = '2026-09-12T10:00:30Z';

(async () => {
  // --- implement: did a PR appear for THIS issue? ------------------------------------------
  let w = world({ prs: [{ head: { ref: 'task/58' } }] });
  let r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', EXEC_FILE: execFile({ num_turns: 76, permission_denials_count: 20, is_error: false }) } });
  check('implement: a PR on task/<issue> is not a no-op', r.noop === false);
  check('implement: the execution figures are read', r.turns === 76 && r.denials === 20);
  check('implement: the figures reach the log', said(w, /turns=76/) && said(w, /denials=20/));

  // The execution file is a message ARRAY in practice; the totals are on its last result record.
  w = world({ prs: [{ head: { ref: 'task/58' } }] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', EXEC_FILE: execFile([
    { type: 'system', subtype: 'init' },
    { type: 'assistant', message: { content: 'working' } },
    { type: 'result', subtype: 'success', is_error: false, num_turns: 76, permission_denials_count: 20 }]) } });
  check('implement: array-form execution file is read the same way', r.turns === 76 && r.denials === 20 && r.noop === false);

  w = world({ prs: [] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', EXEC_FILE: execFile({ num_turns: 156, permission_denials_count: 26, is_error: false }) } });
  check('implement: no PR at all is a no-op', r.noop === true);
  check('implement: a no-op still reports what it burned', r.turns === 156 && r.denials === 26);

  w = world({ prs: [{ head: { ref: 'task/99' } }] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', EXEC_FILE: execFile({ num_turns: 5 }) } });
  check('implement: another issue\'s PR does not satisfy this one', r.noop === true);

  w = world({ prs: [{ head: { ref: 'task/58' } }] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', EXEC_FILE: execFile({ num_turns: 3, is_error: true }) } });
  check('implement: is_error is a no-op even with a PR present', r.noop === true);

  w = world({ prs: [{ head: { ref: 'task/58' }, state: 'closed' }] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', EXEC_FILE: execFile({ num_turns: 9 }) } });
  check('implement: a closed PR does not count as output', r.noop === true);

  w = world({ prs: [] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', EXEC_FILE: '/nonexistent/exec.json' } });
  check('implement: an unreadable execution file fails closed to no-op', r.noop === true);
  check('implement: and says so rather than pretending it read figures',
    said(w, /warning:/) && r.turns === null && r.denials === null);

  // --- fix: did the branch move, or did the agent at least explain itself? ------------------
  w = world({ head: 'b'.repeat(40) });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 85, permission_denials_count: 18 }) } });
  check('fix: a new head commit is not a no-op', r.noop === false);

  // review-fix may rebut a finding and change nothing (its step 5), so a comment IS production.
  w = world({ head: 'a'.repeat(40), comments: [{ user: { login: 'claude[bot]' }, created_at: during, body: 'Finding 2 is wrong because X; no code change.' }] });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 26, permission_denials_count: 4 }) } });
  check('fix: an explanatory claude[bot] comment during the run counts as output', r.noop === false);

  // The verdict that TRIGGERED the run is a claude[bot] comment too — it must not count.
  w = world({ head: 'a'.repeat(40), comments: [{ user: { login: 'claude[bot]' }, created_at: before, body: 'findings...\nVERDICT(review): findings @ ' + 'a'.repeat(40) }] });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 26, permission_denials_count: 4 }) } });
  check('fix: the triggering verdict comment is not the fixer\'s output', r.noop === true);

  // A red-team verdict landing mid-run is also not the fixer's work.
  w = world({ head: 'a'.repeat(40), comments: [{ user: { login: 'claude[bot]' }, created_at: during, body: 'attack notes\nVERDICT(red-team): findings @ ' + 'a'.repeat(40) }] });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 26, permission_denials_count: 4 }) } });
  check('fix: a verdict arriving DURING the run is still not output', r.noop === true);

  w = world({ head: 'a'.repeat(40), comments: [{ user: { login: 'github-actions[bot]' }, created_at: during, body: '🔧 review-fix attempt 1 of 4 starting' }] });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 26, permission_denials_count: 4 }) } });
  check('fix: the gate\'s own announcement comment is not the agent\'s output', r.noop === true);

  w = world({ head: 'a'.repeat(40) });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 26, permission_denials_count: 4 }) } });
  check('fix: same head, no comment, is a no-op', r.noop === true);

  // --- finalize (implement): free the claim, fail loudly ------------------------------------
  w = world({ labels: ['task', 'in-progress', 'ready'] });
  await finalize({ ...w, env: { KIND: 'implement', ISSUE: '58', ATTEMPTS: '2', TURNS: '156', DENIALS: '26' } });
  check('implement finalize: releases the claim', w.log.includes('-in-progress'));
  check('implement finalize: FAILS the job — silence was the whole defect', typeof w.outputs.__failed === 'string');
  check('implement finalize: the comment carries the per-attempt figures', said(w, /comment:.*156.*26/));
  check('implement finalize: names the run so a human can read the transcript', said(w, /comment:.*4242/));
  check('implement finalize: never applies a release label (GOVERNANCE §1)',
    !w.state.labels.includes('queued') && !said(w, /\+(ready|queued)/));

  w = world({ labels: ['task'] });
  let threw = null;
  try { await finalize({ ...w, env: { KIND: 'implement', ISSUE: '58', ATTEMPTS: '1', TURNS: '5', DENIALS: '0' } }); } catch (e) { threw = e; }
  check('implement finalize: an already-absent label 404 is tolerated', threw === null);
  check('implement finalize: still fails and still comments', typeof w.outputs.__failed === 'string' && said(w, /comment:/));

  // --- finalize (fix): hand the attempt back, escalate --------------------------------------
  w = world({ labels: ['risk:t2', 'agent-authored', 'review-fix-1'] });
  await finalize({ ...w, env: { KIND: 'fix', PR: '342', ATTEMPT: '1', ATTEMPTS: '2', TURNS: '26', DENIALS: '4' } });
  check('fix finalize: a no-op does not consume an attempt', w.log.includes('-review-fix-1'));
  check('fix finalize: escalates needs-human', w.state.labels.includes('needs-human'));
  check('fix finalize: FAILS the job', typeof w.outputs.__failed === 'string');
  check('fix finalize: the comment carries the figures', said(w, /comment:.*26.*4/));
  check('fix finalize: says the attempt was returned, so the count is auditable', said(w, /comment:.*returned/i));

  for (const [n, ok] of results) console.log((ok ? 'ok   ' : 'FAIL ') + n);
  console.log(`${results.filter(r => r[1]).length}/${results.length} cases passed`);
})();
