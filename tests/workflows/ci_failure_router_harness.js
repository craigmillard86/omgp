// Executes the `route` AND `sweep` github-script bodies of .github/workflows/ci-failure-router.yml
// VERBATIM against a mocked GitHub API, so the routing rules (GOVERNANCE.md §4 "CI-failure
// auto-resolution") are tested on every push instead of only on real failed runs. Driven by
// tools/refimpl/test_workflow_scripts.py, which extracts the scripts into a JSON file (argv[2]).
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
    // Like real octokit paginate: unwrap list-shaped responses ({workflow_runs: [...]}) to the array.
    paginate: async (fn, args) => fn(args).then(r => Array.isArray(r.data) ? r.data : r.data.workflow_runs),
    rest: {
      pulls: {
        // No `head` filter (the sweep's whole-repo listing) returns every open PR.
        list: async ({head, state}) => ({data: [...prState.values()].filter(p => p.state === (state || 'open') && (!head || `o:${p.head.ref}` === head))}),
      },
      issues: {
        get: async ({issue_number}) => { const i = issueState.get(issue_number); if (!i) throw Object.assign(new Error('Not Found'), {status: 404}); return {data: i}; },
        listForRepo: async ({labels, state}) => ({data: [...issueState.values()].filter(i => i.state === (state || 'open') && (labels || '').split(',').filter(Boolean).every(l => i.labels.some(x => x.name === l)))}),
        listComments: async ({issue_number}) => ({data: (comments[issue_number] || []).map(body => ({body}))}),
        // GitHub labels are a SET: re-adding an existing one is a no-op. Fidelity here is
        // load-bearing (red-team on #120 F5): the push-duplicates version could not observe
        // the unreachable-bound trap of a non-prefix label set.
        addLabels: async ({issue_number, labels}) => { const c = labelsOf(issue_number); for (const l of labels) if (!c.some(x => x.name === l)) c.push({name: l}); log.push(`+${labels.join('+')}@${issue_number}`); },
        removeLabel: async ({issue_number, name}) => {
          if (failRemove === issue_number) throw Object.assign(new Error('Server Error'), {status: 500});
          const t = prState.get(issue_number) || issueState.get(issue_number);
          if (!t || !t.labels.some(l => l.name === name)) throw Object.assign(new Error('Not Found'), {status: 404});
          t.labels = t.labels.filter(l => l.name !== name); log.push(`-${name}@${issue_number}`);
        },
        createComment: async ({issue_number, body}) => log.push(`comment@${issue_number}: ${body.replace(/\n+/g, ' | ').slice(0, 2000)}`),
        create: async ({title, labels, body}) => { const n = 900 + issueState.size; issueState.set(n, {number: n, state: 'open', title, labels: labels.map(name => ({name})), body}); log.push(`create#${n}: ${title} [${labels.join(',')}]`); return {data: {number: n, html_url: `u/${n}`}}; },
      },
      actions: {
        listJobsForWorkflowRun: async () => ({data: jobs}),
        // Real response shape: {total_count, workflow_runs} honouring per_page — the exhaustion
        // path reads it directly (one call, no pagination); the sweep goes via paginate.
        // head_sha honoured too (review round 3 on #120): the sweep pins its query to the PR
        // head so a stale head's failure is never re-delivered — the mock must be able to
        // observe that pin, not silently ignore it.
        listWorkflowRunsForRepo: async ({branch, status, head_sha, per_page}) => {
          const all = failedRuns.filter(r => r.head_branch === branch && (!status || r.conclusion === status) && (!head_sha || r.head_sha === head_sha));
          return {data: {total_count: all.length, workflow_runs: all.slice(0, per_page || 30)}};
        },
        createWorkflowDispatch: async ({inputs, ref}) => log.push(`dispatch run=${inputs.run_id} ref=${ref}`),
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
async function sweep(w) {
  const context = {repo: {owner: 'o', repo: 'r'}, eventName: 'schedule', payload: {repository: {default_branch: 'main'}}};
  await new AsyncFunction('github', 'context', 'core', 'require', S.sweep)(w.github, context, w.core, require);
}
// `head` carries the real nested shape (ref/sha/repo) — the sweep reads it; `route` never does.
const pr = (number, head, labels, body = '') => ({number, state: 'open', head: {ref: head, sha: 'aaa111', repo: {full_name: REPO}}, labels: labels.map(name => ({name})), html_url: `u/pr/${number}`, body});
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

  const runs = [{id: 5001, name: 'ci', head_branch: 'task/26', status: 'completed', conclusion: 'failure', html_url: 'https://gh/r/5001'},
                {id: 4900, name: 'ci', head_branch: 'task/26', status: 'completed', conclusion: 'failure', html_url: 'https://gh/r/4900'},
                {id: 4800, name: 'ci', head_branch: 'task/26', status: 'completed', conclusion: 'failure', html_url: 'https://gh/r/4800'},
                {id: 4750, name: 'security', head_branch: 'task/26', status: 'completed', conclusion: 'failure', html_url: 'https://gh/r/4750'},
                {id: 4700, name: 'ci', head_branch: 'task/99', status: 'completed', conclusion: 'failure', html_url: 'https://gh/r/4700'}];
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
  // (The dead `after 4 attempts` alternation was dropped here — review round 3 on #120; the
  // F3 case below pins the attempts/bound wording verbatim, so no coverage moved.)
  check('exhausted comment links the failed runs on this branch (not other branches)', said(w, /exhausted/, 94) && said(w, /gh\/r\/5001/, 94) && said(w, /gh\/r\/4900/, 94) && !said(w, /gh\/r\/4700/, 94));
  // 2026-09-03 (maintainer report on #118): the run list silently truncated at 3, presenting
  // 3 of 4 failed runs as if complete. ALL of this branch's failed runs must appear (a
  // security failure included), up to a sane cap with an explicit "and N more" tail beyond it.
  check('exhausted comment lists ALL four failed runs on the branch', said(w, /gh\/r\/4800/, 94) && said(w, /gh\/r\/4750/, 94));
  const many = Array.from({length: 14}, (_, k) => ({id: 6000 + k, name: 'ci', head_branch: 'task/26', status: 'completed', conclusion: 'failure', html_url: `https://gh/r/${6000 + k}`}));
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'auto-fix-3', 'auto-fix-4'])], issues: [issue(26, ['task'])], jobs: JOBS, failedRuns: many});
  await route(w, {head_sha: 'eee444'});
  check('more than ten failed runs -> ten listed plus an explicit "and N more" tail', said(w, /gh\/r\/6009/, 94) && !said(w, /gh\/r\/6010/, 94) && said(w, /and 4 more non-successful run/, 94));
  check('exhausted path does not add another attempt label', !labels(w, 94).includes('auto-fix-5') && !w.log.some(l => /^\+auto-fix/.test(l)));

  // --- red-team findings on #120 (2026-09-03) ---
  // F5: labels are a set; a human removing auto-fix-1 to grant another go leaves {2,3,4}.
  //     Writing attempts+1 (= auto-fix-4, already present) would be a no-op and the bound
  //     unreachable forever. The router must write the first FREE index.
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-2', 'auto-fix-3', 'auto-fix-4'])], jobs: JOBS});
  await route(w, {head_sha: 'gap001'});
  check('F5: non-prefix label set {2,3,4} -> writes the free auto-fix-1, count advances', w.outputs.route === 'autofix' && labels(w, 94).includes('auto-fix-1') && said(w, /attempt 4 of 4/, 94));
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'auto-fix-3', 'auto-fix-4'])], issues: [issue(26, ['task'])], jobs: JOBS, failedRuns: runs});
  await route(w, {head_sha: 'gap002'});
  check('F5: the full set then exhausts — the bound is reachable', w.outputs.route === 'exhausted');
  // F2/F7: knob semantics — < 1 disables (the sibling knobs' off switch), non-digit values
  //        are unreadable (fail closed to 2), values above 10 are clamped (no "unlimited").
  for (const off of ['0', '-1']) {
    w = world({prs: [pr(94, 'task/26', ['agent-authored'])], jobs: JOBS});
    await route(w, {head_sha: `off${off}`}, off);
    check(`F2: auto_fix_max_attempts=${off} disables the loop (no label; a marker comment quiets the sweep)`, w.outputs.route === 'none' && !w.log.some(l => /^\+/.test(l)) && said(w, /disabled/, 94) && said(w, new RegExp(`ci-failure-router sha=off${off}`), 94) && w.log.some(l => /notice:.*disabled/.test(l)));
  }
  // ...and the disabled marker is idempotent per sha+pass: a second delivery says nothing new.
  w = world({prs: [pr(94, 'task/26', ['agent-authored'])], jobs: JOBS, comments: {94: ['<!-- ci-failure-router sha=abc123 pass=1 run=5001 -->\n⏸️ disabled earlier']}});
  await route(w, {}, '0');
  check('F2: disabled state does not re-comment on the same sha+pass', w.outputs.route === 'none' && !w.log.some(l => l.startsWith('comment@')));
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'in-progress'])], issues: [issue(26, ['task', 'in-progress'])], jobs: JOBS, failedRuns: runs});
  await route(w, {head_sha: 'sci001'}, '1e3');
  check('F7: scientific-notation knob is unreadable -> fail closed to 2, not 1', w.outputs.route === 'exhausted' && w.log.some(l => /notice:.*unreadable/.test(l)));
  w = world({prs: [pr(94, 'task/26', ['agent-authored'])], jobs: JOBS});
  await route(w, {head_sha: 'big001'}, '999');
  check('F7: 999 is clamped to 10 and says so', w.outputs.route === 'autofix' && w.log.some(l => /notice:.*clamped to 10/.test(l)) && said(w, /attempt 1 of 10/, 94));
  // F3: the exhaustion comment reports the attempts actually taken, never just the bound.
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'auto-fix-3', 'auto-fix-4'])], issues: [issue(26, ['task'])], jobs: JOBS, failedRuns: runs});
  await route(w, {head_sha: 'low001'}, '2');
  check('F3: 4 labels, knob lowered to 2 -> "after 4 attempts (configured bound 2)"', w.outputs.route === 'exhausted' && said(w, /after 4 attempts \(configured bound 2\)/, 94));
  // F6 + M1/M2: every non-successful conclusion is listed (timed_out, startup_failure,
  //             cancelled), successes are not; no tail when nothing was cut; an empty list
  //             falls back to naming the triggering run.
  const mixed = [{id: 7001, name: 'ci', head_branch: 'task/26', status: 'completed', conclusion: 'failure', html_url: 'https://gh/r/7001'},
                 {id: 7002, name: 'ci', head_branch: 'task/26', status: 'completed', conclusion: 'timed_out', html_url: 'https://gh/r/7002'},
                 {id: 7003, name: 'ci', head_branch: 'task/26', status: 'completed', conclusion: 'startup_failure', html_url: 'https://gh/r/7003'},
                 {id: 7004, name: 'ci', head_branch: 'task/26', status: 'completed', conclusion: 'cancelled', html_url: 'https://gh/r/7004'},
                 {id: 7005, name: 'ci', head_branch: 'task/26', status: 'completed', conclusion: 'success', html_url: 'https://gh/r/7005'}];
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'auto-fix-3', 'auto-fix-4'])], issues: [issue(26, ['task'])], jobs: JOBS, failedRuns: mixed});
  await route(w, {head_sha: 'mix001'});
  check('F6: timed_out/startup_failure/cancelled listed with their conclusion; success is not', said(w, /7002.*timed_out/, 94) && said(w, /7003.*startup_failure/, 94) && said(w, /7004.*cancelled/, 94) && !said(w, /7005/, 94));
  check('M1: nothing was cut -> no "and N more" tail and no window line', !said(w, /more non-successful/, 94) && !said(w, /window:/, 94));
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'auto-fix-3', 'auto-fix-4'])], issues: [issue(26, ['task'])], jobs: JOBS, failedRuns: []});
  await route(w, {head_sha: 'mt0001'});
  check('M2: empty run list -> the comment still names the triggering run (fallback)', said(w, /actions\/runs\/5001/, 94));
  // Partition before capping (review round 3 on #120): ci.yml cancels in-progress runs on
  // every non-main push, so a burst of pushes manufactures newer `cancelled` runs than the
  // real failures — newest-first alone would fill all 10 slots with cancellations.
  const noisy = [...Array.from({length: 12}, (_, k) => ({id: 9500 + k, name: 'ci', head_branch: 'task/26', status: 'completed', conclusion: 'cancelled', html_url: `https://gh/r/${9500 + k}`})),
                 {id: 9400, name: 'ci', head_branch: 'task/26', status: 'completed', conclusion: 'failure', html_url: 'https://gh/r/9400'},
                 {id: 9300, name: 'security', head_branch: 'task/26', status: 'completed', conclusion: 'timed_out', html_url: 'https://gh/r/9300'}];
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'auto-fix-3', 'auto-fix-4'])], issues: [issue(26, ['task'])], jobs: JOBS, failedRuns: noisy});
  await route(w, {head_sha: 'noz001'});
  check('rank: hard failures listed before cancellations even when 12 cancellations are newer', said(w, /9400.*failure/, 94) && said(w, /9300.*timed_out/, 94) && said(w, /and 4 more non-successful/, 94));
  check('rank: the oldest cancellations are what falls below the cap, not the failures', !said(w, /9511/, 94) && said(w, /9500/, 94));
  // Window clipping (review on #120): counts are scoped to the fetch, never worded branch-wide.
  const flood = Array.from({length: 105}, (_, k) => ({id: 9000 + k, name: 'ci', head_branch: 'task/26', status: 'completed', conclusion: 'failure', html_url: `https://gh/r/${9000 + k}`}));
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'auto-fix-1', 'auto-fix-2', 'auto-fix-3', 'auto-fix-4'])], issues: [issue(26, ['task'])], jobs: JOBS, failedRuns: flood});
  await route(w, {head_sha: 'fld001'});
  check('window: 105 runs -> tail counts within the fetched 100 and the clip is disclosed', said(w, /and 90 more non-successful run\(s\) in the newest 100 runs fetched/, 94) && said(w, /window: newest 100 of 105 runs on this branch — older runs not scanned/, 94));

  // --- sweep (delivery backstop): re-delivers at EVERY attempt count; `route` decides ---
  const swRuns = [{id: 8001, name: 'ci', head_branch: 'task/26', head_sha: 'aaa111', status: 'completed', conclusion: 'failure', created_at: '2026-09-01T00:00:00Z', run_attempt: 1, html_url: 'https://gh/r/8001'}];
  for (const n of [0, 2, 4, 5]) {
    w = world({prs: [pr(94, 'task/26', ['agent-authored', ...Array.from({length: n}, (_, i) => `auto-fix-${i + 1}`)])], failedRuns: swRuns});
    await sweep(w);
    check(`sweep: re-delivers at ${n} attempt labels (F1: escalation needs the backstop most)`, w.log.some(l => l.startsWith('dispatch run=8001')));
  }
  w = world({prs: [pr(94, 'task/26', ['agent-authored', 'needs-human'])], failedRuns: swRuns});
  await sweep(w);
  check('sweep: needs-human is the only label stop', !w.log.some(l => l.startsWith('dispatch')));
  w = world({prs: [pr(94, 'task/26', ['agent-authored'])], failedRuns: swRuns, comments: {94: ['<!-- ci-failure-router sha=aaa111 pass=1 run=8001 -->']}});
  await sweep(w);
  check('sweep: an attempt marker for the head sha+pass suppresses re-dispatch', !w.log.some(l => l.startsWith('dispatch')));
  w = world({prs: [pr(94, 'task/26', ['agent-authored'])], failedRuns: [...swRuns, {id: 8002, name: 'ci', head_branch: 'task/26', head_sha: 'aaa111', status: 'completed', conclusion: 'success', created_at: '2026-09-02T00:00:00Z', run_attempt: 1}]});
  await sweep(w);
  check('sweep: a later success on the same workflow means fixed — no dispatch', !w.log.some(l => l.startsWith('dispatch')));
  w = world({prs: [pr(94, 'task/26', ['agent-authored'])], failedRuns: [{id: 8003, name: 'ci', head_branch: 'task/26', head_sha: 'old000', status: 'completed', conclusion: 'failure', created_at: '2026-09-01T00:00:00Z', run_attempt: 1, html_url: 'https://gh/r/8003'}]});
  await sweep(w);
  check('sweep: a stale head\'s failure is never re-delivered (head_sha pin observable)', !w.log.some(l => l.startsWith('dispatch')));
  w = world({prs: [pr(95, 'ci/tooling', ['agent-authored'])], failedRuns: swRuns});
  await sweep(w);
  check('sweep: non-task branch is not swept', !w.log.some(l => l.startsWith('dispatch')));
  w = world({prs: [Object.assign(pr(96, 'task/26', ['agent-authored']), {head: {ref: 'task/26', sha: 'aaa111', repo: {full_name: 'someone/fork'}}})], failedRuns: swRuns});
  await sweep(w);
  check('sweep: fork head is never swept', !w.log.some(l => l.startsWith('dispatch')));

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
