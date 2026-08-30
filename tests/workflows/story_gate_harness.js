// Executes the github-script bodies of ready-gate.yml and promote-queued.yml VERBATIM against
// a mocked GitHub API, so the story-release logic is tested on every push instead of only on
// real label events. Driven by tools/refimpl/test_workflow_scripts.py, which extracts the two
// scripts from the workflow YAML into a JSON file and passes its path as argv[2].
//
// Each case builds a small issue world, runs one script with a synthetic `context`, and asserts
// on the API calls it made. Exit code 1 on any failure; the failing case names are printed.
'use strict';
const fs = require('fs');
const S = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));

const NAMES = ['Intent', 'Spec references', 'Acceptance criteria', 'Evidence required',
               'Out of scope', 'Dependencies', 'Expected risk tier'];
const body = (deps, tier = '**T1** — host-only tooling; no T3 task exists in this feature', style = '### ') =>
  [['Intent', 'Do the thing.'], ['Spec references', 'specs/002-trunk-link-layer/tasks.md T002'],
   ['Acceptance criteria', '- [ ] it builds'], ['Evidence required', 'pipeline green'],
   ['Out of scope', 'everything else'], ['Dependencies', deps], ['Expected risk tier', tier]]
  .map(([n, v]) => (style === 'bold' ? `**${n}**` : `${style}${n}`) + '\n' + v).join('\n');

function world(issues, opts = {}) {
  const log = [];
  const state = new Map(issues.map(i => [i.number, i]));
  const github = {
    paginate: async (fn, args) => fn(args).then(r => r.data),
    rest: {
      issues: {
        get: async ({issue_number}) => { const i = state.get(issue_number); if (!i) throw new Error('404'); return {data: i}; },
        listForRepo: async ({labels}) => ({data: [...state.values()].filter(i => i.state === 'open' && labels.split(',').every(l => i.labels.some(x => x.name === l)))}),
        removeLabel: async ({issue_number, name}) => { const i = state.get(issue_number); i.labels = i.labels.filter(l => l.name !== name); log.push(`-${name}@${issue_number}`); },
        addLabels: async ({issue_number, labels}) => { const i = state.get(issue_number); for (const l of labels) i.labels.push({name: l}); log.push(`+${labels.join('+')}@${issue_number}`); },
        createComment: async ({issue_number, body}) => log.push(`comment@${issue_number}: ${body.replace(/\n+/g, ' | ').slice(0, 600)}`),   // whole comment, flattened
      },
      repos: { createDispatchEvent: async ({event_type, client_payload}) => {
        if (opts.dispatchFails) throw new Error('simulated dispatch failure');
        log.push(`dispatch ${event_type}#${client_payload.issue}`);
      } },
    },
  };
  const core = { info: () => {}, notice: m => log.push(`notice: ${m}`), warning: m => log.push(`warning: ${m}`) };
  return {github, core, log, state};
}
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
// The workflow scripts require() the shared parser from the checked-out repo; github-script
// supplies `require`, so the harness does too, rooted at the repository (argv[3] or cwd).
process.env.GITHUB_WORKSPACE = process.argv[3] || process.cwd();
async function gate(w, issueNo, label, eventName = 'issues') {
  const context = { repo: {owner: 'o', repo: 'r'}, eventName,
    payload: eventName === 'issues' ? {issue: w.state.get(issueNo), label: {name: label}} : {client_payload: {issue: issueNo}} };
  await new AsyncFunction('github', 'context', 'core', 'require', S.gate)(w.github, context, w.core, require);
}
async function promote(w, closedNo) {
  const context = { repo: {owner: 'o', repo: 'r'}, eventName: 'issues', payload: {issue: {number: closedNo}} };
  await new AsyncFunction('github', 'context', 'core', 'require', S.promote)(w.github, context, w.core, require);
}
const mk = (n, st, labels, deps, tier, style) => ({number: n, state: st, labels: labels.map(name => ({name})), body: body(deps, tier, style)});
const results = [];
const check = (name, cond) => { results.push([name, !!cond]); if (!cond) process.exitCode = 1; };
const has = (w, s) => w.log.includes(s);
const said = (w, re, n) => w.log.some(l => re.test(l) && (n === undefined || l.startsWith(`comment@${n}`)));

