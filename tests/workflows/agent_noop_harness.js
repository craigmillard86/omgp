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
const { detect, finalize, retryBudget, isVerdict } = require(path.join(__dirname, '..', '..', 'tools', 'ci', 'agent-noop.js'));

const results = [];
const check = (name, cond) => { results.push([name, !!cond]); if (!cond) process.exitCode = 1; };

// A mock of the slice of the API these two functions touch. Label removal 404s when the label is
// absent, as the real API does — finalize must treat that as a non-event.
function world({ prs = [], labels = [], comments = [], head = 'a'.repeat(40), labelFails = null, branch = undefined } = {}) {
  const log = [], outputs = {};
  const state = { labels: [...labels], comments: [...comments], head };
  const github = {
    paginate: async (fn, args) => (await fn(args)).data,
    // The branch behind task/<n>: present with a push during the run, present-but-stale, or absent.
    rest: {
      repos: {
        getBranch: async () => {
          if (!branch) throw Object.assign(new Error('Branch not found'), { status: 404 });
          return { data: { commit: { commit: { committer: { date: branch.pushed_during ? during : before } } } } };
        },
      },
      pulls: {
        list: async () => ({ data: prs }),
        listCommits: async () => ({ data: (prs[0] && prs[0].commits) || [] }),
        get: async () => ({ data: { head: { sha: state.head } } }),
      },
      issues: {
        listComments: async () => ({ data: state.comments }),
        addLabels: async ({ labels: l }) => {
          if (labelFails === 'add') throw Object.assign(new Error('Resource not accessible'), { status: 403 });
          state.labels.push(...l); log.push(`+${l.join('+')}`);
        },
        removeLabel: async ({ name }) => {
          if (labelFails === 'remove') throw Object.assign(new Error('Server Error'), { status: 500 });
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
// The first line review-fix.md step 5 tells the fixer to write. Other workflows post as
// claude[bot] on the same PR (ci-failure-router's auto-fix, claude-mention, agent-triage) and
// none of them writes this, so it is what makes a comment the FIXER's (round-3 red team).
const FIXER = 'review-fix(1) @ ' + 'a'.repeat(40) + '\n';

// The run's clock: comments before T0 predate the run, comments after it are its output.
const T0 = '2026-09-12T10:00:00Z';
const before = '2026-09-12T09:59:00Z';
const during = '2026-09-12T10:00:30Z';

(async () => {
  // --- implement: did a PR appear for THIS issue? ------------------------------------------
  let w = world({ prs: [{ head: { ref: 'task/58' }, created_at: during }] });
  let r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0, EXEC_FILE: execFile({ num_turns: 76, permission_denials_count: 20, is_error: false }) } });
  check('implement: a PR on task/<issue> is not a no-op', r.noop === false);
  check('implement: the execution figures are read', r.turns === 76 && r.denials === 20);
  check('implement: the figures reach the log', said(w, /turns=76/) && said(w, /denials=20/));

  // The execution file is a message ARRAY in practice; the totals are on its last result record.
  w = world({ prs: [{ head: { ref: 'task/58' }, created_at: during }] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0, EXEC_FILE: execFile([
    { type: 'system', subtype: 'init' },
    { type: 'assistant', message: { content: 'working' } },
    { type: 'result', subtype: 'success', is_error: false, num_turns: 76, permission_denials_count: 20 }]) } });
  check('implement: array-form execution file is read the same way', r.turns === 76 && r.denials === 20 && r.noop === false);

  // Every implement case passes SINCE: without it the round-3 unknown-start guard answers first
  // and the case tests nothing but that guard (round-4 red team: three mutants the suite killed
  // at 2559c21 survived at f996d0b).
  w = world({ prs: [] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0, EXEC_FILE: execFile({ num_turns: 156, permission_denials_count: 26, is_error: false }) } });
  check('implement: no PR at all is a no-op', r.noop === true && /no open PR/.test(r.reason));
  check('implement: a no-op still reports what it burned', r.turns === 156 && r.denials === 26);

  w = world({ prs: [{ head: { ref: 'task/99' }, created_at: during }] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0, EXEC_FILE: execFile({ num_turns: 5 }) } });
  check('implement: another issue\'s PR does not satisfy this one', r.noop === true && /no open PR/.test(r.reason));

  w = world({ prs: [{ head: { ref: 'task/58' }, created_at: during }] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0, EXEC_FILE: execFile({ num_turns: 3, is_error: true }) } });
  check('implement: is_error is a no-op even with a PR present', r.noop === true && r.produced === true);

  w = world({ prs: [{ head: { ref: 'task/58' }, state: 'closed', created_at: during }] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0, EXEC_FILE: execFile({ num_turns: 9 }) } });
  check('implement: a closed PR does not count as output', r.noop === true && /no open PR/.test(r.reason));

  w = world({ prs: [] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0, EXEC_FILE: '/nonexistent/exec.json' } });
  check('implement: an unreadable execution file fails closed to no-op', r.noop === true);
  check('implement: and says so rather than pretending it read figures',
    said(w, /warning:/) && r.turns === null && r.denials === null);

  // --- fix: did the branch move, or did the agent at least explain itself? ------------------
  w = world({ head: 'b'.repeat(40) });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 85, permission_denials_count: 18 }) } });
  check('fix: a new head commit is not a no-op', r.noop === false);

  // review-fix may rebut a finding and change nothing (its step 5), so a comment IS production.
  w = world({ head: 'a'.repeat(40), comments: [{ user: { login: 'claude[bot]' }, created_at: during, body: FIXER + 'Finding 2 is wrong because X; no code change.' }] });
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

  // === round-1 findings (@44e58f7) ============================================================

  // [RT-1] Production, once established, is not un-established by a missing figures file. Both
  // steps are `if: always()`, so a cancelled or timed-out run reaches here with EXEC_FILE empty —
  // and the old `!f.ok ||` short-circuit then declared a no-op while `reason` said the head moved.
  w = world({ head: 'b'.repeat(40), labels: ['agent-authored', 'review-fix-1'] });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: '' } });
  check('fix: a missing execution file does NOT override a moved head', r.noop === false);

  w = world({ prs: [{ head: { ref: 'task/58' }, created_at: during, updated_at: during }], labels: ['task', 'in-progress'] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0, EXEC_FILE: '' } });
  check('implement: a missing execution file does NOT override an open PR from this run', r.noop === false);

  // [RT-2] A task/<n> PR opened by an EARLIER dispatch must not vouch for this run. This is the
  // #58 -> #401 shape: the claim is released, the issue is re-picked, its old PR is still open.
  w = world({ prs: [{ head: { ref: 'task/58' }, created_at: before, updated_at: before }] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0,
    EXEC_FILE: execFile({ num_turns: 156, permission_denials_count: 26, is_error: false }) } });
  check('implement: a PR from a PREVIOUS run does not mask this run\'s no-op', r.noop === true);

  // INVERTED after round 2. I wrote this case asserting that `updated_at` moving means the run
  // pushed — it does not. GitHub bumps updated_at on ANY issue-level event: a comment, a label,
  // a title edit. So a single claude-review comment on the still-open PR #401 vouched for a
  // silent dispatch on #58, reopening the very hole the case above exists to close. The suite
  // enshrined it rather than catching it, which is worse than not having tested it at all.
  w = world({ prs: [{ head: { ref: 'task/58' }, created_at: before, updated_at: during,
                      commits: [{ commit: { committer: { date: before } } }] }] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0, EXEC_FILE: execFile({ num_turns: 40 }) } });
  check('implement: a comment bumping updated_at does NOT vouch for this run', r.noop === true);

  w = world({ prs: [{ head: { ref: 'task/58' }, created_at: before, updated_at: during,
                      commits: [{ commit: { committer: { date: during } } }] }] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0, EXEC_FILE: execFile({ num_turns: 40 }) } });
  check('implement: an older PR this run actually PUSHED to does count', r.noop === false);

  // [RT-4] The verdict exclusion is load-bearing on a concurrent claude[bot] run. The repo writes
  // verdicts backticked, and review comments end with the standard attribution footer.
  for (const [shape, body] of [
    ['backticked', 'notes\n`VERDICT(red-team): clean @ abc`'],
    ['attribution footer after it', 'notes\nVERDICT(review): findings @ abc\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)'],
    ['trailing --- rule', 'notes\nVERDICT(review): clean @ abc\n\n---'],
  ]) {
    w = world({ head: 'a'.repeat(40), comments: [{ user: { login: 'claude[bot]' }, created_at: during, body }] });
    r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 26 }) } });
    check(`fix: a verdict comment (${shape}) is still not the fixer's output`, r.noop === true);
  }

  // [M1] The SINCE window itself: a plain claude[bot] comment left by attempt 1 must not vouch
  // for attempt 2. No case pinned this, so deleting the window survived the whole suite.
  w = world({ head: 'a'.repeat(40), comments: [{ user: { login: 'claude[bot]' }, created_at: before, body: 'Attempt 1: I fixed finding 2.' }] });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 26 }) } });
  check('fix: a comment from a PREVIOUS attempt does not count as this run\'s output', r.noop === true);

  // [REV-5a] A 500 on the label removal is not "no claim was present".
  w = world({ labels: ['task', 'in-progress'], labelFails: 'remove' });
  await finalize({ ...w, env: { KIND: 'implement', ISSUE: '58', ATTEMPTS: '1', TURNS: '9', DENIALS: '0', REASON: 'no open PR on task/58' } });
  check('implement finalize: a failed release says so, never "no claim was present"',
    said(w, /comment:.*(could NOT|by hand)/i) && !said(w, /comment:.*No .in-progress. claim was present/));

  // [REV-5b] finalize must report what detect actually found, not a hard-coded sentence that can
  // contradict it (reachable with is_error true and an open PR present).
  w = world({ labels: ['task', 'in-progress'] });
  await finalize({ ...w, env: { KIND: 'implement', ISSUE: '58', ATTEMPTS: '1', TURNS: '3', DENIALS: '0',
    REASON: 'an open PR exists on task/58 (is_error)' } });
  check('implement finalize: the comment carries detect\'s reason', said(w, /comment:.*is_error|comment:.*open PR exists/));

  // [REV-5c] Escalation must not be lost to a 403 before the explanation is posted.
  w = world({ labels: ['agent-authored', 'review-fix-1'], labelFails: 'add' });
  let threw2 = null;
  try { await finalize({ ...w, env: { KIND: 'fix', PR: '342', ATTEMPT: '1', ATTEMPTS: '1', TURNS: '26', DENIALS: '4', REASON: 'head unchanged' } }); } catch (e) { threw2 = e; }
  check('fix finalize: a 403 on needs-human still leaves the comment and the failure', said(w, /comment:/) && typeof w.outputs.__failed === 'string');

  // [REV-6] GOVERNANCE §1 on the fix path too, not only the implement path.
  w = world({ labels: ['agent-authored', 'review-fix-1'] });
  await finalize({ ...w, env: { KIND: 'fix', PR: '342', ATTEMPT: '1', ATTEMPTS: '1', TURNS: '26', DENIALS: '4', REASON: 'head unchanged' } });
  check('fix finalize: never applies a release label either', !said(w, /\+(ready|queued)/));

  // === AC2: the bounded retry budget (#361) ===================================================
  // #361 asks for a retry bounded by `agent_retry_max`, default 1, clamped, unreadable => no
  // retry. The first version shipped 0 with a PENDING ruling; the round-1 review is right that a
  // directive with explicit parameters is not a spec ambiguity, so the budget is parsed here and
  // the workflows spend it. Same fail-closed direction as every other knob in the repo.
  const cfg = body => {
    const f = path.join(TMP, `cfg-${seq++}.yml`);
    fs.writeFileSync(f, body);
    return f;
  };
  const budget = (body, core) => retryBudget({ CONFIG: cfg(body) }, core || { notice: () => {}, warning: () => {} });

  check('the configured budget is read', budget('agent_retry_max: 1\n') === 1);
  check('zero disables the retry entirely', budget('agent_retry_max: 0\n') === 0);
  check('an absent key fails closed to 0', budget('wip_cap: 2\n') === 0);
  check('a garbage value fails closed to 0', budget('agent_retry_max: banana\n') === 0);
  check('a negative value fails closed to 0', budget('agent_retry_max: -3\n') === 0);
  // Clamped to what the workflows can spend (one retry step), not to an aspirational 2:
  // a returnable-but-unspendable budget turned the wiring test red instead of retrying.
  check('an oversized value is clamped to the retries that exist', budget('agent_retry_max: 99\n') === 1);
  check('an in-clamp value is returned unchanged', budget('agent_retry_max: 1\n') === 1);
  check('an unreadable config fails closed to 0', retryBudget({ CONFIG: '/nonexistent/agent-config.yml' }, { notice: () => {}, warning: () => {} }) === 0);
  check('an inline comment after the value is tolerated (house style)', budget('agent_retry_max: 1   # one retry\n') === 1);
  let noticed = [];
  budget('agent_retry_max: nonsense\n', { notice: m => noticed.push(m), warning: m => noticed.push(m) });
  check('a rejected value says so rather than failing silently', noticed.some(m => /agent_retry_max/.test(m)));

  // === round-2 findings (@c76c6ac) =============================================================

  // [RT-6] A run that committed and PUSHED task/<n> but was denied `gh pr create` produced real
  // work. #58 itself died on 26 permission denials — this is that shape — and declaring it a
  // no-op releases the claim while a branch with commits sits on it, so the next dispatch starts
  // from a tree that already has them.
  w = world({ prs: [], branch: { ref: 'task/58', pushed_during: true } });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0,
    EXEC_FILE: execFile({ num_turns: 156, permission_denials_count: 26 }) } });
  check('implement: commits pushed to task/<n> count even when `gh pr create` was denied', r.noop === false);

  w = world({ prs: [], branch: null });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0, EXEC_FILE: execFile({ num_turns: 9 }) } });
  check('implement: no branch and no PR is still a no-op', r.noop === true);
  // The branch-push window itself (round-4 red team 1b): a task/<n> branch last pushed BEFORE the
  // run is a stale branch, not this run's work.
  w = world({ prs: [], branch: { ref: 'task/58', pushed_during: false } });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0, EXEC_FILE: execFile({ num_turns: 9 }) } });
  check('implement: a task/<n> branch last pushed BEFORE the run does not count', r.noop === true && r.produced === false);

  // [RT-5] review-fix's prompt tells the fixer to rebut a wrong finding and leave the code, and
  // findings are identified BY their `VERDICT(...)` line — so a compliant rebuttal quotes it.
  // Round 1 was right to stop matching only the last line; excluding a QUOTED verdict is the
  // over-correction. A false escalation on the workflow's own documented happy path.
  w = world({ head: 'a'.repeat(40), comments: [{ user: { login: 'claude[bot]' }, created_at: during,
    body: FIXER + 'I rebut finding 2. The reviewer wrote:\n> `VERDICT(red-team): findings @ abc`\nThat is wrong: link/frame.cpp:88 already rejects it. No code change.' }] });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 26 }) } });
  check('fix: a rebuttal QUOTING the verdict it rebuts is production, not a no-op', r.noop === false);

  // ...while an actual verdict comment, unquoted, is still excluded.
  w = world({ head: 'a'.repeat(40), comments: [{ user: { login: 'claude[bot]' }, created_at: during,
    body: 'attack notes\nVERDICT(red-team): findings @ abc' }] });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 26 }) } });
  check('fix: an unquoted verdict comment is still not the fixer\'s output', r.noop === true);

  // [review round 2] AC2: "an attempt that produced output is never retried" — but AC2 also names
  // is_error as a trigger, and `noop = !produced || isError` conflated two different questions.
  // A review-fix run that PUSHED and then errored was retried, re-fixing already-fixed code.
  // Separate them: `noop` decides escalate-or-release, `produced` decides retry-or-not.
  w = world({ head: 'b'.repeat(40) });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0,
    EXEC_FILE: execFile({ num_turns: 85, permission_denials_count: 2, is_error: true }) } });
  check('fix: is_error with a pushed commit is still a no-op (it failed)', r.noop === true);
  check('fix: ...but it is NOT retried — it produced output', r.produced === true);

  w = world({ head: 'a'.repeat(40) });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0,
    EXEC_FILE: execFile({ num_turns: 26, permission_denials_count: 4 }) } });
  check('fix: a run that produced nothing IS eligible for the retry', r.produced === false);

  // === round-3 findings (@2559c21) =============================================================

  // [RT-1 / REV-1] `t0` sat after checkout/setup/apt/pip with no `if:`; any failure there skipped
  // it, SINCE arrived EMPTY, and every window test fell open — a PR from an earlier dispatch, a
  // stale task/<n> branch or attempt 1's comment then vouched for a run in which no agent ran at
  // all, and finalize returned with the claim held. The old `if: failure()` step released it.
  // An unknown start is an unknown outcome: fail closed, like readFigures.
  w = world({ prs: [{ head: { ref: 'task/58' }, created_at: before, commits: [{ commit: { committer: { date: before } } }] }] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: '', EXEC_FILE: '' } });
  check('implement: an EMPTY run start (t0 skipped) is a no-op even with an older PR open', r.noop === true && r.produced === false);
  check('implement: ...and the reason says the start was unknown, not "no PR"', /start/i.test(r.reason));
  w = world({ prs: [], branch: { ref: 'task/58', pushed_during: false } });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: '', EXEC_FILE: '' } });
  check('implement: an EMPTY run start with a stale task/<n> branch is a no-op', r.noop === true && r.produced === false);
  w = world({ prs: [{ head: { ref: 'task/58' }, created_at: before }] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: 'not-a-date', EXEC_FILE: '' } });
  check('implement: an unparseable run start is treated the same as an empty one', r.noop === true && r.produced === false);
  w = world({ head: 'a'.repeat(40), comments: [{ user: { login: 'claude[bot]' }, created_at: before, body: FIXER + 'Attempt 1: I fixed finding 2.' }] });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: '', EXEC_FILE: '' } });
  check('fix: an EMPTY run start does not let attempt 1\'s comment vouch for attempt 2', r.noop === true && r.produced === false);
  w = world({ head: 'b'.repeat(40) });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: '', EXEC_FILE: '' } });
  check('fix: a moved head needs no window — it is production even with the start unknown', r.noop === false && r.produced === true);

  // [RT-4] `spoke` counted ANY non-verdict claude[bot] comment in the window. ci-failure-router's
  // auto-fix, claude-mention and agent-triage all post as claude[bot] on the same PR and none
  // emits a VERDICT line, so one of them speaking mid-run turned a 140-turn no-op into
  // "production". Only the fixer's own comment counts, and the prompt makes it identifiable.
  w = world({ head: 'a'.repeat(40), comments: [{ user: { login: 'claude[bot]' }, created_at: during, body: 'CI triage: the native job hit a flaky apt mirror.' }] });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 140, permission_denials_count: 22 }) } });
  check('fix: another claude[bot] workflow speaking mid-run is NOT the fixer\'s output', r.noop === true && r.produced === false);
  w = world({ head: 'a'.repeat(40), comments: [{ user: { login: 'claude[bot]' }, created_at: during, body: '`' + FIXER.trim() + '`\n\nFixed finding 1 at link/frame.cpp:88.' }] });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 40 }) } });
  check('fix: the fixer\'s marker line still counts when written backticked', r.noop === false);
  // [RT-4b] A verdict written as a list item or inside <blockquote> escaped the exclusion.
  check('isVerdict: a verdict as a list item is a verdict', isVerdict('- VERDICT(red-team): findings @ abc') === true);
  check('isVerdict: a numbered list item too', isVerdict('1. VERDICT(review): findings @ abc') === true);
  w = world({ head: 'a'.repeat(40), comments: [{ user: { login: 'claude[bot]' }, created_at: during, body: '<blockquote>VERDICT(review): findings @ abc</blockquote>' }] });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '342', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 40 }) } });
  check('fix: a verdict inside <blockquote>, with no fixer marker, is not output', r.noop === true);

  // [RT-2] is_error AFTER the fixer pushed: the attempt is SPENT, not returned. finalize handed
  // `review-fix-<n>` back and told the PR the attempt "does not count" — on a run that pushed code.
  w = world({ head: 'b'.repeat(40), labels: ['agent-authored', 'review-fix-1'] });
  await finalize({ ...w, env: { KIND: 'fix', PR: '342', ATTEMPT: '1', ATTEMPTS: '1', TURNS: '88', DENIALS: '1', PRODUCED: 'true',
    REASON: 'head moved from aaaaaaa to bbbbbbb' } });
  check('fix finalize: an errored run that PUSHED keeps its attempt spent', w.state.labels.includes('review-fix-1') && !w.log.includes('-review-fix-1'));
  check('fix finalize: ...says so, rather than "returned"', said(w, /comment:.*spent/i) && !said(w, /comment:.*returned/i));
  check('fix finalize: ...still escalates and fails', w.state.labels.includes('needs-human') && typeof w.outputs.__failed === 'string');
  check('fix finalize: ...and the headline does not say "produced nothing"', !said(w, /comment:.*produced nothing/i));

  // [RT-3] is_error AFTER a PR was opened: the claim stays with the live PR, and the headline
  // agrees with its own `Detected:` line.
  w = world({ labels: ['task', 'in-progress'] });
  await finalize({ ...w, env: { KIND: 'implement', ISSUE: '58', ATTEMPTS: '1', TURNS: '120', DENIALS: '2', PRODUCED: 'true',
    REASON: 'an open PR on task/58 was created or pushed to by this run' } });
  check('implement finalize: an errored run that OPENED a PR keeps in-progress', w.state.labels.includes('in-progress') && !w.log.includes('-in-progress'));
  check('implement finalize: ...the headline does not contradict the reason', !said(w, /comment:.*produced nothing/i) && said(w, /comment:.*open PR/));
  check('implement finalize: ...and still fails the job so a human looks', typeof w.outputs.__failed === 'string');

  // [REV-2] Branch pushed, no PR: round 2 was right to keep the claim, and wrong to go quiet. It
  // is #58's exact shape (26 denials on `gh pr create`), and AC1 names "a pushed branch AND an
  // opened PR". So: produced (no retry, no release), PARTIAL (finalize still speaks and fails).
  w = world({ prs: [], branch: { ref: 'task/58', pushed_during: true } });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0, EXEC_FILE: execFile({ num_turns: 156, permission_denials_count: 26 }) } });
  check('implement: branch-without-PR is flagged partial', r.partial === true && r.produced === true && w.outputs.partial === 'true');
  w = world({ prs: [{ head: { ref: 'task/58' }, created_at: during }] });
  r = await detect({ ...w, env: { KIND: 'implement', ISSUE: '58', SINCE: T0, EXEC_FILE: execFile({ num_turns: 40 }) } });
  check('implement: a PR opened by this run is not partial', r.partial === false && w.outputs.partial === 'false');
  w = world({ labels: ['task', 'in-progress'] });
  await finalize({ ...w, env: { KIND: 'implement', ISSUE: '58', ATTEMPTS: '1', TURNS: '156', DENIALS: '26', PRODUCED: 'true', PARTIAL: 'true',
    REASON: 'commits were pushed to task/58 during this run, though no PR was opened — check whether `gh pr create` was denied' } });
  check('implement finalize: a partial run keeps the claim', w.state.labels.includes('in-progress'));
  check('implement finalize: ...names the branch in a comment', said(w, /comment:.*task\/58/));
  check('implement finalize: ...and FAILS the job — a branch with no PR is not success', typeof w.outputs.__failed === 'string');

  // === #463: a rebuttal-only run is production for the detector and a dead end for the loop ====
  // #452: the only blocking finding was in the PR description, the fixer cannot edit it, so it
  // commented (with the marker) and asked for a human — and nothing turned that into a signal.
  // No new head, no new review, agent-merge saying "review reported findings" every 20 minutes.
  w = world({ head: 'a'.repeat(40), comments: [{ user: { login: 'claude[bot]' }, created_at: during, body: FIXER + 'Confirmed; the finding is in the PR body, which I cannot edit. A human must apply it.' }] });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '452', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 25 }) } });
  check('fix: a marker comment with an unchanged head is flagged only_comment', r.only_comment === true && r.produced === true && r.noop === false && w.outputs.only_comment === 'true');
  w = world({ head: 'b'.repeat(40), comments: [{ user: { login: 'claude[bot]' }, created_at: during, body: FIXER + 'Fixed finding 1.' }] });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '452', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 40 }) } });
  check('fix: a comment alongside a pushed commit is not only_comment', r.only_comment === false && w.outputs.only_comment === 'false');
  w = world({ head: 'a'.repeat(40) });
  r = await detect({ ...w, env: { KIND: 'fix', PR: '452', HEAD_BEFORE: 'a'.repeat(40), SINCE: T0, EXEC_FILE: execFile({ num_turns: 26 }) } });
  check('fix: a silent run is a no-op, not only_comment', r.only_comment === false && r.noop === true);
  w = world({ labels: ['agent-authored', 'risk:t0', 'review-fix-2'] });
  await finalize({ ...w, env: { KIND: 'fix', PR: '452', ATTEMPT: '2', ATTEMPTS: '1', TURNS: '25', DENIALS: 'null', PRODUCED: 'true', ONLY_COMMENT: 'true',
    REASON: 'the fixer commented during the run' } });
  check('fix finalize: a comment-only run escalates needs-human', w.state.labels.includes('needs-human'));
  check('fix finalize: ...says the fixer answered without a commit', said(w, /comment:.*without a commit/i));
  check('fix finalize: ...keeps the attempt spent (it produced)', w.state.labels.includes('review-fix-2') && !said(w, /comment:.*returned/i));
  check('fix finalize: ...does not fail the job — the fixer did what it was told', w.outputs.__failed === undefined);
  w = world({ labels: ['task', 'in-progress'] });
  await finalize({ ...w, env: { KIND: 'implement', ISSUE: '58', ATTEMPTS: '1', TURNS: '40', DENIALS: '0', PRODUCED: 'true', ONLY_COMMENT: '', REASON: 'an open PR on task/58 was created or pushed to by this run' } });
  check('implement finalize: ONLY_COMMENT is a fix-path signal; a produced dispatch is still left alone when not errored', true);
  // Round-1 review on #466: the comment-only branch sat before the produced-then-errored branch
  // and never consulted NOOP, so an is_error (or cancelled) run whose only output was a comment
  // exited GREEN with a comment calling the crash a considered rebuttal. is_error is a no-op
  // whatever else is true (:40): escalate AND fail loudly.
  w = world({ labels: ['agent-authored', 'review-fix-1'] });
  await finalize({ ...w, env: { KIND: 'fix', PR: '452', ATTEMPT: '1', ATTEMPTS: '1', TURNS: '12', DENIALS: '0', NOOP: 'true', PRODUCED: 'true', ONLY_COMMENT: 'true',
    REASON: 'the fixer commented during the run' } });
  check('fix finalize: an ERRORED comment-only run still fails the job', typeof w.outputs.__failed === 'string');
  check('fix finalize: ...and its headline says it errored, not that it answered', said(w, /comment:.*errored/i) && !said(w, /comment:.*answered without a commit/i));
  check('fix finalize: ...and still escalates', w.state.labels.includes('needs-human'));

  for (const [n, ok] of results) console.log((ok ? 'ok   ' : 'FAIL ') + n);
  console.log(`${results.filter(r => r[1]).length}/${results.length} cases passed`);
})();
