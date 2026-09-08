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

  // --- The NOT EXAMINED disclosure is not a follow-up list (2026-09-08) ------------------------
  // Verdicts write the marker EMPHASISED (`**NOT EXAMINED:**`) as often as bare, and the section
  // is separated from the follow-ups by a horizontal rule. Testing only the bare prefix let the
  // whole disclosure through: eight verdicts on #145/#149 filed ~57 scope-disclosure bullets as
  // `task` issues (#264-#270, #279-#290, #296-#306, #308-#313, #315-#318, #322-#335). The shape
  // below is the FOLLOW-UPS region of the red-team verdict on #149 @`18ef2a8a`, verbatim in form.
  const disclosure =
    `- The \`deep-verify (T2/T3 only)\` check, which is \`pending\` at this head — not a task\n` +
    `- Scenario suite, \`diffcheck\`, \`refimpl\`: taken from the green CI checks, not re-run\n` +
    `- No code from the diff was built, executed or fuzzed in this job.`;
  const realShape = (marker, lead = 'None. Everything I confirmed is inside the change.') =>
    `## FOLLOW-UPS\n\n${lead}\n\n---\n\n${marker}\n\n${disclosure}\n\nVERDICT(red-team): findings @ ${HEAD}`;

  for (const marker of ['**NOT EXAMINED:**', '__NOT EXAMINED:__', '*NOT EXAMINED*',
                        '**NOT EXAMINED**', '### NOT EXAMINED', '## NOT EXAMINED', 'NOT EXAMINED:']) {
    w = world();
    await run(w, {body: realShape(marker)});
    check(`the disclosure under ${marker} files nothing`, w.created.length === 0);
  }

  // A prose "None." is how a verdict says it proposes nothing; only a bullet was recognised.
  w = world();
  await run(w, {body: `## FOLLOW-UPS\n\nNone. Everything I confirmed is inside the change.\n\nVERDICT(review): clean @ ${HEAD}`});
  check('a prose "None." files nothing', w.created.length === 0);
  w = world();
  await run(w, {body: comment('- **None** — nothing outside the change')});
  check('an emphasised "none" bullet files nothing', w.created.length === 0);

  // The bound that matters: a real proposal BEFORE the marker must still be filed. If the stop
  // moved too early this case would file 0 and the fix would be silently losing follow-ups.
  w = world();
  await run(w, {body: realShape('**NOT EXAMINED:**', '- **Bound the ESP32-S3 UART RX path** — real proposal')});
  check('a real follow-up before the marker is still filed, the disclosure is not',
        w.created.length === 1 && /Bound the ESP32-S3 UART RX path/.test(w.created[0].title));

  // --- Dedup survives the reviewer rephrasing between rounds (2026-09-08) ----------------------
  // Exact-title dedup filed one standing proposal seven times as the wording drifted
  // ("answer via" / "answer through"): #253, #258, #262, #275, #278, #291, #292.
  w = world({existing: [{title: "Make `MockWire::Kind::Respond` answer via the node's `RequestHandler`", labels: ['task']}]});
  await run(w, {body: comment("- **Make `MockWire::Kind::Respond` answer through the node's `RequestHandler`** — round 5")});
  check('a rephrased duplicate (via/through) is not re-filed', w.created.length === 0);

  w = world({existing: [{title: 'Bound Responder::HeldBytes per-byte timestamps', labels: ['task']}]});
  await run(w, {body: comment("- **Bound `Responder::HeldBytes`' per-byte timestamps** — 1.1 KB of RAM")});
  check('a duplicate differing only in markdown and punctuation is not re-filed', w.created.length === 0);

  // The safety bound on that: over-merging would silently drop a genuine follow-up, which is
  // worse than a duplicate. A different proposal sharing a leading verb must still be filed.
  w = world({existing: [{title: 'Bound the descriptor TLV loop', labels: ['task']}]});
  await run(w, {body: comment('- Bound the ESP32-S3 UART RX path against a Responder drain stop — different thing')});
  check('a genuinely different proposal sharing a verb is still filed', w.created.length === 1);
  w = world({existing: [{title: 'Count a frame-level discard with no window open', labels: ['task']}]});
  await run(w, {body: comment('- Count a frame-level discard against a bus-level counter — related but distinct')});
  check('a related-but-distinct proposal is still filed', w.created.length === 1);

  // Two rephrasings of the same thing inside ONE comment collapse to one issue.
  w = world();
  await run(w, {body: comment("- Make `MockWire::Kind::Respond` answer via the node's `RequestHandler`\n" +
                              "- Make `MockWire::Kind::Respond` answer through the node's `RequestHandler`")});
  check('two rephrasings in one comment file one issue', w.created.length === 1);

  const pass = results.filter(([, ok]) => ok).length;
  for (const [n, ok] of results) console.log(`${ok ? 'ok  ' : 'FAIL'} ${n}`);
  console.log(`${pass}/${results.length} cases passed`);
})();