(async () => {
  let w;
  // --- ready path (unchanged behaviour) ---
  w = world([mk(1, 'open', ['task']), mk(2, 'open', ['task', 'ready'], '#1')]);
  await gate(w, 2, 'ready'); check('ready + open dep -> rejected, label removed', has(w, '-ready@2') && said(w, /Not ready/, 2));
  w = world([mk(1, 'closed', ['task']), mk(2, 'open', ['task', 'ready'], '#1')]);
  await gate(w, 2, 'ready'); check('ready + closed dep -> passed with predicted tier', said(w, /passed\*\* \(predicted T1\)/, 2));
  // --- queued path ---
  w = world([mk(1, 'open', ['task']), mk(2, 'open', ['task', 'queued'], '#1')]);
  await gate(w, 2, 'queued'); check('queued + open dep -> waits, names the dep', !w.log.some(l => l.startsWith('-queued')) && said(w, /queued.*waiting on #1/, 2));
  w = world([mk(1, 'closed', ['task']), mk(2, 'open', ['task', 'queued'], '#1')]);
  await gate(w, 2, 'queued'); check('queued + closed deps -> swapped to ready on the spot', has(w, '-queued@2') && has(w, '+ready@2') && said(w, /queued → ready/, 2));
  w = world([mk(2, 'open', ['task', 'queued'], 'None', 'T3 — touches protocol YAML')]);
  await gate(w, 2, 'queued'); check('queued + T3 -> rejected', has(w, '-queued@2') && said(w, /Not queued/, 2));
  w = world([{...mk(2, 'open', ['task', 'queued'], 'None'), body: body('None') + '\nREPLACE: x'}]);
  await gate(w, 2, 'queued'); check('queued + REPLACE placeholder -> rejected', has(w, '-queued@2'));
  w = world([mk(2, 'open', ['task', 'queued'], 'None', 'T1 — "no T3 task exists" is rationale, not the tier')]);
  await gate(w, 2, 'queued'); check('declared tier is the first token, not any T3 mention', has(w, '+ready@2'));
  // --- heading tolerance (#26) ---
  w = world([mk(1, 'closed', ['task']), mk(2, 'open', ['task', 'queued'], '#1', undefined, 'bold')]);
  await gate(w, 2, 'queued'); check('bold section titles are parsed', has(w, '+ready@2') && !said(w, /missing section/));
  w = world([mk(1, 'closed', ['task']), mk(2, 'open', ['task', 'ready'], '#1', undefined, '## ')]);
  await gate(w, 2, 'ready'); check('h2 headings are parsed', said(w, /passed\*\*/, 2));
  w = world([mk(2, 'open', ['task', 'queued'], 'None', undefined, '###')]);       // "###Intent": not an ATX heading
  await gate(w, 2, 'queued'); check('###Name without a space is not a heading (reported missing)', said(w, /missing section/, 2));
  // --- the #90 review bug: a bold aside inside a section must not end the section ---
  const aside = '#1\n**Note**\n#4';
  w = world([mk(1, 'closed', ['task']), mk(4, 'open', ['task']), mk(2, 'open', ['task', 'queued'], aside)]);
  await gate(w, 2, 'queued'); check('gate: bold aside inside Dependencies does not hide a later #ref', said(w, /waiting on #4/, 2) && !has(w, '+ready@2'));
  w = world([mk(1, 'closed', ['task']), mk(4, 'open', ['task']), mk(2, 'open', ['task', 'queued'], aside)]);
  await promote(w, 1); check('promoter: bold aside inside Dependencies does not promote early', !has(w, '+ready@2'));
  // --- promotion ---
  w = world([mk(1, 'closed', ['task']), mk(4, 'open', ['task']), mk(2, 'open', ['task', 'queued'], '#1'),
             mk(3, 'open', ['task', 'queued'], '#1, #4'), mk(5, 'open', ['task', 'queued'], 'None'),
             mk(6, 'open', ['task', 'queued', 'needs-human'], 'None')]);
  await promote(w, 1);
  check('promote: dependency closed -> ready + final-validation dispatch', has(w, '-queued@2') && has(w, '+ready@2') && has(w, 'dispatch ready-gate#2'));
  check('promote: another dependency open -> held', !has(w, '-queued@3'));
  check('promote: no dependencies declared -> promoted', has(w, '+ready@5') && said(w, /promoted.*none declared/, 5));
  check('promote: needs-human -> never promoted', !has(w, '+ready@6'));
  check('promote: notice counts', has(w, 'notice: promote-queued: promoted 2 of 4'));
  await gate(w, 2, undefined, 'repository_dispatch');
  check('dispatched final validation passes', said(w, /passed\*\* \(predicted T1\)/, 2));
  check('dispatched final validation also nudges the dispatcher', has(w, 'dispatch backlog-changed#2'));
  w = world([mk(2, 'open', ['task'], 'None')]);
  await gate(w, 2, undefined, 'repository_dispatch'); check('dispatch on an issue no longer labelled ready is a no-op', w.log.length === 1 && /no longer labelled ready/.test(w.log[0]));
  // --- dependency rule (docs/DEFINITION-OF-READY.md "Dependencies rule") ---
  let deps = '- #1 (T001) -- open\n\nNot a blocking dependency, but directly relevant: #4 (T009).';
  w = world([mk(1, 'closed', ['task']), mk(4, 'open', ['task']), mk(2, 'open', ['task', 'queued'], deps)]);
  await gate(w, 2, 'queued'); check('prose non-blocking mention is ignored', has(w, '+ready@2'));
  deps = '- #1 (T001)\n- Not dependent on, and not blocked by, its sibling test tasks #4 (T016)';
  w = world([mk(1, 'closed', ['task']), mk(4, 'open', ['task']), mk(2, 'open', ['task', 'queued'], deps)]);
  await gate(w, 2, 'queued'); check('"not blocked by" list item is ignored', has(w, '+ready@2'));
  deps = '- **#1 (T001)** closed\n- **#4 (T035)** OPEN and expected to remain so while this test is written';
  w = world([mk(1, 'closed', ['task']), mk(4, 'open', ['task']), mk(2, 'open', ['task', 'queued'], deps)]);
  await gate(w, 2, 'queued'); check('"expected to remain open" item is ignored', has(w, '+ready@2'));
  deps = '- Must land first (Phase 1/2): #1 (T001),\n  #4 (T003)';
  w = world([mk(1, 'closed', ['task']), mk(4, 'open', ['task']), mk(2, 'open', ['task', 'queued'], deps)]);
  await gate(w, 2, 'queued'); check('ref on a continuation line of a list item counts', said(w, /waiting on #4/, 2));
  deps = 'Blocked on the full Setup phase, all currently open:\n#1 (T001), #4 (T003).';
  w = world([mk(1, 'closed', ['task']), mk(4, 'open', ['task']), mk(2, 'open', ['task', 'queued'], deps)]);
  await gate(w, 2, 'queued'); check('prose-only section counts every ref', said(w, /waiting on #4/, 2));
  deps = '- #1–#4 (every preceding task)';
  w = world([mk(1, 'closed', ['task']), mk(3, 'open', ['task']), mk(4, 'closed', ['task']), mk(2, 'open', ['task', 'queued'], deps)]);
  await gate(w, 2, 'queued'); check('#A–#B range expands to the issues between', said(w, /waiting on #3/, 2));
  w = world([mk(1, 'closed', ['task']),
             {...mk(26, 'open', ['task', 'queued'], '- #1 (T001)\n\nNot a blocking dependency, but relevant: #27 (T009).')},
             {...mk(27, 'open', ['task', 'queued'], '- **#1 (T001)**\n- **#26 (T008)** — no file dependency, [P] siblings, same PR')}]);
  await promote(w, 1); check('promoter: former #26↔#27 cycle — both promote', has(w, '+ready@26') && has(w, '+ready@27'));

  // --- #92 review fixes, exercised through the WIRED workflows ---
  deps = '- #4 (T010) must land first; #1 is a sibling test, not blocking';
  w = world([mk(1, 'closed', ['task']), mk(4, 'open', ['task']), mk(2, 'open', ['task', 'ready'], deps)]);
  await gate(w, 2, 'ready'); check('mixed bullet: the real blocker still blocks ready', has(w, '-ready@2') && said(w, /dependency #4 is still open/, 2));
  w = world([mk(1, 'closed', ['task']), mk(4, 'open', ['task']), mk(2, 'open', ['task', 'queued'], deps)]);
  await promote(w, 1); check('mixed bullet: the promoter does not promote past the real blocker', !has(w, '+ready@2'));
  deps = '- #1–4 (every preceding task)';
  w = world([mk(1, 'closed', ['task']), mk(3, 'open', ['task']), mk(4, 'closed', ['task']), mk(2, 'open', ['task', 'queued'], deps)]);
  await gate(w, 2, 'queued'); check('#A–B range (bare second endpoint) expands', said(w, /waiting on #3/, 2));
  let flood = ''; for (let n = 1000; flood.length < 40000; n += 201) flood += `- #${n}-${n + 200} (batch)\n`;
  w = world([mk(2, 'open', ['task', 'ready'], flood)]);
  await gate(w, 2, 'ready'); check('ref flood: gate treats a capped section as a gap (fails closed)', has(w, '-ready@2') && said(w, /more than 500 issues/, 2));
  w = world([mk(1, 'closed', ['task']), mk(2, 'open', ['task', 'queued'], flood)]);
  await promote(w, 1); check('ref flood: promoter holds the story (fails closed)', !has(w, '+ready@2') && w.log.some(l => /warning:.*more than 500/.test(l)));

  // --- the gate nudges the dispatcher whenever a story becomes claimable ---
  w = world([mk(1, 'closed', ['task']), mk(2, 'open', ['task', 'ready'], '#1')]);
  await gate(w, 2, 'ready'); check('ready pass -> repository_dispatch backlog-changed', has(w, 'dispatch backlog-changed#2'));
  w = world([mk(1, 'closed', ['task']), mk(2, 'open', ['task', 'queued'], '#1')]);
  await gate(w, 2, 'queued'); check('queued -> ready swap -> repository_dispatch backlog-changed', has(w, 'dispatch backlog-changed#2'));
  w = world([mk(1, 'open', ['task']), mk(2, 'open', ['task', 'queued'], '#1')]);
  await gate(w, 2, 'queued'); check('queued that waits does NOT nudge the dispatcher', !w.log.some(l => /backlog-changed/.test(l)));
  w = world([mk(1, 'open', ['task']), mk(2, 'open', ['task', 'ready'], '#1')]);
  await gate(w, 2, 'ready'); check('rejected ready does NOT nudge the dispatcher', !w.log.some(l => /backlog-changed/.test(l)));
  // dispatch is best-effort: a transient createDispatchEvent failure must not turn an
  // already-passed DoR gate into a red run (review of #93, Copilot + red-team finding 2)
  w = world([mk(1, 'closed', ['task']), mk(2, 'open', ['task', 'ready'], '#1')], {dispatchFails: true});
  await gate(w, 2, 'ready');
  check('dispatch failure does not fail the gate run', said(w, /passed\*\*/, 2));
  check('dispatch failure is surfaced as a warning, not a thrown error', w.log.some(l => l.startsWith('warning:') && /backlog-changed/.test(l)));

  // --- parsing cost on attacker-reachable input (#90 review finding 5): 64 KB adversarial body ---
  const adversarial = ('**Intent**\n' + '**Note**\n'.repeat(3000) + '## x\n'.repeat(3000)).slice(0, 65536);
  w = world([{number: 2, state: 'open', labels: [{name: 'task'}, {name: 'queued'}], body: adversarial}]);
  const t0 = Date.now(); await gate(w, 2, 'queued'); const ms = Date.now() - t0;
  check(`64 KB adversarial body parses in < 1000 ms (took ${ms} ms)`, ms < 1000);

  for (const [n, ok] of results) console.log((ok ? 'ok   ' : 'FAIL ') + n);
  console.log(`${results.filter(r => r[1]).length}/${results.length} cases passed`);
})();
