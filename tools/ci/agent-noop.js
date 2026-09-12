'use strict';
// Shared "did the agent actually produce anything?" check for agent-dispatch.yml (implement) and
// review-fix.yml (fix). Both workflows require() this from the checkout, as ready-gate.yml and
// promote-queued.yml do with dep-refs.js — one implementation, one test suite
// (tests/workflows/agent_noop_harness.js directly; the wiring in test_workflow_scripts.py).
//
// The defect this closes (#361). A claude-code-action run can exit `success`, with
// `is_error: false`, having pushed no branch, opened no PR and posted no comment. It happened
// five times on 2026-09-07/08 and 2026-09-11: #54 twice, #342's fix twice, and #58 after 156
// turns, 26 permission denials and $9.07. Each run left its claim in place — `in-progress` on the
// issue, or a spent `review-fix-<n>` label on the PR — so the WIP cap stayed held and the loop
// stalled until a human looked. Both workflows already carried a step meant to catch exactly
// this, and neither could ever run: `agent-dispatch.yml:134` and `review-fix.yml:278` were
// `if: failure()`, and a silent no-op is not a failure. Detection must therefore run on SUCCESS.
//
// What counts as production is deliberately generous, because the cost of a false no-op (a real
// PR abandoned, a claim released under a working agent) is worse than the cost of a missed one
// (the status quo, which a human already has to notice):
//   implement — an OPEN pull request whose head is `task/<issue>`. Another issue's PR does not
//               count; neither does a closed one.
//   fix       — the PR head moved, OR claude[bot] said something during the run. review-fix is
//               explicitly allowed to rebut a finding and change no code (review-fix.yml step 5),
//               so a comment IS a real outcome. Verdict comments are excluded: claude-review and
//               red-team post `VERDICT(<kind>)` comments on the same PR, and one landing while
//               the fixer runs must not be mistaken for the fixer's own work.
// `is_error`, or an unreadable execution file, is a no-op whatever else is true — fail closed.
const fs = require('fs');

// claude-code-action's `execution_file` is JSON: either the result record itself or the message
// array whose last result-bearing entry carries the totals. Accept both rather than guess.
function readFigures(env, core) {
  const miss = { turns: null, denials: null, isError: null, ok: false };
  if (!env.EXEC_FILE) { core.warning('agent-noop: no execution file path — figures unavailable, treating as a no-op (fail closed)'); return miss; }
  let raw;
  try { raw = JSON.parse(fs.readFileSync(env.EXEC_FILE, 'utf8')); }
  catch (e) { core.warning(`agent-noop: execution file unreadable (${e.message}) — figures unavailable, treating as a no-op (fail closed)`); return miss; }
  const rec = Array.isArray(raw)
    ? [...raw].reverse().find(x => x && typeof x === 'object' && ('num_turns' in x || 'is_error' in x)) || {}
    : (raw && typeof raw === 'object' ? raw : {});
  const num = v => (Number.isFinite(v) ? v : null);
  return { turns: num(rec.num_turns), denials: num(rec.permission_denials_count), isError: rec.is_error === true, ok: true };
}

const lastLine = body => ((body || '').trim().split('\n').pop() || '').trim();
const isVerdict = body => /^VERDICT\([^)]*\):/.test(lastLine(body));

