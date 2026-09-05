// Executes the `file` github-script body of .github/workflows/review-followups.yml VERBATIM
// against a mocked GitHub API, so the follow-up filer's parse/dedup/cap/label rules are tested
// on every push (CLAUDE.md rule 8). Driven by tools/refimpl/test_workflow_scripts.py, which
// extracts the script into a JSON file (argv[2]).
//
// Properties under test: only claude[bot] verdict comments file anything; only the "## FOLLOW-UPS"
// section is read, and only until the next heading / NOT EXAMINED / VERDICT; "none" files nothing;
// a leading [SEVERITY] and a " — why" tail are stripped from the title; open `task` titles dedup;
// the label is `task` (never ready/queued); a runaway is capped.
'use strict';
const fs = require('fs');
const S = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));

function world({existing = []} = {}) {
  const created = [];
  const log = [];
  const github = {
    paginate: async (fn, args) => fn(args).then(r => r.data),
    rest: {
      issues: {
        listForRepo: async ({labels}) => ({data: existing.filter(i => (i.labels || ['task']).includes(labels))}),
        create: async ({title, labels, body}) => { created.push({title, labels, body}); return {data: {html_url: `https://gh/issues/${created.length}`}}; },
      },
    },
  };
  const core = {info: () => {}, notice: m => log.push(m), warning: m => log.push(m)};
  return {github, core, created, log};
}

const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
async function run(w, {login = 'claude[bot]', body, number = 94, isPr = true} = {}) {
  const payload = {issue: {number, pull_request: isPr ? {url: 'u'} : undefined}, comment: {user: {login}, body}};
  const context = {repo: {owner: 'o', repo: 'r'}, payload};
  await new AsyncFunction('github', 'context', 'core', 'require', S.file)(w.github, context, w.core, require);
}

// A realistic reviewer comment: two follow-ups, then the required tail. The filer must read only
// the two bullets and stop at NOT EXAMINED (the third bullet is NOT a follow-up).
const HEAD = 'a'.repeat(40);
const comment = (followups) =>
  `Some blocking findings text.\n\n## FOLLOW-UPS\n${followups}\n\nNOT EXAMINED: nothing excluded\n` +
  `- this bullet is after NOT EXAMINED and must be ignored\nVERDICT(review): findings @ ${HEAD}`;

const results = [];
const check = (name, cond) => { results.push([name, !!cond]); if (!cond) process.exitCode = 1; };

(async () => {
  let w;

  // Two proposals -> two task issues; severity tag and " — why" tail stripped from the title.
  w = world();
  await run(w, {body: comment('- [MEDIUM] Bound the descriptor TLV loop — outside #45\'s criteria\n- Wire quality into CI — pre-existing gap')});
  check('two follow-ups -> two task issues filed', w.created.length === 2);
  check('title strips the [SEVERITY] tag and the " — why" tail',
        w.created[0].title === 'Bound the descriptor TLV loop' && w.created[1].title === 'Wire quality into CI');
  check('every filed issue is labelled task and NOTHING else that releases it',
        w.created.every(i => i.labels.length === 1 && i.labels[0] === 'task'));
  check('the bullet after NOT EXAMINED is not filed', !w.created.some(i => /must be ignored/.test(i.title)));

  // "none" -> nothing filed.
  w = world();
  await run(w, {body: comment('- none')});
  check('a "none" follow-up list files nothing', w.created.length === 0);

  // Dedup against an open task with the same title.
  w = world({existing: [{title: 'Wire quality into CI', labels: ['task']}]});
  await run(w, {body: comment('- Wire quality into CI — pre-existing gap\n- Bound the TLV loop — new')});
  check('an already-open task title is not re-filed', w.created.length === 1 && w.created[0].title === 'Bound the TLV loop');

  // No FOLLOW-UPS section -> nothing (belt: the job `if` also guards, but the script must too).
  w = world();
  await run(w, {body: `no section here\n\nNOT EXAMINED: nothing excluded\nVERDICT(review): clean @ ${HEAD}`});
  check('no FOLLOW-UPS section -> files nothing', w.created.length === 0);

  // Not a claude[bot] comment / not a verdict -> nothing.
  w = world();
  await run(w, {login: 'someone', body: comment('- Something — anything')});
  check('a non-claude[bot] author files nothing', w.created.length === 0);
  w = world();
  await run(w, {body: `## FOLLOW-UPS\n- No verdict line here — should not file`});
  check('a comment with no VERDICT( files nothing', w.created.length === 0);

  // Cap a runaway.
  w = world();
  const many = Array.from({length: 14}, (_, i) => `- Follow-up number ${i}`).join('\n');
  await run(w, {body: comment(many)});
  check('a runaway list is capped at 10', w.created.length === 10);

  // Comment on a plain issue (not a PR) -> nothing.
  w = world();
  await run(w, {isPr: false, body: comment('- Something — anything')});
  check('a comment on a plain issue files nothing', w.created.length === 0);

  const pass = results.filter(([, ok]) => ok).length;
  for (const [n, ok] of results) console.log(`${ok ? 'ok  ' : 'FAIL'} ${n}`);
  console.log(`${pass}/${results.length} cases passed`);
})();
