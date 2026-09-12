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
// stalled until a human looked.
//
// Both workflows already carried a step meant to catch this, gated `if: failure()`. The precise
// claim matters, and the first version of this file got it wrong: that step was NOT dead. Run
// 33332211854 shows it firing — the Claude step failed, and "Release claim if no PR was opened"
// ran and released the claim (round-1 review on #416). What it could not do is fire for a run
// that EXITS SUCCESS having produced nothing, which is every one of the five. Detection therefore
// runs on success — and, because the old step covered failures too, an UNKNOWN outcome (detect
// itself throwing, an empty `noop` output) must still release the claim rather than return.
//
// What counts as production is deliberately generous, because the cost of a false no-op (a real
// PR abandoned, a claim released under a working agent) is worse than the cost of a missed one
// (the status quo, which a human already has to notice):
//   implement — an OPEN pull request on `task/<issue>` that this run created or pushed to. A PR
//               left open by an EARLIER dispatch does not count: #58's claim was released and the
//               issue re-picked while PR #401 was still open, so a second silent run would have
//               been vouched for by the first run's work. With no SINCE recorded, it counts.
//   fix       — the PR head moved, OR claude[bot] said something during the run. review-fix is
//               explicitly allowed to rebut a finding and change no code (review-fix.yml step 5),
//               so a comment IS a real outcome. Verdict comments are excluded on ANY line, since
//               the repo writes them backticked and often with a trailing attribution footer.
// `is_error` is a no-op whatever else is true. Missing figures fail closed only where production
// is UNKNOWN — once established, production is not un-established by an unreadable file.
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

// A verdict comment is claude-review's or red-team's, never the fixer's. Matching only the LAST
// line let three real shapes through (round-1 red team): the repo writes verdicts backticked, a
// review comment ends with the standard attribution footer, and a trailing `---` rule is common.
// So: any line, unwrapped from backticks/emphasis. This errs toward EXCLUDING a comment — a fixer
// comment that merely quotes a verdict stops counting as output — which errs toward declaring a
// no-op. That direction is loud (needs-human, a comment, a failed job), never silent.
const unwrap = line => (line || '').trim().replace(/^[`*_>\s]+/, '').replace(/[`*_\s]+$/, '');
const isVerdict = body => (body || '').split('\n').some(l => /^VERDICT\([^)]*\):/.test(unwrap(l)));

// How many times a no-op run may be retried in-job before the claim is released (#361 AC2).
// Read at RUN time from the DEFAULT-branch config, like every other knob here, so retuning it
// needs no workflow-scope push. Fail closed to 0 on anything unreadable, absent, negative or
// non-numeric, and clamp above: a typo must never buy an agent more attempts than the ruling
// allows. The clamp mirrors auto_fix_max_attempts' "values above 10 are clamped DOWN".
const RETRY_CLAMP = 2;

function retryBudget(env, core) {
  let text;
  try { text = fs.readFileSync(env.CONFIG, 'utf8'); }
  catch (e) { core.warning(`agent-noop: agent_retry_max unreadable (${e.message}) — no retry (fail closed)`); return 0; }
  const m = /^agent_retry_max: *(-?\w+)/m.exec(text);
  if (!m) { core.notice('agent-noop: agent_retry_max absent from agent-config.yml — no retry (fail closed)'); return 0; }
  const raw = m[1];
  if (!/^\d+$/.test(raw)) { core.notice(`agent-noop: agent_retry_max=${raw} is not a whole number — no retry (fail closed)`); return 0; }
  const n = Number(raw);
  if (n > RETRY_CLAMP) { core.notice(`agent-noop: agent_retry_max=${n} exceeds the ${RETRY_CLAMP}-retry clamp — using ${RETRY_CLAMP}`); return RETRY_CLAMP; }
  return n;
}

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
    const since = Date.parse(env.SINCE || '');
    const prs = await github.paginate(github.rest.pulls.list, { owner, repo, state: 'open', per_page: 100 });
    const mine = prs.filter(p => p && p.state !== 'closed' && p.head && p.head.ref === `task/${issue}`);
    // "Is there a PR now?" is not "did THIS run produce one". #58's claim was released and the
    // issue re-picked while PR #401 was still open on task/58 — a second silent run would then be
    // vouched for by the first run's PR, which is the stall #361 exists to catch (round-1 red
    // team). A PR counts only if it was opened or pushed to after this run started. With no
    // SINCE (nothing recorded the start) it counts, because a false no-op is the worse error.
    const fresh = p => !Number.isFinite(since) ||
      Date.parse(p.created_at || '') > since || Date.parse(p.updated_at || '') > since;
    produced = mine.some(fresh);
    reason = produced ? `an open PR on task/${issue} was created or updated by this run`
      : mine.length ? `the only open PR on task/${issue} predates this run`
        : `no open PR on task/${issue}`;
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

  // `!f.ok ||` used to short-circuit here, so a cancelled or timed-out run — both steps are
  // `if: always()` — was declared a no-op even when detection had just established that a PR
  // exists or the head moved, releasing a live claim and posting a comment its own `reason`
  // contradicted (round-1 red team). Missing figures fail closed only where production is
  // UNKNOWN; where it is known and positive, it stands.
  const noop = !produced || f.isError === true;
  if (noop) core.notice(`agent-noop: NO-OP — ${reason}${f.isError ? ' (is_error)' : ''}${f.ok ? '' : ' (figures unavailable)'}`);
  core.setOutput('noop', String(noop));
  core.setOutput('reason', reason);   // finalize reports THIS, not a hard-coded sentence
  core.setOutput('turns', String(f.turns));
  core.setOutput('denials', String(f.denials));
  return { noop, turns: f.turns, denials: f.denials, reason };
}

