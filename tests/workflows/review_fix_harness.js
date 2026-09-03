// Executes the `gate` github-script body of .github/workflows/review-fix.yml VERBATIM
// against a mocked GitHub API, so the bounds that make the review-finding loop safe are
// tested on every push instead of only when a real review posts findings. Driven by
// tools/refimpl/test_workflow_scripts.py, which extracts the script into a JSON file (argv[2]).
//
// The properties under test: only claude[bot] verdicts count, only as the FINAL line, only
// at the CURRENT head; only agent-authored task/* PRs are ever pushed to; one attempt per
// head commit; two attempts per PR, then needs-human with the claim released.
'use strict';
const fs = require('fs');
const S = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
// The gate reads review_fix_max_attempts from the real .github/agent-config.yml on the
// default branch, so the harness points it at the repo root it was given.
process.env.GITHUB_WORKSPACE = process.argv[3] || '.';
const MAX = Number.parseInt(/^review_fix_max_attempts: *(\d+)/m.exec(
  fs.readFileSync(`${process.env.GITHUB_WORKSPACE}/.github/agent-config.yml`, 'utf8'))[1], 10);
const SPENT = Array.from({length: MAX}, (_, i) => `review-fix-${i + 1}`);   // every attempt used up

const REPO = 'o/r';
const HEAD = 'a'.repeat(40);
const OLD = 'b'.repeat(40);

