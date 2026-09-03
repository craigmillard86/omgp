// Executes the `pick` github-script body of .github/workflows/agent-dispatch.yml VERBATIM
// against a mocked GitHub API (ruling 2026-09-03: WIP cap is the agent-config `wip_cap`
// knob, counting STORIES in flight — open agent-authored PRs ∪ claimed task issues,
// joined on the task/<n> head-branch convention so a story with both counts once).
// Driven by tools/refimpl/test_workflow_scripts.py (script JSON path in argv[2]).
'use strict';
const fs = require('fs');
const S = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));

function world({prs = [], claimed = [], ready = []} = {}) {
  const log = [];
  const outputs = {};
  const github = {
    rest: {
      pulls: {
        list: async () => ({data: prs.map(([number, head, labels]) => ({number, head: {ref: head}, labels: labels.map(name => ({name}))}))}),
      },
      issues: {
        listForRepo: async ({labels}) => {
          if (labels === 'task,in-progress') return {data: claimed.map(n => ({number: n, labels: [{name: 'task'}, {name: 'in-progress'}]}))};
          if (labels === 'task,ready') return {data: ready.map(([n, extra]) => ({number: n, labels: ['task', 'ready', ...(extra || [])].map(name => ({name}))}))};
          throw new Error(`unexpected labels query ${labels}`);
        },
        addLabels: async ({issue_number, labels}) => log.push(`+${labels.join('+')}@${issue_number}`),
        createComment: async ({issue_number}) => log.push(`comment@${issue_number}`),
      },
    },
  };
  const core = {info: () => {}, notice: m => log.push(`notice: ${m}`), setOutput: (k, v) => { outputs[k] = String(v); }};
  return {github, core, log, outputs};
}
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
async function pick(w, cap = '2') {
  process.env.WIP_CAP = cap;
  const context = {repo: {owner: 'o', repo: 'r'}, runId: 1, serverUrl: 'https://gh'};
  await new AsyncFunction('github', 'context', 'core', 'require', S.pick)(w.github, context, w.core, require);
}
const claimedIssue = w => w.outputs.issue || null;
const results = [];
const check = (name, cond) => { results.push([name, !!cond]); if (!cond) process.exitCode = 1; };

(async () => {
  let w;
  w = world({ready: [[10], [11]]});
  await pick(w);
  check('nothing in flight -> claims the oldest ready', claimedIssue(w) === '10' && w.log.includes('+in-progress+agent-authored@10'));

  w = world({prs: [[200, 'task/9', ['agent-authored']]], ready: [[10]]});
  await pick(w);
  check('one agent PR in flight, cap 2 -> still claims (the frozen-pipeline fix)', claimedIssue(w) === '10');

  w = world({claimed: [9], ready: [[10]]});
  await pick(w);
  check('one claimed issue in flight, cap 2 -> still claims', claimedIssue(w) === '10');

  w = world({prs: [[200, 'task/9', ['agent-authored']]], claimed: [9], ready: [[10]]});
  await pick(w);
  check('a story with BOTH its PR open and its issue claimed counts once', claimedIssue(w) === '10');

  w = world({prs: [[200, 'task/9', ['agent-authored']], [201, 'task/8', ['agent-authored']]], ready: [[10]]});
  await pick(w);
  check('two agent PRs in flight -> cap reached, no pull', claimedIssue(w) === null && w.log.some(l => /notice: WIP cap/.test(l)));

  w = world({prs: [[200, 'task/9', ['agent-authored']]], claimed: [8], ready: [[10]]});
  await pick(w);
  check('one PR + one different claimed issue -> two stories, no pull', claimedIssue(w) === null);

  w = world({prs: [[200, 'governance/x', ['agent-authored']], [201, 'task/9', ['agent-authored']]], ready: [[10]]});
  await pick(w);
  check('a non-task/<n> agent branch still counts as a story (fail closed on the join)', claimedIssue(w) === null);

  w = world({prs: [[200, 'task/9', ['risk:t2']]], ready: [[10]]});
  await pick(w);
  check('a human PR (no agent-authored label) never counts', claimedIssue(w) === '10');

  w = world({prs: [[200, 'task/9', ['agent-authored']]], ready: [[10, ['needs-human']], [11]]});
  await pick(w);
  check('needs-human ready story is skipped; the next clean one is claimed', claimedIssue(w) === '11');

  w = world({prs: [[200, 'task/9', ['agent-authored']]], ready: [[10]]});
  await pick(w, '1');
  check('wip_cap=1 restores the original behaviour exactly', claimedIssue(w) === null);

  w = world({ready: [[10]]});
  await pick(w, '');
  check('unreadable wip_cap -> conservative cap 1 (fail closed), still claims when idle', claimedIssue(w) === '10' && w.log.some(l => /notice: .*wip_cap/.test(l)));

  w = world({prs: [[200, 'task/9', ['agent-authored']]], ready: [[10]]});
  await pick(w, 'zzz');
  check('unreadable wip_cap with one in flight -> behaves as cap 1, no pull', claimedIssue(w) === null);

  for (const [n, ok] of results) console.log((ok ? 'ok   ' : 'FAIL ') + n);
  console.log(`${results.filter(r => r[1]).length}/${results.length} cases passed`);
})();