async function detect({ github, context, core, env }) {
  const { owner, repo } = context.repo;
  const kind = env.KIND;
  const f = readFigures(env, core);
  // Always report the figures, especially for a no-op: "156 turns, 26 denials, nothing produced"
  // is the line that tells a human this was a permissions problem, not an empty backlog.
  core.notice(`agent-noop: ${kind} run — turns=${f.turns} denials=${f.denials} is_error=${f.isError}`);

  let produced = false, reason = '';
  if (kind === 'implement') {
    const issue = String(env.ISSUE || '').trim();
    const prs = await github.paginate(github.rest.pulls.list, { owner, repo, state: 'open', per_page: 100 });
    produced = prs.some(p => p && p.state !== 'closed' && p.head && p.head.ref === `task/${issue}`);
    reason = produced ? `an open PR exists on task/${issue}` : `no open PR on task/${issue}`;
  } else {
    const number = Number(env.PR);
    const before = String(env.HEAD_BEFORE || '').trim();
    const pr = (await github.rest.pulls.get({ owner, repo, pull_number: number })).data;
    const moved = !!before && pr.head.sha !== before;
    // Only comments made DURING this run count; the verdict that triggered the run predates it.
    const since = Date.parse(env.SINCE || '');
    const comments = await github.paginate(github.rest.issues.listComments, { owner, repo, issue_number: number, per_page: 100 });
    const spoke = comments.some(c =>
      c.user && c.user.login === 'claude[bot]' && !isVerdict(c.body) &&
      (!Number.isFinite(since) || Date.parse(c.created_at || '') > since));
    produced = moved || spoke;
    reason = moved ? `head moved from ${before.slice(0, 7)} to ${pr.head.sha.slice(0, 7)}`
      : spoke ? 'claude[bot] commented during the run'
        : `head unchanged at ${before.slice(0, 7)} and claude[bot] said nothing`;
  }

  const noop = !f.ok || f.isError === true || !produced;
  if (noop) core.notice(`agent-noop: NO-OP — ${reason}${f.isError ? ' (is_error)' : ''}${f.ok ? '' : ' (figures unavailable)'}`);
  core.setOutput('noop', String(noop));
  core.setOutput('turns', String(f.turns));
  core.setOutput('denials', String(f.denials));
  return { noop, turns: f.turns, denials: f.denials, reason };
}

// A label that is already absent is a non-event (404), the same rule review-fix's exhaustion path
// and ci-failure-router follow; any other error is real and must not be swallowed.
async function dropLabel(github, owner, repo, number, name, core) {
  try { await github.rest.issues.removeLabel({ owner, repo, issue_number: number, name }); return true; }
  catch (e) {
    if (e && e.status === 404) return false;
    core.warning(`agent-noop: could NOT remove \`${name}\` from #${number} (${(e && e.status) || ''} ${(e && e.message) || e}) — do it by hand`);
    return false;
  }
}

async function finalize({ github, context, core, env }) {
  const { owner, repo } = context.repo;
  const kind = env.KIND;
  const runUrl = `${context.serverUrl}/${owner}/${repo}/actions/runs/${context.runId}`;
  const spent = `${env.ATTEMPTS} attempt(s), last run: turns=${env.TURNS}, permission denials=${env.DENIALS}`;

  if (kind === 'implement') {
    const n = Number(env.ISSUE);
    const released = await dropLabel(github, owner, repo, n, 'in-progress', core);
    // Never `ready`/`queued`: releasing a claim is not a release decision (GOVERNANCE §1).
    await github.rest.issues.createComment({
      owner, repo, issue_number: n,
      body: `🛑 Dispatch produced **nothing** — no branch, no PR, no comment — after ${spent}.\n\n` +
        `${released ? 'The `in-progress` claim has been released' : 'No `in-progress` claim was present'}, so the WIP cap is not held. ` +
        `The issue keeps whatever release label it had; re-dispatch is a human decision.\n\n` +
        `A high denial count usually means the agent hit the allow-list rather than the task. ` +
        `The transcript is attached to the run as an artifact: ${runUrl}`});
    core.setFailed(`agent-noop: dispatch for #${n} produced nothing after ${spent} — claim released, job failed so this is not silent (#361).`);
    return;
  }

  const pr = Number(env.PR);
  // The gate applies `review-fix-<n>` BEFORE this job runs (review-fix.yml:161), so a run that
  // produced nothing has already spent one of the four attempts. Hand it back: the bound exists
  // to cap real fix rounds, and an agent that never acted did not take one.
  const label = `review-fix-${env.ATTEMPT}`;
  const returned = await dropLabel(github, owner, repo, pr, label, core);
  await github.rest.issues.addLabels({ owner, repo, issue_number: pr, labels: ['needs-human'] });
  await github.rest.issues.createComment({
    owner, repo, issue_number: pr,
    body: `🛑 Review-fix produced **nothing** — no commit, no comment — after ${spent}.\n\n` +
      `${returned ? `\`${label}\` has been removed, so this attempt is **returned** and does not count against \`review_fix_max_attempts\`` : `\`${label}\` was already absent, so no attempt was counted`}. ` +
      `\`needs-human\` is applied: the findings are still unfixed, and a run that does nothing twice will do nothing a third time.\n\n` +
      `The transcript is attached to the run as an artifact: ${runUrl}`});
  core.setFailed(`agent-noop: review-fix on #${pr} produced nothing after ${spent} — attempt returned, needs-human applied, job failed (#361).`);
}

module.exports = { detect, finalize, readFigures, isVerdict };
