// Executes the merge github-script body of .github/workflows/agent-merge.yml VERBATIM
// against a mocked GitHub API. This is the only gate between an agent's work and `main`
// with no human in the loop, so every refusal in it is a test here. Driven by
// tools/refimpl/test_workflow_scripts.py, which extracts the script into a JSON file (argv[2]).
//
// The real CODEOWNERS and agent-config.yml are read from the repo root (argv[3]) — the
// script reads them from the default-branch checkout, and the cases below assert on the
// paths those real files actually protect.
'use strict';
const fs = require('fs');
const path = require('path');
const S = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const ROOT = process.argv[3] || '.';
process.env.GITHUB_WORKSPACE = ROOT;

const REPO = 'o/r';
const HEAD = 'a'.repeat(40);
const OLD = 'b'.repeat(40);

function world({prs = [], issues = [], comments = {}, files = {}, checks = null, status = null, mergeFails = null} = {}) {
  const log = [];
  const prState = new Map(prs.map(p => [p.number, p]));
  const issueState = new Map(issues.map(i => [i.number, i]));
  const github = {
    paginate: async (fn, args) => fn(args).then(r => r.data),
    rest: {
      pulls: {
        list: async () => ({data: [...prState.values()].filter(p => p.state === 'open')}),
        get: async ({pull_number}) => { const p = prState.get(pull_number); if (!p) throw Object.assign(new Error('Not Found'), {status: 404}); return {data: p}; },
        listFiles: async ({pull_number}) => ({data: (files[pull_number] || ['core/link.cpp']).map(filename => ({filename}))}),
        merge: async ({pull_number, sha, merge_method, commit_message}) => {
          if (mergeFails) throw Object.assign(new Error(mergeFails.message), {status: mergeFails.status});
          log.push(`merge@${pull_number} sha=${sha.slice(0, 7)} method=${merge_method} msg=${commit_message.replace(/\n+/g, ' | ').slice(0, 300)}`);
          prState.get(pull_number).state = 'closed';
        },
      },
      issues: {
        listComments: async ({issue_number}) => ({data: comments[issue_number] || []}),
        createComment: async ({issue_number, body}) => log.push(`comment@${issue_number}: ${body.replace(/\n+/g, ' | ').slice(0, 400)}`),
        get: async ({issue_number}) => { const i = issueState.get(issue_number); if (!i) throw Object.assign(new Error('Not Found'), {status: 404}); return {data: i}; },
        update: async ({issue_number, state}) => { const i = issueState.get(issue_number); i.state = state; log.push(`close@${issue_number}`); },
        removeLabel: async ({issue_number, name}) => {
          const i = issueState.get(issue_number);
          if (!i || !i.labels.some(l => l.name === name)) throw Object.assign(new Error('Not Found'), {status: 404});
          i.labels = i.labels.filter(l => l.name !== name); log.push(`-${name}@${issue_number}`);
        },
      },
      checks: {
        listForRef: async () => ({data: checks || [{name: 'score', status: 'completed', conclusion: 'success'},
                                                   {name: 'ci-gate', status: 'completed', conclusion: 'success'}]}),
      },
      repos: {
        getCombinedStatusForRef: async () => ({data: status || {total_count: 0, state: 'pending'}}),
      },
    },
  };
  const core = {info: () => {}, notice: m => log.push(`notice: ${m}`), warning: m => log.push(`warning: ${m}`), setOutput: () => {}};
  return {github, core, log, prState, issueState};
}

const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
async function run(w, {number = 94, comment = {user: {login: 'claude[bot]'}, body: `VERDICT(review): clean @ ${HEAD}`}, sweep = false, dispatchPr = ''} = {}) {
  const payload = sweep ? {} : {issue: {number, pull_request: {url: 'u'}}, comment};
  const context = {repo: {owner: 'o', repo: 'r'}, eventName: sweep ? 'schedule' : 'issue_comment', runId: 7777, serverUrl: 'https://gh', payload};
  process.env.DISPATCH_PR = dispatchPr;
  try {
    await new AsyncFunction('github', 'context', 'core', 'require', S.merge)(w.github, context, w.core, require);
  } finally { delete process.env.DISPATCH_PR; }
}