// A label that is already absent is a non-event (404), the same rule review-fix's exhaustion path
// and ci-failure-router follow; any other error is real and must not be swallowed.
// -> 'removed' | 'absent' | 'failed'. Both callers report this to a human, and conflating
// "already absent (404)" with "removal failed (500)" made finalize assert that no claim was held
// when the claim was held and the WIP cap was stuck (round-1 review).
async function dropLabel(github, owner, repo, number, name, core) {
  try { await github.rest.issues.removeLabel({ owner, repo, issue_number: number, name }); return 'removed'; }
  catch (e) {
    if (e && e.status === 404) return 'absent';
    core.warning(`agent-noop: could NOT remove \`${name}\` from #${number} (${(e && e.status) || ''} ${(e && e.message) || e}) — do it by hand`);
    return 'failed';
  }
}

async function finalize({ github, context, core, env }) {
  const { owner, repo } = context.repo;
  const kind = env.KIND;
  const runUrl = `${context.serverUrl}/${owner}/${repo}/actions/runs/${context.runId}`;
  const spent = `${env.ATTEMPTS} attempt(s), last run: turns=${env.TURNS}, permission denials=${env.DENIALS}`;
  // detect computed an accurate reason; hard-coding "no branch, no PR, no comment" here made the
  // comment contradict it on the is_error path (round-1 review).
  const why = env.REASON ? `Detected: ${env.REASON}.` : 'It produced no branch, no PR and no comment.';
  const said = {
    removed: 'has been released',
    absent: 'was not present',
    failed: 'could NOT be released — do it by hand',
  };

  if (kind === 'implement') {
    const n = Number(env.ISSUE);
    const claim = await dropLabel(github, owner, repo, n, 'in-progress', core);
    // Never `ready`/`queued`: releasing a claim is not a release decision (GOVERNANCE §1).
    await github.rest.issues.createComment({
      owner, repo, issue_number: n,
      body: `🛑 Dispatch produced nothing usable after ${spent}.\n\n${why}\n\n` +
        `The \`in-progress\` claim ${said[claim]}${claim === 'failed' ? ', so the WIP cap may still be held' : ''}. ` +
        `The issue keeps whatever release label it had; re-dispatch is a human decision.\n\n` +
        `A high denial count usually means the agent hit the allow-list rather than the task. ` +
        `The per-attempt figures are above and in the job log: ${runUrl}`});
    core.setFailed(`agent-noop: dispatch for #${n} produced nothing after ${spent} — ${env.REASON || 'no output'}; claim ${claim}, job failed so this is not silent (#361).`);
    return;
  }

  const pr = Number(env.PR);
  // The gate applies `review-fix-<n>` BEFORE this job runs (review-fix.yml:161), so a run that
  // produced nothing has already spent one of the four attempts. Hand it back.
  const label = `review-fix-${env.ATTEMPT}`;
  const attempt = await dropLabel(github, owner, repo, pr, label, core);
  // Unguarded, a 403 here aborted before the comment below — losing both the escalation and the
  // explanation on the one path whose entire purpose is not being silent (round-1 review).
  let escalated = true;
  try { await github.rest.issues.addLabels({ owner, repo, issue_number: pr, labels: ['needs-human'] }); }
  catch (e) { escalated = false; core.warning(`agent-noop: could NOT apply \`needs-human\` to #${pr} (${(e && e.status) || ''}) — apply it by hand`); }
  await github.rest.issues.createComment({
    owner, repo, issue_number: pr,
    body: `🛑 Review-fix produced nothing usable after ${spent}.\n\n${why}\n\n` +
      `\`${label}\` ${attempt === 'removed' ? 'has been removed, so this attempt is **returned** and does not count against `review_fix_max_attempts`'
        : attempt === 'absent' ? 'was already absent, so no attempt was counted'
          : 'could NOT be removed — this attempt is still counted; remove the label by hand to return it'}. ` +
      `${escalated ? '`needs-human` is applied' : '⚠️ `needs-human` could NOT be applied — apply it by hand'}: the findings are still unfixed, ` +
      `and a run that does nothing twice will do nothing a third time.\n\n` +
      `The per-attempt figures are above and in the job log: ${runUrl}`});
  core.setFailed(`agent-noop: review-fix on #${pr} produced nothing after ${spent} — ${env.REASON || 'no output'}; attempt ${attempt}, needs-human ${escalated ? 'applied' : 'NOT applied'}, job failed (#361).`);
}

module.exports = { detect, finalize, readFigures, isVerdict, retryBudget };
