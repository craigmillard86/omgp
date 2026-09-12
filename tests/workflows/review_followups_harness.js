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
        w.created.length === 2 && w.created[0].title === 'Bound the descriptor TLV loop' &&
        w.created[1].title === 'Wire quality into CI');
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
  // NOTE (#341 review, [MEDIUM]): this template must NOT put a `---` rule before ${marker}. An
  // earlier revision did, the scan broke on the rule, and all seven marker cases passed without
  // ever reading the marker line — seven demonstrations of the rule stop, none of the fix.
  const realShape = (marker, lead = 'None. Everything I confirmed is inside the change.') =>
    `## FOLLOW-UPS\n\n${lead}\n\n${marker}\n\n${disclosure}\n\nVERDICT(red-team): findings @ ${HEAD}`;

  for (const marker of ['**NOT EXAMINED:**', '__NOT EXAMINED:__', '*NOT EXAMINED*',
                        '**NOT EXAMINED**', '### NOT EXAMINED', '## NOT EXAMINED', 'NOT EXAMINED:']) {
    w = world();
    await run(w, {body: realShape(marker)});
    check(`the disclosure under ${marker} files nothing`, w.created.length === 0);
  }
  // The corpus shape puts a rule between the follow-ups and the marker. That must still work, but
  // it must work BECAUSE of the marker, which is what the rule-free cases above establish.
  w = world();
  await run(w, {body: `## FOLLOW-UPS\n\nNone.\n\n---\n\n**NOT EXAMINED:**\n\n${disclosure}\n\nVERDICT(red-team): findings @ ${HEAD}`});
  check('the corpus shape (rule, then an emphasised marker) files nothing', w.created.length === 0);

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

  // --- The section must not end early (#341 red team, findings 1, 2 and 5) --------------------
  // Reproducers are fenced, and the brief REQUIRES one per finding. A `# ` line in one is a shell
  // comment, not a heading; 20 such lines exist across 8 of the 92 corpus comments. A scan that
  // broke there would drop every later proposal with no signal. The scan is fence-BLIND, as on
  // main (#341 round 5, maintainer direction): a `## ` line inside a fence would still end the
  // section. That is pre-existing, 0 occurrences in the 99-comment corpus, and tracked in #376.
  w = world();
  await run(w, {body: '## FOLLOW-UPS\n- First proposal — worth doing\n\n```bash\n# regenerate the vectors\n' +
                      'python tools/genvectors.py\n```\n\n- Second proposal — also worth doing\n\n' +
                      `VERDICT(review): findings @ ${HEAD}`});
  check('a fenced `# ` reproducer does not end the section', w.created.length === 2);
  w = world();
  await run(w, {body: '## FOLLOW-UPS\n- First proposal — worth doing\n\n~~~\n# tilde-fenced too\n~~~\n\n' +
                      `- Second proposal — also worth doing\n\nVERDICT(review): findings @ ${HEAD}`});
  check('a tilde-fenced `# ` reproducer does not end the section', w.created.length === 2);

  // Verdicts use `---` freely as an in-prose separator (22 of the 92 corpus comments), including
  // BETWEEN findings. It must not be mistaken for the end of the follow-up list.
  w = world();
  await run(w, {body: `## FOLLOW-UPS\n- First proposal — worth doing\n\n---\n\n- Second proposal — also worth doing\n\nVERDICT(review): findings @ ${HEAD}`});
  check('a `---` rule between proposals does not end the section', w.created.length === 2);

  // The verdict line ends it too. Nothing bound that half of the stop test until now (#341
  // round-5 mutation probe): a bullet after the verdict line would have been filed as a task.
  w = world();
  await run(w, {body: `## FOLLOW-UPS\n- First proposal — worth doing\n\nVERDICT(review): findings @ ${HEAD}\n` +
                      `- a bullet after the verdict line, not a proposal`});
  check('the VERDICT line ends the section', w.created.length === 1 &&
        !w.created.some(i => /after the verdict line/.test(i.title)));

  // A real heading still ends it.
  w = world();
  await run(w, {body: `## FOLLOW-UPS\n- First proposal — worth doing\n\n## What held\n- not a follow-up\n\nVERDICT(review): findings @ ${HEAD}`});
  check('a real `## ` heading still ends the section', w.created.length === 1);

  // The heading stop stays at exactly `##`, as it was before this change. A deeper heading is a
  // sub-point of the section, not the end of it — `### NOT EXAMINED` still stops, via the marker.
  w = world();
  await run(w, {body: `## FOLLOW-UPS\n- First proposal — worth doing\n\n### Cheaper alternative\n\n- Second proposal — also worth doing\n\nVERDICT(review): findings @ ${HEAD}`});
  check('a `### ` sub-heading does not end the section', w.created.length === 2);

  // "None" as the first word of a genuine proposal is not the same as a "none" list.
  w = world();
  await run(w, {body: comment('- None of the scenarios assert `T_resp` under a full superframe — add one')});
  check('a proposal whose first word is "None" is still filed', w.created.length === 1);

  // --- The dedup threshold is calibrated, and both directions are pinned ----------------------
  // `not`/`no`/`against` were stopwords, so a claim and its negation had identical token sets
  // (#341 red team, finding 3). Sense-carrying words must survive normalisation.
  w = world({existing: [{title: 'Frame-level discards are counted in the bus-level counter', labels: ['task']}]});
  await run(w, {body: comment('- Frame-level discards are not counted in the bus-level counter — the opposite claim')});
  check('a proposal and its negation are not merged', w.created.length === 1);
  w = world({existing: [{title: 'Count a frame-level discard with no window open', labels: ['task']}]});
  await run(w, {body: comment('- Count a frame-level discard with a window open — the other case')});
  check('"with no X" and "with a X" are not merged', w.created.length === 1);

  // A negation inserted into a LONG title (#341 red team round 4, F3 and FU-1). At >= 9 content
  // tokens a one-token superset clears the 0.9 threshold, so "X" and "X never" merged and the
  // opposite proposal was silently lost. The short case above passes on a 0.875 margin, not on
  // the property. A pair whose token sets differ by a negation word is never the same proposal.
  w = world({existing: [{title: 'The scheduler should retry a lost poll and log the SUSPECT transition', labels: ['task']}]});
  await run(w, {body: comment('- The scheduler should never retry a lost poll and log the SUSPECT transition — the opposite')});
  check('a negation inserted into a 9-token title is not merged', w.created.length === 1);
  w = world({existing: [{title: 'The descriptor parser rejects an oversized TLV before the length check runs', labels: ['task']}]});
  await run(w, {body: comment('- The descriptor parser never rejects an oversized TLV before the length check runs — the opposite')});
  check('a negation inserted into a longer title is not merged', w.created.length === 1);
  // A contraction is the same negation, so "doesn't" and "does not" state one claim.
  w = world({existing: [{title: 'The engine does not count a discarded request beyond the hold', labels: ['task']}]});
  await run(w, {body: comment("- The engine doesn't count a discarded request beyond the hold — restated")});
  check('"doesn\'t" and "does not" are the same claim, so they merge', w.created.length === 0);

  // Two distinct boundaries differing in one content word (#341 red team, finding 4): at the old
  // 0.8 these collapsed to one issue. This case fails if the threshold is lowered back.
  w = world({existing: [{title: 'The suite does not cover zero length payload frames', labels: ['task']}]});
  await run(w, {body: comment('- The suite does not cover maximum length payload frames — a different boundary')});
  check('two boundaries differing in one word are not merged', w.created.length === 1);

  // ...and the other direction, so the threshold cannot simply be raised to 1.0 to pass the above.
  w = world({existing: [{title: 'Bound the ESP32-S3 UART RX path against a Responder drain stop', labels: ['task']}]});
  await run(w, {body: comment('- Bound the ESP32-S3 UART RX path against the Responder drain stop — restated')});
  check('a restatement differing only in an article is still merged', w.created.length === 0);

  // The threshold pinned from ABOVE (#341 review, round 2): every merge case above is Jaccard 1.0,
  // so `>= 1` passed them all. This pair is 10 content tokens vs the same 10 plus one appended —
  // 10/11 = 0.909 — so it merges at 0.9 and fails at 1.0 or 0.95. With the 0.857 and 0.875 cases
  // above, the calibrated value is pinned to (0.875, 0.909].
  w = world({existing: [{title: 'Pin the ESP32 UART receive ring depth against a stalled Responder drain', labels: ['task']}]});
  await run(w, {body: comment('- Pin the ESP32 UART receive ring depth against a stalled Responder drain stop — restated')});
  check('a one-token superset of a 10-token title (J = 0.909) is still merged', w.created.length === 0);

  // ...and the same shape BELOW that length must not merge: 6 content tokens vs the same 6 plus one
  // is J = 6/7 = 0.857. A pin (it passes at 804d49a too), and the case that keeps a lowered
  // threshold visible now that the negation rule, not the threshold, carries the negation cases.
  w = world({existing: [{title: 'Count a frame-level discard with a window open', labels: ['task']}]});
  await run(w, {body: comment('- Count a frame-level discard with a window open on the bus — narrower')});
  check('a one-token superset of a 6-token title (J = 0.857) is not merged', w.created.length === 1);

  // --- Word order carries sense (#341 red team round 2, finding 1) -----------------------------
  // A token SET is order-blind: "A against B" and "B against A" were identical, J = 1.0, and the
  // second proposal was silently never filed. These are opposite proposals; each must file.
  w = world({existing: [{title: 'Count a frame-level discard against the bus-level counter', labels: ['task']}]});
  await run(w, {body: comment('- Count a bus-level discard against the frame-level counter — the other direction')});
  check('a permuted proposal (frame/bus swapped) is not merged', w.created.length === 1);
  w = world({existing: [{title: 'Reject a TLV whose length exceeds the frame', labels: ['task']}]});
  await run(w, {body: comment('- Reject a frame whose length exceeds the TLV — the converse check')});
  check('a converse proposal is not merged', w.created.length === 1);
  w = world({existing: [{title: 'Dedup closed issues against filed tasks', labels: ['task']}]});
  await run(w, {body: comment('- Dedup filed tasks against closed issues — the reverse direction')});
  check('a reversed proposal is not merged', w.created.length === 1);

  // --- An unbalanced fence (#341 red team round 2, finding 2) ----------------------------------
  // GitHub truncates a comment at 65 536 characters, and a verdict carries a fenced reproducer per
  // finding, so a long verdict can arrive with a fence that never closes. The round-2 fence handler
  // skipped to end-of-body, dropping every later proposal AND both stop markers. The fence-blind
  // scan cannot do that. These cases stay as guards against a future fence handler (#376)
  // reintroducing the swallow.
  w = world();
  await run(w, {body: '## FOLLOW-UPS\n- First proposal — worth doing\n\n```bash\n./build/native/scenario_runner tests/scenarios/f04.yaml -v\n\n' +
                      '- Second proposal — also worth doing\n\nNOT EXAMINED: nothing excluded\n- a disclosure bullet, not a task\n\n' +
                      `VERDICT(review): findings @ ${HEAD}`});
  check('an unclosed fence does not swallow later proposals', w.created.length === 2 &&
        w.created.some(i => i.title === 'Second proposal'));
  check('an unclosed fence does not swallow the NOT EXAMINED stop', !w.created.some(i => /disclosure bullet/.test(i.title)));

  // --- One missing closer must not re-pair every later fence (#341 red team round 3, finding 1) --
  // With a fence handler, an opener that lost its closer paired with the NEXT block's closing
  // fence: the proposal between vanished (R1), and one block later the marker itself was swallowed
  // (R2). The fence-blind scan is immune to both. The red team's inputs stay, verbatim in shape,
  // as guards for #376.
  const FENCE = '```';
  w = world();
  await run(w, {body: `## FOLLOW-UPS\n- Bound the descriptor TLV loop — real\n\n${FENCE}bash\n./build/native/scenario_runner f04.yaml -v\n` +
                      `\n- Wire quality into CI — real\n\n${FENCE}bash\n./pipeline.sh unit\n${FENCE}\n\nNOT EXAMINED: nothing excluded\n\n` +
                      `VERDICT(red-team): findings @ ${HEAD}`});
  check('an unclosed fence followed by another block does not swallow the proposal between them',
        w.created.length === 2 && w.created.some(i => i.title === 'Wire quality into CI'));
  w = world();
  await run(w, {body: `## FOLLOW-UPS\n- Bound the descriptor TLV loop — real\n\n${FENCE}bash\n./build/native/scenario_runner f04.yaml -v\n` +
                      `\n**NOT EXAMINED:**\n\n- the ESP32-S3 target build, not run\n- the scenario suite, taken from CI:\n\n` +
                      `${FENCE}\n./pipeline.sh scenarios\n${FENCE}\n\n- refimpl and diffcheck, also taken from CI\n` +
                      `- no code from the diff was fuzzed\n\nVERDICT(red-team): findings @ ${HEAD}`});
  check('an unclosed fence cannot carry the scan past the NOT EXAMINED marker',
        w.created.length === 1 && w.created[0].title === 'Bound the descriptor TLV loop');

  // --- Directional prepositions and conjunctions carry sense (#341 red team round 3, finding 2) --
  // With `to`/`from`/`into`/`and`/`or` stopped, each pair below was one token set in one order, so
  // sameOrder could not tell them apart and the second proposal was never filed.
  for (const [open, prop] of [['Copy the descriptor cache into the module', 'Copy the descriptor cache from the module'],
                              ['Report the queue depth to the host-core', 'Report the queue depth from the host-core'],
                              ['Reject a zero and a max length payload', 'Reject a zero or a max length payload']]) {
    w = world({existing: [{title: open, labels: ['task']}]});
    await run(w, {body: comment(`- ${prop} — the other case`)});
    check(`"${prop}" is not merged with "${open}"`, w.created.length === 1);
  }

  // --- #134 guardrail 2: MEDIUM+ only, "no backlog spam" (2026-09-12) ------------------------
  // #134 ruled it and the filer never implemented it: every bullet became an issue, every round,
  // on every PR. 152 of 190 open issues came from this filer; #172 alone produced 35. Verdicts
  // DO tag severity, so the threshold is mechanical. Note the tag is written BACKTICKED in real
  // verdicts (`[LOW]`), which is why both forms are tested — matching only the bare form would
  // leave the guard silently dead.
  w = world();
  await run(w, {body: comment('- `[LOW]` Rename the helper for clarity — cosmetic')});
  check('a backticked [LOW] follow-up is not filed', w.created.length === 0);
  w = world();
  await run(w, {body: comment('- [LOW] Rename the helper for clarity — cosmetic')});
  check('a bare [LOW] follow-up is not filed', w.created.length === 0);
  w = world();
  await run(w, {body: comment('- `[MEDIUM]` Bound the descriptor TLV loop — real hardening')});
  check('a [MEDIUM] follow-up is still filed', w.created.length === 1);
  w = world();
  await run(w, {body: comment('- `[HIGH]` Bound the descriptor TLV loop — real hardening')});
  check('a [HIGH] follow-up is still filed', w.created.length === 1);
  // Untagged bullets fail TOWARD filing: the reviewer omitting a tag must not silently drop a
  // real proposal. Recorded as a judgement in docs/OPEN-QUESTIONS.md, since #134 does not say.
  w = world();
  await run(w, {body: comment('- Make `MockWire::Kind::Respond` answer via the handler — untagged')});
  check('an untagged follow-up is still filed', w.created.length === 1);
  // The tag is stripped from the title either way, so a filed issue never carries it.
  w = world();
  await run(w, {body: comment('- `[MEDIUM]` Bound the descriptor TLV loop — real hardening')});
  check('the severity tag is stripped from the filed title',
        w.created.length === 1 && !/MEDIUM/.test(w.created[0].title));

  const pass = results.filter(([, ok]) => ok).length;
  for (const [n, ok] of results) console.log(`${ok ? 'ok  ' : 'FAIL'} ${n}`);
  console.log(`${pass}/${results.length} cases passed`);
})();