const PR = (labels, {number = 94, ref = 'task/26', sha = HEAD, state = 'open', draft = false, mergeable = true, mergeable_state = 'clean', full_name = REPO, title = 'T026 something'} = {}) =>
  ({number, state, draft, mergeable, mergeable_state, title, labels: labels.map(name => ({name})), head: {ref, sha, repo: {full_name}}});
const verdict = (kind, state, sha, {user = 'claude[bot]'} = {}) =>
  ({user: {login: user}, body: `text\n\nNOT EXAMINED: nothing excluded\nVERDICT(${kind}): ${state} @ ${sha}`});
const clean = sha => [verdict('review', 'clean', sha)];
const cleanT2 = sha => [verdict('review', 'clean', sha), verdict('red-team', 'clean', sha)];

const results = [];
const check = (name, cond) => { results.push([name, !!cond]); if (!cond) process.exitCode = 1; };
const mergedIt = w => w.log.some(l => l.startsWith('merge@'));
const said = (w, re) => w.log.some(l => re.test(l));

(async () => {
  let w;
  // --- merges ---
  w = world({prs: [PR(['agent-authored', 'risk:t1'])], comments: {94: clean(HEAD)}});
  await run(w);
  check('T1, clean review, checks green -> merged at the verdict head', mergedIt(w) && said(w, new RegExp(`sha=${HEAD.slice(0, 7)}`)) && said(w, /method=merge/));
  check('merge message records the tier, the verdict and the knob', said(w, /risk:t1 <= auto_merge_max_tier=2/) && said(w, /VERDICT\(review\): clean/));

  w = world({prs: [PR(['agent-authored', 'risk:t2'])], comments: {94: cleanT2(HEAD)}});
  await run(w);
  check('T2 with a clean red-team verdict too -> merged', mergedIt(w) && said(w, /VERDICT\(red-team\): clean/));

  w = world({prs: [PR(['agent-authored', 'risk:t0'])], comments: {94: clean(HEAD)}, files: {94: ['docs/OPEN-QUESTIONS.md', 'specs/001-x/tasks.md', 'sim/x.cpp']}});
  await run(w);
  check('the agent-sanctioned owner paths (OPEN-QUESTIONS, tasks.md) do not block the merge', mergedIt(w));

  w = world({prs: [PR(['agent-authored', 'risk:t1'], {number: 94}), PR(['agent-authored', 'risk:t1'], {number: 95})],
             comments: {94: clean(HEAD), 95: clean(HEAD)}});
  await run(w, {sweep: true});
  check('sweep considers every open PR, not just a commented one', w.log.filter(l => l.startsWith('merge@')).length === 2);

  // --- tier ---
  w = world({prs: [PR(['agent-authored', 'risk:t3'])], comments: {94: cleanT2(HEAD)}});
  await run(w);
  check('T3 is never merged by an agent, however clean', !mergedIt(w) && said(w, /T3 is never merged/));

  w = world({prs: [PR(['agent-authored'])], comments: {94: clean(HEAD)}});
  await run(w);
  check('no risk label -> fail closed', !mergedIt(w) && said(w, /risk tier unresolved/));

  w = world({prs: [PR(['agent-authored', 'risk:t1'])], comments: {94: clean(HEAD)},
             checks: [{name: 'ci-gate', status: 'completed', conclusion: 'success'}]});
  await run(w);
  check('label present but the `score` check never ran at this head -> fail closed', !mergedIt(w) && said(w, /scored=false/));

  // --- CODEOWNERS: ground truth and governance stay human ---
  for (const [name, f] of [['protocol YAML', 'protocol/omgp-protocol.yaml'], ['golden vectors', 'tests/vectors/v1.json'],
                           ['spec doc', 'docs/protocol-l3.md'], ['governance', 'docs/GOVERNANCE.md'],
                           ['workflows', '.github/workflows/ci.yml'], ['CLAUDE.md', 'CLAUDE.md'],
                           ['pipeline definition', 'pipeline.sh'], ['mutation policy', 'tools/mutate.cfg']]) {
    w = world({prs: [PR(['agent-authored', 'risk:t1'])], comments: {94: clean(HEAD)}, files: {94: [f]}});
    await run(w);
    check(`CODEOWNERS: ${name} (${f}) is never auto-merged`, !mergedIt(w) && said(w, /CODEOWNERS-owned path/));
  }

  // --- verdict discipline (identical rule to agent-approve) ---
  w = world({prs: [PR(['agent-authored', 'risk:t1'])], comments: {94: []}});
  await run(w);
  check('no review verdict at this head -> not merged', !mergedIt(w) && said(w, /no review verdict/));

  w = world({prs: [PR(['agent-authored', 'risk:t1'])], comments: {94: clean(OLD)}});
  await run(w);
  check('review verdict from an older head -> not merged', !mergedIt(w) && said(w, /no review verdict/));

  w = world({prs: [PR(['agent-authored', 'risk:t1'])], comments: {94: [verdict('review', 'findings', HEAD)]}});
  await run(w);
  check('review reported findings -> not merged', !mergedIt(w) && said(w, /review reported findings/));

  w = world({prs: [PR(['agent-authored', 'risk:t1'])], comments: {94: [verdict('review', 'clean', HEAD, {user: 'someone'})]}});
  await run(w);
  check('verdict from a non-claude[bot] author -> not merged', !mergedIt(w));

  w = world({prs: [PR(['agent-authored', 'risk:t1'])], comments: {94: [{user: {login: 'claude[bot]'}, body: `VERDICT(review): clean @ ${HEAD}\n\nquoted, not my verdict`}]}});
  await run(w);
  check('verdict token that is not the final line -> not merged', !mergedIt(w));

  w = world({prs: [PR(['agent-authored', 'risk:t2'])], comments: {94: clean(HEAD)}});
  await run(w);
  check('T2 without a red-team verdict -> not merged', !mergedIt(w) && said(w, /no red-team verdict/));

  w = world({prs: [PR(['agent-authored', 'risk:t2'])], comments: {94: [verdict('review', 'clean', HEAD), verdict('red-team', 'findings', HEAD)]}});
  await run(w);
  check('T2 with red-team findings -> not merged', !mergedIt(w) && said(w, /red-team reported findings/));

  // --- who may be merged ---
  for (const [name, pr] of [['human-authored', PR(['human-authored', 'risk:t1'])],
                            ['needs-human', PR(['agent-authored', 'risk:t1', 'needs-human'])],
                            ['blocked', PR(['agent-authored', 'risk:t1', 'blocked'])],
                            ['draft', PR(['agent-authored', 'risk:t1'], {draft: true})],
                            ['fork head', PR(['agent-authored', 'risk:t1'], {full_name: 'someone/fork'})],
                            ['non-task branch', PR(['agent-authored', 'risk:t1'], {ref: 'feat/x'})],
                            ['unmergeable', PR(['agent-authored', 'risk:t1'], {mergeable: false, mergeable_state: 'dirty'})]]) {
    w = world({prs: [pr], comments: {94: clean(HEAD)}});
    await run(w);
    check(`${name} -> not merged`, !mergedIt(w));
  }

  // --- checks ---
  w = world({prs: [PR(['agent-authored', 'risk:t1'])], comments: {94: clean(HEAD)},
             checks: [{name: 'score', status: 'completed', conclusion: 'success'}, {name: 'ci-gate', status: 'in_progress', conclusion: null}]});
  await run(w);
  check('a check still running -> wait, never merge', !mergedIt(w) && said(w, /checks still running: ci-gate/));

  w = world({prs: [PR(['agent-authored', 'risk:t1'])], comments: {94: clean(HEAD)},
             checks: [{name: 'score', status: 'completed', conclusion: 'success'}, {name: 'ci-gate', status: 'completed', conclusion: 'failure'}]});
  await run(w);
  check('a red check -> not merged', !mergedIt(w) && said(w, /checks not green: ci-gate=failure/));

  w = world({prs: [PR(['agent-authored', 'risk:t1'])], comments: {94: clean(HEAD)},
             checks: [{name: 'score', status: 'completed', conclusion: 'success'}, {name: 'red-team', status: 'completed', conclusion: 'skipped'}]});
  await run(w);
  check('skipped/neutral checks are green enough', mergedIt(w));

  w = world({prs: [PR(['agent-authored', 'risk:t1'])], comments: {94: clean(HEAD)}, status: {total_count: 2, state: 'failure'}});
  await run(w);
  check('a failing legacy commit status -> not merged', !mergedIt(w) && said(w, /commit status failure/));

  // --- the race this design exists to lose safely ---
  w = world({prs: [PR(['agent-authored', 'risk:t1'])], comments: {94: clean(HEAD)}, mergeFails: {status: 409, message: 'Head branch was modified'}});
  await run(w);
  check('409 (head moved between the checks and the merge) -> nothing merged, no noise', !mergedIt(w) && said(w, /head moved since/) && !w.log.some(l => l.startsWith('comment@')));

  w = world({prs: [PR(['agent-authored', 'risk:t1'])], comments: {94: clean(HEAD)}, mergeFails: {status: 405, message: 'Pull Request is not mergeable'}});
  await run(w);
  check('405 (branch protection refused) -> left for a human, said once', !mergedIt(w) && said(w, /comment@94.*Left for a human/));

  w = world({prs: [PR(['agent-authored', 'risk:t1'])], comments: {94: [...clean(HEAD), {user: {login: 'github-actions[bot]'}, body: `<!-- agent-merge sha=${HEAD} -->\nearlier complaint`}]},
             mergeFails: {status: 405, message: 'Pull Request is not mergeable'}});
  await run(w);
  check('405 again on the same head -> no second comment (sweep runs every 20 minutes)', !w.log.some(l => l.startsWith('comment@')));

  // --- the claim must be released MECHANICALLY, not by the PR body's prose ---
  // Live failure on #114 (2026-09-02): the body said "Closes T021 (issue #39)", which GitHub
  // does not treat as a closing reference. Issue #39 stayed open with `in-progress`, held the
  // WIP cap, and agent-dispatch pulled nothing for ~10 hours.
  const ISSUE = (number, labels, state = 'open') => ({number, state, labels: labels.map(name => ({name}))});

  w = world({prs: [PR(['agent-authored', 'risk:t1'], {ref: 'task/39'})],
             issues: [ISSUE(39, ['task', 'in-progress'])], comments: {94: clean(HEAD)}});
  w.prState.get(94).body = 'Closes T021 (issue #39) - a form GitHub does not honour.';
  await run(w);
  check('malformed closing prose -> the branch task/<n> still releases the claim and closes it',
        mergedIt(w) && said(w, /-in-progress@39/) && said(w, /close@39/) && w.issueState.get(39).state === 'closed');

  w = world({prs: [PR(['agent-authored', 'risk:t1'], {ref: 'task/39'})],
             issues: [ISSUE(39, ['task', 'in-progress']), ISSUE(40, ['task', 'in-progress'])], comments: {94: clean(HEAD)}});
  w.prState.get(94).body = 'Body. Closes #40';
  await run(w);
  check('every Closes/Fixes target is released, plus the branch issue', said(w, /close@40/) && said(w, /close@39/));

  w = world({prs: [PR(['agent-authored', 'risk:t1'], {ref: 'task/39'})],
             issues: [ISSUE(39, ['task'], 'closed')], comments: {94: clean(HEAD)}});
  await run(w);
  check('an already-closed issue is not re-closed or re-commented', mergedIt(w) && !said(w, /close@39/) && !said(w, /comment@39/));

  w = world({prs: [PR(['agent-authored', 'risk:t1'], {ref: 'task/39'})], issues: [], comments: {94: clean(HEAD)}});
  await run(w);
  check('a missing issue (404) is a non-event, never a failed merge', mergedIt(w) && !w.log.some(l => l.startsWith('warning:')));

  for (const [n, ok] of results) console.log((ok ? 'ok   ' : 'FAIL ') + n);
  console.log(`${results.filter(r => r[1]).length}/${results.length} cases passed`);
})();
