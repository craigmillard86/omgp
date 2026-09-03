// Executes the `route` github-script body of .github/workflows/ci-failure-router.yml VERBATIM
// against a mocked GitHub API, so the routing rules (GOVERNANCE.md §4 "CI-failure
// auto-resolution") are tested on every push instead of only on real failed runs. Driven by
// tools/refimpl/test_workflow_scripts.py, which extracts the script into a JSON file (argv[2]).
//
// Each case builds a small world (open PRs, issues, the failed run's jobs, prior failed runs),
// runs the script with a synthetic workflow_run payload, and asserts on the API calls it made
// and the `route` output it set. Exit code 1 on any failure; failing case names are printed.
'use strict';
const fs = require('fs');
const S = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));

const REPO = 'o/r';
function world({prs = [], issues = [], jobs = [], failedRuns = [], comments = {}, failRemove = null} = {}) {
  const log = [];
  const outputs = {};
  const prState = new Map(prs.map(p => [p.number, p]));
  const issueState = new Map(issues.map(i => [i.number, i]));
  const labelsOf = n => (prState.get(n) || issueState.get(n) || {labels: []}).labels;
  const github = {
    paginate: async (fn, args) => fn(args).then(r => r.data),
    rest: {
      pulls: {
        list: async ({head, state}) => ({data: [...prState.values()].filter(p => p.state === (state || 'open') && `o:${p.head}` === head)}),
      },
      issues: {
        get: async ({issue_number}) => { const i = issueState.get(issue_number); if (!i) throw Object.assign(new Error('Not Found'), {status: 404}); return {data: i}; },
        listForRepo: async ({labels, state}) => ({data: [...issueState.values()].filter(i => i.state === (state || 'open') && (labels || '').split(',').filter(Boolean).every(l => i.labels.some(x => x.name === l)))}),
        listComments: async ({issue_number}) => ({data: (comments[issue_number] || []).map(body => ({body}))}),
        addLabels: async ({issue_number, labels}) => { for (const l of labels) labelsOf(issue_number).push({name: l}); log.push(`+${labels.join('+')}@${issue_number}`); },
        removeLabel: async ({issue_number, name}) => {
          if (failRemove === issue_number) throw Object.assign(new Error('Server Error'), {status: 500});
          const t = prState.get(issue_number) || issueState.get(issue_number);
          if (!t || !t.labels.some(l => l.name === name)) throw Object.assign(new Error('Not Found'), {status: 404});
          t.labels = t.labels.filter(l => l.name !== name); log.push(`-${name}@${issue_number}`);
        },
        createComment: async ({issue_number, body}) => log.push(`comment@${issue_number}: ${body.replace(/\n+/g, ' | ').slice(0, 700)}`),
        create: async ({title, labels, body}) => { const n = 900 + issueState.size; issueState.set(n, {number: n, state: 'open', title, labels: labels.map(name => ({name})), body}); log.push(`create#${n}: ${title} [${labels.join(',')}]`); return {data: {number: n, html_url: `u/${n}`}}; },
      },
      actions: {
        listJobsForWorkflowRun: async () => ({data: jobs}),
        listWorkflowRunsForRepo: async ({branch, status}) => ({data: failedRuns.filter(r => r.head_branch === branch && (!status || r.conclusion === status))}),
      },
    },
  };
  const core = {
    info: () => {}, notice: m => log.push(`notice: ${m}`), warning: m => log.push(`warning: ${m}`),
    setOutput: (k, v) => { outputs[k] = String(v); log.push(`out ${k}=${v}`); },
  };
  return {github, core, log, outputs, prState, issueState};
}
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
async function route(w, run, maxAttempts = '4') {
  process.env.MAX_ATTEMPTS = maxAttempts;
  const workflow_run = {
    id: 5001, name: 'ci', conclusion: 'failure', head_branch: 'task/26', head_sha: 'abc123', run_attempt: 1,
    html_url: 'https://gh/o/r/actions/runs/5001', head_repository: {full_name: REPO}, ...run,
  };
  const context = {repo: {owner: 'o', repo: 'r'}, eventName: 'workflow_run', runId: 7777, serverUrl: 'https://gh', payload: {workflow_run}};
  await new AsyncFunction('github', 'context', 'core', 'require', S.route)(w.github, context, w.core, require);
}
const pr = (number, head, labels, body = '') => ({number, state: 'open', head, labels: labels.map(name => ({name})), html_url: `u/pr/${number}`, body});
const issue = (number, labels, title = `T00${number}`) => ({number, state: 'open', title, labels: labels.map(name => ({name}))});
const JOBS = [{name: 'native (build + full test suite)', conclusion: 'success', steps: []},
              {name: 'deep-verify (T2/T3 only)', conclusion: 'failure', steps: [{name: 'Set up job', conclusion: 'success'}, {name: 'Diff-scoped mutation', conclusion: 'failure'}]}];