function world({pr, issues = [], comments = [], failRemove = null} = {}) {
  const log = [];
  const outputs = {};
  const prState = new Map([[pr.number, pr]]);
  const issueState = new Map(issues.map(i => [i.number, i]));
  const labelsOf = n => (prState.get(n) || issueState.get(n) || {labels: []}).labels;
  const github = {
    paginate: async (fn, args) => fn(args).then(r => r.data),
    rest: {
      pulls: {
        get: async ({pull_number}) => { const p = prState.get(pull_number); if (!p) throw Object.assign(new Error('Not Found'), {status: 404}); return {data: p}; },
      },
      issues: {
        listComments: async () => ({data: comments}),
        addLabels: async ({issue_number, labels}) => { for (const l of labels) labelsOf(issue_number).push({name: l}); log.push(`+${labels.join('+')}@${issue_number}`); },
        removeLabel: async ({issue_number, name}) => {
          if (failRemove === issue_number) throw Object.assign(new Error('Server Error'), {status: 500});
          const t = prState.get(issue_number) || issueState.get(issue_number);
          if (!t || !t.labels.some(l => l.name === name)) throw Object.assign(new Error('Not Found'), {status: 404});
          t.labels = t.labels.filter(l => l.name !== name); log.push(`-${name}@${issue_number}`);
        },
        createComment: async ({issue_number, body}) => log.push(`comment@${issue_number}: ${body.replace(/\n+/g, ' | ').slice(0, 700)}`),
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
async function gate(w, {number = 94, comment = {user: {login: 'claude[bot]'}, body: `VERDICT(review): findings @ ${HEAD}`}, dispatchPr = ''} = {}) {
  const payload = {issue: {number, pull_request: {url: 'u'}}, comment};
  const context = {repo: {owner: 'o', repo: 'r'}, eventName: 'issue_comment', runId: 7777, serverUrl: 'https://gh', payload};
  const prev = process.env.DISPATCH_PR;
  process.env.DISPATCH_PR = dispatchPr;
  try {
    await new AsyncFunction('github', 'context', 'core', 'require', S.gate)(w.github, context, w.core, require);
  } finally { if (prev === undefined) delete process.env.DISPATCH_PR; else process.env.DISPATCH_PR = prev; }
}

const PR = (labels, {ref = 'task/26', sha = HEAD, state = 'open', body = '', full_name = REPO} = {}) => ({
  number: 94, state, body, labels: labels.map(name => ({name})),
  head: {ref, sha, repo: {full_name}},
});
const issue = (number, labels) => ({number, state: 'open', labels: labels.map(name => ({name}))});
const verdict = (kind, state, sha, {user = 'claude[bot]', pre = ''} = {}) =>
  ({user: {login: user}, body: `${pre}findings text here\n\nNOT EXAMINED: nothing excluded\nVERDICT(${kind}): ${state} @ ${sha}`});

const results = [];
const check = (name, cond) => { results.push([name, !!cond]); if (!cond) process.exitCode = 1; };
const said = (w, re, n) => w.log.some(l => re.test(l) && (n === undefined || l.startsWith(`comment@${n}`)));
const labels = (w, n) => (w.prState.get(n) || w.issueState.get(n)).labels.map(l => l.name);
const quiet = w => !w.log.some(l => l.startsWith('comment@') || /^[+-]/.test(l));

(async () => {
  let w;
  // --- the happy path ---
  w = world({pr: PR(['agent-authored', 'in-progress']), comments: [verdict('review', 'findings', HEAD)]});
  await gate(w);
  check('review findings at head -> attempt 1 + go', w.outputs.go === 'yes' && w.outputs.attempt === '1' && w.outputs.kinds === 'review' && labels(w, 94).includes('review-fix-1'));
  check('outputs carry the branch and head the fix job checks out', w.outputs.branch === 'task/26' && w.outputs.head === HEAD && w.outputs.pr === '94');
  check('attempt comment carries the sha marker and states the severity policy', said(w, new RegExp(`review-fix sha=${HEAD}`), 94) && said(w, new RegExp(`attempt 1 of ${MAX}`), 94) && said(w, /LOW finding is fixed only where/, 94));
  check('attempt path touches no issue labels', !w.log.some(l => /@26$/.test(l)));

  w = world({pr: PR(['agent-authored', 'review-fix-1']), comments: [verdict('review', 'findings', HEAD), verdict('red-team', 'findings', HEAD)]});
  await gate(w);
  check('both passes report findings -> attempt 2, kinds names both', w.outputs.go === 'yes' && w.outputs.attempt === '2' && w.outputs.kinds === 'review,red-team' && labels(w, 94).includes('review-fix-2'));

  // The bound is whatever .github/agent-config.yml says (4 since the 2026-09-03 ruling, raised
  // from 2 after #116 exhausted the old bound while still fixing real findings each round).
  w = world({pr: PR(['agent-authored', 'review-fix-1', 'review-fix-2']), comments: [verdict('review', 'findings', HEAD)]});
  await gate(w);
  check(`two attempts spent -> a third is still due when the bound is ${MAX}`,
        MAX > 2 ? (w.outputs.go === 'yes' && w.outputs.attempt === '3') : w.outputs.go === 'no');
  check('the attempt comment states the configured bound', said(w, new RegExp(`attempt 3 of ${MAX}`), 94));

  w = world({pr: PR(['agent-authored']), comments: [verdict('review', 'clean', HEAD), verdict('red-team', 'findings', HEAD)]});
  await gate(w);
  check('clean review but red-team findings -> still fixes, kinds is red-team only', w.outputs.go === 'yes' && w.outputs.kinds === 'red-team');

  // --- verdict discipline (mirrors agent-approve: the gate must not be fooled) ---
  w = world({pr: PR(['agent-authored']), comments: [verdict('review', 'clean', HEAD)]});
  await gate(w);
  check('clean verdict -> nothing to fix', w.outputs.go === 'no' && quiet(w));

  w = world({pr: PR(['agent-authored']), comments: [verdict('review', 'findings', OLD)]});
  await gate(w);
  check('findings verdict from an older head -> ignored', w.outputs.go === 'no' && quiet(w));

  w = world({pr: PR(['agent-authored']), comments: [{user: {login: 'claude[bot]'}, body: `VERDICT(review): findings @ ${HEAD}\n\nquoted above, not my verdict`}]});
  await gate(w);
  check('verdict token that is not the final line -> ignored', w.outputs.go === 'no' && quiet(w));

  w = world({pr: PR(['agent-authored']), comments: [verdict('review', 'findings', HEAD, {user: 'someone'})]});
  await gate(w);
  check('verdict from a non-claude[bot] author -> ignored', w.outputs.go === 'no' && quiet(w));

  w = world({pr: PR(['agent-authored']), comments: [verdict('review', 'findings', HEAD), verdict('review', 'clean', HEAD)]});
  await gate(w);
  check('latest verdict at this head wins (findings then clean) -> nothing to fix', w.outputs.go === 'no' && quiet(w));

  w = world({pr: PR(['agent-authored']), comments: [verdict('review', 'findings', HEAD)]});
  await gate(w, {comment: {user: {login: 'github-actions[bot]'}, body: 'VERDICT(review): findings @ ' + HEAD}});
  check('trigger comment not authored by claude[bot] -> untouched', w.outputs.go === 'no' && quiet(w));

  // --- who may be pushed to ---
  w = world({pr: PR(['human-authored']), comments: [verdict('review', 'findings', HEAD)]});
  await gate(w);
  check('not agent-authored -> never pushed to', w.outputs.go === 'no' && quiet(w));

  w = world({pr: PR(['agent-authored', 'needs-human']), comments: [verdict('review', 'findings', HEAD)]});
  await gate(w);
  check('already needs-human -> no-op', w.outputs.go === 'no' && quiet(w));

  w = world({pr: PR(['agent-authored'], {ref: 'feat/thing'}), comments: [verdict('review', 'findings', HEAD)]});
  await gate(w);
  check('non-task/* branch -> untouched', w.outputs.go === 'no' && quiet(w));

  w = world({pr: PR(['agent-authored'], {full_name: 'someone/fork'}), comments: [verdict('review', 'findings', HEAD)]});
  await gate(w);
  check('fork head -> never touched (no secrets path for forks)', w.outputs.go === 'no' && quiet(w));

  w = world({pr: PR(['agent-authored'], {state: 'closed'}), comments: [verdict('review', 'findings', HEAD)]});
  await gate(w);
  check('closed PR -> nothing to do', w.outputs.go === 'no' && quiet(w));

  // --- one attempt per head commit ---
  w = world({pr: PR(['agent-authored', 'review-fix-1']),
             comments: [verdict('review', 'findings', HEAD), {user: {login: 'github-actions[bot]'}, body: `<!-- review-fix sha=${HEAD} -->\n🔧 earlier`}, verdict('red-team', 'findings', HEAD)]});
  await gate(w);
  check('red-team findings arriving after review on the SAME commit -> not a second attempt', w.outputs.go === 'no' && !labels(w, 94).includes('review-fix-2') && quiet(w));

  // --- the bound ---
  w = world({pr: PR(['agent-authored', ...SPENT, 'in-progress'], {body: 'Body.\n\nCloses #40'}),
             issues: [issue(26, ['task', 'in-progress']), issue(40, ['task', 'in-progress'])],
             comments: [verdict('review', 'findings', HEAD)]});
  await gate(w);
  check('every attempt spent -> no further attempt, escalates needs-human on the PR', w.outputs.go === 'no' && labels(w, 94).includes('needs-human') && !labels(w, 94).includes(`review-fix-${MAX + 1}`) && !w.log.some(l => /^\+review-fix/.test(l)));
  check('exhaustion releases the claim on the PR and on the issue it Closes, not the branch digits', !labels(w, 94).includes('in-progress') && labels(w, 40).includes('needs-human') && !labels(w, 40).includes('in-progress') && !labels(w, 26).includes('needs-human'));
  check('exhaustion comment says which pass still has findings', said(w, /exhausted/, 94) && said(w, /review findings remain/, 94));
  check('exhaustion comment names the configured bound', said(w, new RegExp(`exhausted\\*\\* after ${MAX} attempts`), 94));

  w = world({pr: PR(['agent-authored', ...SPENT, 'in-progress']),
             issues: [issue(26, ['task', 'in-progress'])], comments: [verdict('review', 'findings', HEAD)], failRemove: 26});
  await gate(w);
  check('a non-404 failure to release the claim is reported, never claimed as done', said(w, /could NOT release `in-progress` on #26/, 94) && !said(w, /released on #26/, 94));
  check('the PR side still got needs-human', labels(w, 94).includes('needs-human'));

  // --- manual dispatch path (no comment in the payload) ---
  w = world({pr: PR(['agent-authored']), comments: [verdict('review', 'findings', HEAD)]});
  await (async () => {
    const context = {repo: {owner: 'o', repo: 'r'}, eventName: 'workflow_dispatch', runId: 7777, serverUrl: 'https://gh', payload: {}};
    process.env.DISPATCH_PR = '94';
    await new AsyncFunction('github', 'context', 'core', 'require', S.gate)(w.github, context, w.core, require);
    delete process.env.DISPATCH_PR;
  })();
  check('workflow_dispatch with a PR number -> same decision, no comment required', w.outputs.go === 'yes' && labels(w, 94).includes('review-fix-1'));

  // Copilot review on #113: `Number('')` is 0, so a blank dispatch input must not act on PR #0.
  w = world({pr: PR(['agent-authored']), comments: [verdict('review', 'findings', HEAD)]});
  await (async () => {
    const context = {repo: {owner: 'o', repo: 'r'}, eventName: 'workflow_dispatch', runId: 7777, serverUrl: 'https://gh', payload: {}};
    process.env.DISPATCH_PR = '   ';
    await new AsyncFunction('github', 'context', 'core', 'require', S.gate)(w.github, context, w.core, require);
    delete process.env.DISPATCH_PR;
  })();
  check('blank workflow_dispatch input -> fails closed, never acts on PR #0', w.outputs.go === 'no' && quiet(w) && w.log.some(l => /no usable PR number/.test(l)));

  for (const [n, ok] of results) console.log((ok ? 'ok   ' : 'FAIL ') + n);
  console.log(`${results.filter(r => r[1]).length}/${results.length} cases passed`);
})();