const results = [];
const check = (name, cond) => { results.push([name, !!cond]); if (!cond) process.exitCode = 1; };
const has = (w, s) => w.log.includes(s);
const said = (w, re, n) => w.log.some(l => re.test(l) && (n === undefined || l.startsWith(`comment@${n}`)));
const labels = (w, n) => (w.prState.get(n) || w.issueState.get(n)).labels.map(l => l.name);

(async () => {
  let w;
  // --- (a) agent branch: bounded auto-fix ---
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'risk:t2'])], issues: [issue(26, ['task', 'in-progress'])], jobs: JOBS});
  await route(w, {});
  check('agent PR, no prior attempts -> auto-fix-1 + route autofix', w.outputs.route === 'autofix' && labels(w, 94).includes('auto-fix-1') && w.outputs.pr === '94' && w.outputs.attempt === '1');
  check('attempt comment names the failed job and step and carries the sha marker', said(w, /ci-failure-router sha=abc123/, 94) && said(w, /deep-verify.*Diff-scoped mutation/, 94) && said(w, /attempt 1 of 4/, 94));
  check('attempt path touches no issue labels', !w.log.some(l => /@26$/.test(l)));

  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1'])], jobs: JOBS});
  await route(w, {head_sha: 'def456'});
  check('one prior attempt -> auto-fix-2 + route autofix', w.outputs.route === 'autofix' && labels(w, 94).includes('auto-fix-2') && w.outputs.attempt === '2');

  const runs = [{id: 5001, name: 'ci', head_branch: 'task/26', conclusion: 'failure', html_url: 'https://gh/r/5001'},
                {id: 4900, name: 'ci', head_branch: 'task/26', conclusion: 'failure', html_url: 'https://gh/r/4900'},
                {id: 4800, name: 'ci', head_branch: 'task/26', conclusion: 'failure', html_url: 'https://gh/r/4800'},
                {id: 4750, name: 'security', head_branch: 'task/26', conclusion: 'failure', html_url: 'https://gh/r/4750'},
                {id: 4700, name: 'ci', head_branch: 'task/99', conclusion: 'failure', html_url: 'https://gh/r/4700'}];
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'in-progress'])], issues: [issue(26, ['task', 'in-progress', 'agent-authored'])], jobs: JOBS, failedRuns: runs});
  await route(w, {head_sha: 'aaa999'});
  check('two prior attempts, bound 4 -> attempt 3, not exhaustion (ruling 2026-09-03)', w.outputs.route === 'autofix' && labels(w, 94).includes('auto-fix-3') && w.outputs.attempt === '3');
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'auto-fix-3'])], jobs: JOBS});
  await route(w, {head_sha: 'aaa998'});
  check('three prior attempts -> attempt 4 (the last)', w.outputs.route === 'autofix' && labels(w, 94).includes('auto-fix-4') && w.outputs.attempt === '4');
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'auto-fix-3', 'auto-fix-4', 'in-progress'])], issues: [issue(26, ['task', 'in-progress', 'agent-authored'])], jobs: JOBS, failedRuns: runs});
  await route(w, {head_sha: 'aaa999'});
  check('four prior attempts -> exhausted: needs-human on PR, in-progress released on PR and task issue', w.outputs.route === 'exhausted' && labels(w, 94).includes('needs-human') && !labels(w, 94).includes('in-progress') && !labels(w, 26).includes('in-progress') && labels(w, 26).includes('needs-human'));
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'in-progress'])], issues: [issue(26, ['task', 'in-progress', 'agent-authored'])], jobs: JOBS, failedRuns: runs});
  await route(w, {head_sha: 'aaa997'}, '2');
  check('auto_fix_max_attempts=2 restores the original bound exactly', w.outputs.route === 'exhausted' && labels(w, 94).includes('needs-human'));
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'in-progress'])], issues: [issue(26, ['task', 'in-progress', 'agent-authored'])], jobs: JOBS, failedRuns: runs});
  await route(w, {head_sha: 'aaa996'}, '');
  check('unreadable bound -> conservative 2 (fail closed): exhausted at two priors', w.outputs.route === 'exhausted' && w.log.some(l => /notice: .*auto_fix_max_attempts/.test(l)));
  check('exhausted comment links the failed runs on this branch (not other branches)', said(w, /exhausted.*after 4 attempts|exhausted/, 94) && said(w, /gh\/r\/5001/, 94) && said(w, /gh\/r\/4900/, 94) && !said(w, /gh\/r\/4700/, 94));
  // 2026-09-03 (maintainer report on #118): the run list silently truncated at 3, presenting
  // 3 of 4 failed runs as if complete. ALL of this branch's failed runs must appear (a
  // security failure included), up to a sane cap with an explicit "and N more" tail beyond it.
  check('exhausted comment lists ALL four failed runs on the branch', said(w, /gh\/r\/4800/, 94) && said(w, /gh\/r\/4750/, 94));
  const many = Array.from({length: 14}, (_, k) => ({id: 6000 + k, name: 'ci', head_branch: 'task/26', conclusion: 'failure', html_url: `https://gh/r/${6000 + k}`}));
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'auto-fix-3', 'auto-fix-4'])], issues: [issue(26, ['task'])], jobs: JOBS, failedRuns: many});
  await route(w, {head_sha: 'eee444'});
  check('more than ten failed runs -> ten listed plus an explicit "and N more" tail', said(w, /gh\/r\/6009/, 94) && !said(w, /gh\/r\/6010/, 94) && said(w, /and 4 more failed run/, 94));
  check('exhausted path does not add another attempt label', !labels(w, 94).includes('auto-fix-5') && !w.log.some(l => /^\+auto-fix/.test(l)));

  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'auto-fix-3', 'auto-fix-4', 'needs-human'])], jobs: JOBS, failedRuns: runs});
  await route(w, {head_sha: 'bbb000'});
  check('already needs-human -> no-op', w.outputs.route === 'none' && !w.log.some(l => l.startsWith('comment@')) && !w.log.some(l => /^[+-]/.test(l)));

  // --- idempotency per commit: a second failed workflow on the same sha is not a second attempt ---
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1'])], jobs: JOBS, comments: {94: ['<!-- ci-failure-router sha=abc123 pass=1 run=5001 -->\n🔧 earlier']}});
  await route(w, {id: 5002, name: 'security'});
  check('same sha already handled -> no second attempt (security after ci)', w.outputs.route === 'none' && !labels(w, 94).includes('auto-fix-2') && !w.log.some(l => l.startsWith('comment@')));

  // --- task/* branch whose PR is not agent-authored is human work ---
  w = world({prs: [pr(50, 'task/26', ['human-authored'])], jobs: JOBS});
  await route(w, {});
  check('task/* branch without agent-authored -> human path (comment only)', w.outputs.route === 'human' && said(w, /Diff-scoped mutation/, 50) && !w.log.some(l => /^[+-]/.test(l)));

  // --- (b) main ---
  w = world({jobs: JOBS});
  await route(w, {head_branch: 'main', head_sha: 'm1'});
  check('main -> ci-failure + task issue titled after workflow and run', w.outputs.route === 'main' && has(w, 'create#900: CI failure on main: ci run 5001 [ci-failure,task]'));
  check('main issue body links the run and names the failed step', [...w.issueState.values()].some(i => /actions\/runs\/5001/.test(i.body) && /Diff-scoped mutation/.test(i.body)));
  w = world({issues: [issue(80, ['ci-failure', 'task'], 'CI failure on main: ci run 4000')], jobs: JOBS});
  await route(w, {head_branch: 'main', head_sha: 'm2'});
  check('main dedupe: open ci-failure issue for the same workflow -> skipped', w.outputs.route === 'main' && !w.log.some(l => l.startsWith('create#')) && w.log.some(l => /notice:.*#80/.test(l)));
  w = world({issues: [issue(80, ['ci-failure', 'task'], 'CI failure on main: ci run 4000')], jobs: JOBS});
  await route(w, {head_branch: 'main', head_sha: 'm3', name: 'security', id: 6000});
  check('main dedupe is per workflow: security failure still files its own issue', has(w, 'create#901: CI failure on main: security run 6000 [ci-failure,task]'));

  // --- (c) human branches ---
  w = world({prs: [pr(60, 'ci/thing', ['risk:t3'])], jobs: JOBS});
  await route(w, {head_branch: 'ci/thing', head_sha: 'h1'});
  check('human branch with PR -> one short comment naming the first failed step, no labels', w.outputs.route === 'human' && said(w, /deep-verify.*Diff-scoped mutation/, 60) && !w.log.some(l => /^[+-]/.test(l)));
  w = world({prs: [pr(60, 'ci/thing', [])], jobs: JOBS, comments: {60: ['<!-- ci-failure-router sha=h1 pass=1 run=5001 -->\nℹ️ earlier']}});
  await route(w, {head_branch: 'ci/thing', head_sha: 'h1', id: 5002, name: 'security'});
  check('human branch: second failed workflow on the same sha -> no second comment', w.outputs.route === 'none' && !w.log.some(l => l.startsWith('comment@')));
  w = world({jobs: JOBS});
  await route(w, {head_branch: 'wip/no-pr', head_sha: 'h2'});
  check('human branch without a PR -> nothing at all', w.outputs.route === 'none' && !w.log.some(l => l.startsWith('comment@') || l.startsWith('create#') || /^[+-]/.test(l)));

  // --- guards ---
  w = world({prs: [pr(94, 'task/26', ['agent-authored'])], jobs: JOBS});
  await route(w, {head_repository: {full_name: 'someone/fork'}});
  check('fork head repository -> nothing (no secrets path for forks)', w.outputs.route === 'none' && !w.log.some(l => /^[+-]/.test(l) || l.startsWith('comment@')));
  w = world({prs: [pr(94, 'task/26', ['agent-authored'])], jobs: JOBS});
  await route(w, {conclusion: 'success'});
  check('non-failure conclusion -> nothing', w.outputs.route === 'none' && w.log.filter(l => !l.startsWith('out ')).every(l => l.startsWith('notice:')));
  w = world({prs: [pr(94, 'task/26', ['agent-authored'])], jobs: [{name: 'x', conclusion: 'cancelled', steps: []}]});
  await route(w, {});
  check('no failed job in the run -> still routes, says so', w.outputs.route === 'autofix' && said(w, /no failed job reported/, 94));

  // --- red-team findings on #96 (2026-08-30) ---
  // F1: a rerun of the same commit (gh run rerun after an "environmental" diagnosis) is a new pass
  //     of the same run id and sha — it must count as the next attempt, not be deduped away.
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1'])], jobs: JOBS, comments: {94: ['<!-- ci-failure-router sha=abc123 pass=1 run=5001 -->\n🔧 attempt 1: environmental, re-running']}});
  await route(w, {run_attempt: 2});
  check('F1: rerun of the same commit fails again -> counts as attempt 2', w.outputs.route === 'autofix' && labels(w, 94).includes('auto-fix-2') && said(w, /pass=2/, 94));
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'auto-fix-3', 'auto-fix-4', 'in-progress'])], issues: [issue(26, ['task', 'in-progress'])], jobs: JOBS, failedRuns: runs,
             comments: {94: ['<!-- ci-failure-router sha=abc123 pass=1 run=5001 -->', '<!-- ci-failure-router sha=abc123 pass=2 run=5001 -->']}});
  await route(w, {run_attempt: 3});
  check('F1: third pass on the same commit -> exhausted, not swallowed', w.outputs.route === 'exhausted' && labels(w, 94).includes('needs-human'));
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1'])], jobs: JOBS, comments: {94: ['<!-- ci-failure-router sha=abc123 pass=1 run=5001 -->']}});
  await route(w, {id: 5002, name: 'security', run_attempt: 1});
  check('F1: a different workflow failing on the same commit and pass is still one attempt', w.outputs.route === 'none');
  // F2: a non-404 failure to release the claim is reported, never claimed as done.
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'auto-fix-3', 'auto-fix-4', 'in-progress'])], issues: [issue(26, ['task', 'in-progress'])], jobs: JOBS, failedRuns: runs, failRemove: 26});
  await route(w, {head_sha: 'ccc111'});
  check('F2: removeLabel 500 on the issue -> comment says it could NOT release it, names the issue', w.outputs.route === 'exhausted' && said(w, /could NOT release `in-progress` on #26/, 94) && !said(w, /released on #26/, 94));
  check('F2: the PR side still got needs-human and its own in-progress released', labels(w, 94).includes('needs-human') && !labels(w, 94).includes('in-progress'));
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'auto-fix-3', 'auto-fix-4'])], issues: [issue(26, ['task'])], jobs: JOBS, failedRuns: runs});
  await route(w, {head_sha: 'ccc222'});
  check('F2: label already absent (404) is not a failure', w.outputs.route === 'exhausted' && !said(w, /could NOT/, 94) && labels(w, 26).includes('needs-human'));
  // F3: escalation targets the issues the PR actually closes, not the branch name's digits.
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'auto-fix-3', 'auto-fix-4'], 'Body.\n\nCloses #40\nFixes #41')], issues: [issue(26, ['task', 'in-progress']), issue(40, ['task', 'in-progress']), issue(41, ['task'])], jobs: JOBS, failedRuns: runs});
  await route(w, {head_sha: 'ddd333'});
  check('F3: Closes/Fixes targets get needs-human and in-progress released; branch-digit issue untouched', labels(w, 40).includes('needs-human') && !labels(w, 40).includes('in-progress') && labels(w, 41).includes('needs-human') && !labels(w, 26).includes('needs-human') && labels(w, 26).includes('in-progress'));
  check('F3: comment names the actual issues', said(w, /#40/, 94) && said(w, /#41/, 94));

  for (const [n, ok] of results) console.log((ok ? 'ok   ' : 'FAIL ') + n);
  console.log(`${results.filter(r => r[1]).length}/${results.length} cases passed`);
})();
