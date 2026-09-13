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
//               been vouched for by the first run's work. Commits pushed to `task/<issue>` with
//               NO PR also count (#58's own shape: `gh pr create` denied) but are PARTIAL — the
//               claim is kept and nothing is retried, and finalize still speaks and fails the job,
//               because AC1 names "a pushed branch AND an opened PR" (round-3 review).
//   fix       — the PR head moved, OR the FIXER commented during the run. review-fix is explicitly
//               allowed to rebut a finding and change no code (review-fix.yml step 5), so a comment
//               IS a real outcome — but only the fixer's. ci-failure-router's auto-fix,
//               claude-mention and agent-triage all post as claude[bot] on the same PR, so a bare
//               claude[bot] comment vouched for a 140-turn no-op (round-3 red team). The prompt
//               tells the fixer to open its comment with `review-fix(<attempt>) @ <head>`, and
//               that line is what makes a comment the fixer's. Verdict comments are excluded on
//               ANY line, since the repo writes them backticked and with an attribution footer.
// `is_error` is a no-op whatever else is true. Missing figures fail closed only where production
// is UNKNOWN — once established, production is not un-established by an unreadable file.
// An UNKNOWN run start (SINCE empty: `t0` skipped by an earlier step failure) is likewise an
// unknown outcome and fails closed — every window test would otherwise fall OPEN and stale
// evidence (an older PR, a stale branch, attempt 1's comment) would vouch for a run in which no
// agent ran at all, holding the claim the old `if: failure()` step released (round-3 red team).
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
// Also strip HTML tags and list markers: `- VERDICT(...)` and `<b>VERDICT(...)</b>` render as a
// verdict just the same (round-3 red team). A `<blockquote>` is a quote, like `>`.
const unwrap = line => (line || '').replace(/<[^>]+>/g, '').trim()
  .replace(/^(?:[-*+]|\d+[.)])\s+/, '').replace(/^[`*_\s]+/, '').replace(/[`*_\s]+$/, '');
const isQuoted = line => /^\s*(?:>|<blockquote)/i.test(line || '');
// Round 1 was right to stop matching only the LAST line; stripping `>` as well was the
// over-correction. review-fix's prompt tells the fixer to rebut a wrong finding and leave the
// code, and findings are identified BY their `VERDICT(...)` line — so a compliant rebuttal quotes
// it. Treating that as "this is a verdict comment" declared the workflow's own documented happy
// path a no-op: attempt returned, needs-human applied, job failed, on a run that did exactly what
// it was told (round-2 red team). A quoted line is someone being quoted; an unquoted one is the
// comment's own verdict.
const isVerdict = body => (body || '').split('\n').some(l => !isQuoted(l) && /^VERDICT\([^)]*\):/.test(unwrap(l)));
// The fixer's marker: the first non-empty line of its comment, as review-fix.md step 5 instructs.
// CONTROL, NOT GUARANTEE (rule 11): a fixer that ignores the instruction is declared a no-op, which
// is the loud direction (needs-human, a comment, a failed job), never silent.
const FIXER_MARK = /^review-fix\(\d+\) @ [0-9a-f]{7,40}\b/i;
const isFixer = body => FIXER_MARK.test(unwrap((body || '').split('\n').find(l => l.trim()) || ''));

// Closing references, in the four spellings GitHub documents — `KEYWORD #n`, `KEYWORD GH-n`,
// `KEYWORD owner/repo#n`, `KEYWORD <issue URL>` (bare, autolinked or markdown-linked), keyword in
// any case, optionally emphasised, optional colon — matched AFTER code (fenced, indented, inline)
// and HTML comments are removed, since GitHub does not honour a reference inside those.
// CONTROL, NOT GUARANTEE (rule 11): GitHub's own parser is the authority; this covers the
// spellings named here and nothing else. Cross-repo forms count under ANY owner, as the URL form
// always did — GitHub honours them, permission-gated — so `fixes core/scheduler#3` in prose is a
// documented false positive in the loud direction (one escalation), not a lost auto-close
// (round-5 red team on #466 reversed round 4's owner filter).
//
// `closingRefs` returns each reference AS SPELLED — the whole matched keyword-and-reference
// text, whitespace runs collapsed — because the guard's job is to notice any change to what the
// merge gate will act on, and agent-merge's own parser reads only `KEYWORD<space>#n`: folding
// `o/r#465` to `465` (round 4), or `Closes:#n`, `**Closes** #n` and `Closes [#n](url)` to `#n`
// (round 5), hid rewrites that leave in-progress on an issue the merge should have released.
// `closingIssues` canonicalises same-repo forms to numbers for the RELEASE paths.
// What the closing-reference guard protects is what the merge automation READS: agent-merge's
// release/close path and this workflow's exhaustion release regex the raw body, so the guard
// compares the raw body's references, each as spelled. It does NOT model what GitHub's renderer
// honours (a reference inside code or an HTML comment is not auto-closed by GitHub). Rounds 4-9
// on #466 tried to: every round found another CommonMark spelling the hand-written stripper
// did not know, and the stripper itself opened holes (a code span pairing across a blank line,
// a literal `<!--` blinding the rest of the body). A partial markdown parser is a control that
// cannot be labelled honestly (rule 11), so it is withdrawn. RESIDUAL, stated: on a HAND merge
// (T3), where agent-merge never runs, closure depends on GitHub's auto-close, and a fixer that
// wraps a live reference in code leaves that issue open — visible in the issue, recoverable by
// a human, forbidden by the prompt, and closed by the follow-up that closes raw-referenced
// issues on any merge. On the autonomous path agent-merge closes from the raw view regardless.
// Two readers, unioned. CLOSING knows the four spellings GitHub documents; MERGE_RE is
// agent-merge.yml's release/close regex VERBATIM (pinned equal by
// test_reference_guard_reads_at_least_what_agent_merge_reads), so whatever CLOSING misses that
// agent-merge would act on — `a**closes #131`, where a lookbehind failed and `\b` matches at the
// keyword (round-11 red team on #466) — is read anyway: the superset holds by construction, not
// by the wider regex being right. The release path (closingIssues) uses MERGE_RE ALONE, so it
// never writes a label for a form agent-merge would not have closed (widening both is #496).
const CLOSING = /(?<![\w*_])[*_]*(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?)[*_]*\s*:?\s*(?:<?(https?:\/\/github\.com\/([\w.-]+)\/([\w.-]+)\/issues\/(\d+))>?|\[#(\d+)\]\(([^)]*)\)|([\w.-]+)\/([\w.-]+)#(\d+)|GH-(\d+)|#(\d+))/gi;
const MERGE_RE = /\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?)\s+#(\d+)/gi;
const ISSUE_URL = /^https?:\/\/github\.com\/([\w.-]+)\/([\w.-]+)\/issues\/(\d+)\/?$/i;
function closingMatches(text, owner, repo) {   // the RAW body; no markup is interpreted
  const own = String(owner).toLowerCase(), rep = String(repo).toLowerCase();
  const sameRepo = (o, r) => o.toLowerCase() === own && r.toLowerCase() === rep;
  const out = [];
  const body = String(text || '');
  for (const m of body.matchAll(CLOSING)) {
    const spelled = m[0].replace(/\s+/g, ' ');
    if (m[1]) out.push({ spelled, kind: 2, num: Number(m[4]), same: sameRepo(m[2], m[3]) });
    else if (m[5]) {
      const tgt = ISSUE_URL.exec(m[6] || '');
      const agrees = !!tgt && sameRepo(tgt[1], tgt[2]) && Number(tgt[3]) === Number(m[5]);
      out.push({ spelled, kind: 0, num: Number(m[5]), same: agrees });
    }
    else if (m[9]) out.push({ spelled, kind: 2, num: Number(m[9]), same: sameRepo(m[7], m[8]) });
    else if (m[10]) out.push({ spelled, kind: 1, num: Number(m[10]), same: true });
    else out.push({ spelled, kind: 0, num: Number(m[11]), same: true });
  }
  // agent-merge's reader, verbatim: anything it finds that CLOSING did not is a reference too.
  for (const m of body.matchAll(MERGE_RE)) {
    const spelled = m[0].replace(/\s+/g, ' ');
    if (!out.some(i => i.spelled === spelled || i.spelled.endsWith(spelled))) out.push({ spelled, kind: 0, num: Number(m[1]), same: true });
  }
  return out;
}
// What agent-merge itself would close/release: MERGE_RE alone, same repo by construction.
function mergeGateIssues(text) {
  return [...new Set([...String(text || '').matchAll(MERGE_RE)].map(m => Number(m[1])))].sort((x, y) => x - y);
}
function joinSpelled(items) {
  const seen = new Set(), uniq = [];
  for (const it of items) { if (!seen.has(it.spelled)) { seen.add(it.spelled); uniq.push(it); } }
  uniq.sort((x, y) => x.kind - y.kind || x.num - y.num || (x.spelled < y.spelled ? -1 : x.spelled > y.spelled ? 1 : 0));
  return uniq.map(i => i.spelled).join(',');
}
function closingRefs(body, owner, repo) { return joinSpelled(closingMatches(body, owner, repo)); }
// The release paths act on what agent-merge acts on — MERGE_RE, nothing wider — so the exhaustion
// release never strips `in-progress` from, or escalates, an issue the merge would not have
// closed (round-11 red team on #466). Widening both readers together is #496.
function closingIssues(body, owner, repo) { return mergeGateIssues(body); }

// How many times a no-op run may be retried in-job before the claim is released (#361 AC2).
// Read at RUN time from the DEFAULT-branch config, like every other knob here, so retuning it
// needs no workflow-scope push. Fail closed to 0 on anything unreadable, absent, negative or
// non-numeric, and clamp above: a typo must never buy an agent more attempts than the ruling
// allows. The clamp mirrors auto_fix_max_attempts' "values above 10 are clamped DOWN".
// Clamped to the number of retry steps the workflows actually contain — ONE. A clamp of 2
// let `agent_retry_max: 2` be configured and returned, while only one retry step existed:
// the extra retry was silently unspendable AND the wiring test (which pins
// len(claude) == budget + 1) turned red, so the documented run-time retune required a
// workflow-scope push after all. Raising it means adding a retry step in the same change.
const RETRY_CLAMP = 1;

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

  let produced = false, partial = false, onlyComment = false, refsChanged = false, refsDetail = '', reason = '';
  const since = Date.parse(env.SINCE || '');
  const sinceKnown = Number.isFinite(since);
  if (!sinceKnown) core.warning('agent-noop: the run start (SINCE) is empty or unparseable — t0 did not run, so nothing can be attributed to this run (fail closed)');
  if (kind === 'implement' && !sinceKnown) {
    reason = 'the run start is unknown (t0 did not run, so an earlier step failed) — nothing can be attributed to this run';
  } else if (kind === 'implement') {
    const issue = String(env.ISSUE || '').trim();
    const prs = await github.paginate(github.rest.pulls.list, { owner, repo, state: 'open', per_page: 100 });
    const mine = prs.filter(p => p && p.state !== 'closed' && p.head && p.head.ref === `task/${issue}`);

    // `updated_at` is NOT a push signal: GitHub bumps it on any issue-level event — a comment, a
    // label, a title edit. So a single claude-review comment on a still-open PR from an EARLIER
    // dispatch vouched for a silent run, reopening the #58 -> #401 hole through a different field
    // (round-2 red team). The PR's last commit date only moves when something is pushed.
    const lastPush = async (p) => {
      try {
        const commits = await github.paginate(github.rest.pulls.listCommits,
          { owner, repo, pull_number: p.number, per_page: 100 });
        const last = commits[commits.length - 1];
        return Date.parse((((last || {}).commit || {}).committer || {}).date || '');
      } catch (e) { return NaN; }
    };
    for (const p of mine) {
      if (Date.parse(p.created_at || '') > since) { produced = true; break; }
      const pushed = await lastPush(p);
      if (Number.isFinite(pushed) && pushed > since) { produced = true; break; }
    }
    reason = produced ? `an open PR on task/${issue} was created or pushed to by this run`
      : mine.length ? `the only open PR on task/${issue} predates this run`
        : `no open PR on task/${issue}`;

    // A run that COMMITTED and PUSHED task/<n> but was denied `gh pr create` produced real work.
    // #58 died on 26 permission denials — this is that shape — and calling it a no-op releases
    // the claim while a branch with commits sits on it, so the next dispatch starts from a tree
    // that already has them (round-2 red team). Counting it follows this module's own policy:
    // a false no-op costs more than a missed one.
    if (!produced) {
      try {
        const br = (await github.rest.repos.getBranch({ owner, repo, branch: `task/${issue}` })).data;
        const when = Date.parse(((((br || {}).commit || {}).commit || {}).committer || {}).date || '');
        if (Number.isFinite(when) && when > since) {
          produced = true;
          partial = true;   // kept, not retried — but finalize still speaks and fails (round-3 review)
          reason = `commits were pushed to task/${issue} during this run, though no PR was opened — check whether \`gh pr create\` was denied`;
        }
      } catch (e) { /* 404: no such branch, so nothing was pushed */ }
    }
  } else {
    const number = Number(env.PR);
    const before = String(env.HEAD_BEFORE || '').trim();
    const pr = (await github.rest.pulls.get({ owner, repo, pull_number: number })).data;
    const moved = !!before && pr.head.sha !== before;
    // Only the FIXER's comments, made DURING this run, count; the verdict that triggered the run
    // predates it, and other claude[bot] workflows are not the fixer. A moved head needs no
    // window: HEAD_BEFORE comes from the gate, so the comparison stands even with SINCE unknown.
    const comments = sinceKnown
      ? await github.paginate(github.rest.issues.listComments, { owner, repo, issue_number: number, per_page: 100 })
      : [];
    const spoke = comments.some(c =>
      c.user && c.user.login === 'claude[bot]' && isFixer(c.body) && !isVerdict(c.body) &&
      Date.parse(c.created_at || '') > since);
    produced = moved || spoke;
    // A comment with no commit is production for THIS check and a dead end for the loop: no new
    // head, so no new review, so agent-merge says "review reported findings" until a human reads
    // the comment (#463, seen on #452). finalize turns it into the label humans watch.
    onlyComment = spoke && !moved;
    // The fixer may now rewrite the description (#463), and agent-merge closes — and the
    // exhaustion path releases — every issue the body's Closes/Fixes/Resolves name. The gate
    // snapshots that set before the run (`none` when empty; '' means an older gate that did not);
    // a change is escalated and the job failed (round-2 review + red team on #466).
    const snap = String(env.CLOSES_BEFORE || '').trim();
    const asSet = t => [...new Set(String(t || '').split(',').map(x => x.trim()).filter(Boolean))].sort().join(',');
    const now = asSet(closingRefs(pr.body, owner, repo));
    const show = t => t ? t.split(',').join(', ') : 'none';
    if (!snap) {
      // No snapshot means a gate older than this detector. The guard fails CLOSED: it cannot
      // judge, so it escalates rather than letting a body rewrite through unseen (round-6 red team).
      refsChanged = true;
      refsDetail = `no closing-reference snapshot from the gate (gate and detector out of step); now: ${show(now)}`;
      core.warning(`agent-noop: ${refsDetail}`);
    } else {
      // The gate and this detector ship together and both load from the default branch, so the
      // snapshot is always this format; anything else is unreadable and fails CLOSED (a shim for an
      // older two-view format truncated legitimate snapshots — round-10 red team on #466).
      if (/^raw=|;gh=/.test(snap)) { refsChanged = true; refsDetail = `unreadable closing-reference snapshot '${snap.slice(0, 60)}' (gate and detector out of step)`; }
      else {
        const before = snap === 'none' ? '' : asSet(snap);
        refsChanged = before !== now;
        refsDetail = `as the merge automation reads it — before: ${show(before)}; now: ${show(now)}`;
      }
      if (refsChanged) core.warning(`agent-noop: the PR's closing references changed during the run (${refsDetail})`);
    }
    reason = moved ? `head moved from ${before.slice(0, 7)} to ${pr.head.sha.slice(0, 7)}`
      : spoke ? 'the fixer commented during the run'
        : !sinceKnown ? `head unchanged at ${before.slice(0, 7)} and the run start is unknown (t0 did not run)`
          : `head unchanged at ${before.slice(0, 7)} and the fixer said nothing`;
  }

  // `!f.ok ||` used to short-circuit here, so a cancelled or timed-out run — both steps are
  // `if: always()` — was declared a no-op even when detection had just established that a PR
  // exists or the head moved, releasing a live claim and posting a comment its own `reason`
  // contradicted (round-1 red team). Missing figures fail closed only where production is
  // UNKNOWN; where it is known and positive, it stands.
  const noop = !produced || f.isError === true;
  if (noop) core.notice(`agent-noop: NO-OP — ${reason}${f.isError ? ' (is_error)' : ''}${f.ok ? '' : ' (figures unavailable)'}`);
  core.setOutput('noop', String(noop));
  // `noop` and `produced` answer DIFFERENT questions, and conflating them retried a run that had
  // already pushed: AC2 says an attempt that produced output is never retried, while also naming
  // is_error as a no-op trigger. So `noop` decides escalate-or-release, and `produced` decides
  // retry-or-not (round-2 review). A review-fix run that pushed and then errored is a no-op that
  // must NOT be retried — re-fixing fixed code is its own hazard.
  core.setOutput('produced', String(produced));
  core.setOutput('partial', String(partial));   // implement only: a branch with no PR
  core.setOutput('only_comment', String(onlyComment));   // fix only: answered, did not push
  core.setOutput('refs_changed', String(refsChanged));   // fix only: Closes/Fixes/Resolves set moved
  core.setOutput('refs_detail', refsDetail);
  core.setOutput('reason', reason);   // finalize reports THIS, not a hard-coded sentence
  core.setOutput('turns', String(f.turns));
  core.setOutput('denials', String(f.denials));
  return { noop, produced, partial, only_comment: onlyComment, refs_changed: refsChanged, refs_detail: refsDetail, turns: f.turns, denials: f.denials, reason };
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
  // Mirror the workflows' own early return, so the module is right even if the glue is not: a
  // run that produced, did not error, is not comment-only and not partial has nothing to
  // finalize. Compared against the literal 'true' — the workflow sends the STRING 'false' on
  // every ordinary run, and 'false' is truthy (round-1 red team on #466).
  if (env.NOOP === 'false' && env.ONLY_COMMENT !== 'true' && env.PARTIAL !== 'true' && env.REFS_CHANGED !== 'true') {
    core.notice('agent-noop: production established — nothing to finalize');
    return;
  }
  const runUrl = `${context.serverUrl}/${owner}/${repo}/actions/runs/${context.runId}`;
  // AC3 asks for the per-attempt figures, and the env used to carry only the LAST attempt's while
  // the comment said "per-attempt" (round-2 review). Report each attempt that ran.
  const perAttempt = [env.FIGURES_1 && `attempt 1: ${env.FIGURES_1}`, env.FIGURES_2 && `attempt 2: ${env.FIGURES_2}`]
    .filter(Boolean).join('; ') || `turns=${env.TURNS}, permission denials=${env.DENIALS}`;
  const spent = `${env.ATTEMPTS} attempt(s) — ${perAttempt}`;
  // detect computed an accurate reason; hard-coding "no branch, no PR, no comment" here made the
  // comment contradict it on the is_error path (round-1 review).
  const why = env.REASON ? `Detected: ${env.REASON}.` : 'It produced no branch, no PR and no comment.';
  const said = {
    removed: 'has been released',
    absent: 'was not present',
    failed: 'could NOT be released — do it by hand',
  };
  // Production established and then is_error, or a branch with no PR: the work EXISTS, so the
  // claim/attempt stays with it. Releasing it under a live PR — and saying "produced nothing" over
  // a `Detected:` line that says a PR was opened — was round 3's finding, twice. Still loud: a
  // comment and a failed job, never a green exit.
  const produced = env.PRODUCED === 'true';
  const partial = env.PARTIAL === 'true';

  if (kind === 'implement' && produced) {
    const n = Number(env.ISSUE);
    const headline = partial
      ? `⚠️ Dispatch pushed \`task/${n}\` but opened no PR, after ${spent}.`
      : `🛑 Dispatch errored AFTER producing output, after ${spent}.`;
    await github.rest.issues.createComment({
      owner, repo, issue_number: n,
      body: `${headline}\n\n${why}\n\n` +
        `The \`in-progress\` claim is KEPT: the work exists on the branch and releasing it would let the next dispatch start over on top of it. ` +
        `${partial ? 'Open the PR by hand, or push the branch away and remove `in-progress` to re-dispatch. A high denial count usually means `gh pr create` hit the allow-list. ' : 'Read the run before deciding whether to re-dispatch. '}` +
        `The per-attempt figures are above and in the job log: ${runUrl}`});
    core.setFailed(`agent-noop: dispatch for #${n} ${partial ? 'pushed a branch but opened no PR' : 'errored after producing output'} after ${spent} — ${env.REASON || ''}; claim kept, job failed so this is not silent (#361).`);
    return;
  }

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
  const label = `review-fix-${env.ATTEMPT}`;
  if (env.REFS_CHANGED === 'true') {
    // An agent rewrote the text the merge gate acts on mechanically. Whatever else the run did,
    // this is escalated and fails: the references are restored by a human, not by the loop.
    let esc = true;
    try { await github.rest.issues.addLabels({ owner, repo, issue_number: pr, labels: ['needs-human'] }); }
    catch (e) { esc = false; core.warning(`agent-noop: could NOT apply \`needs-human\` to #${pr} (${(e && e.status) || ''}) — apply it by hand`); }
    await github.rest.issues.createComment({
      owner, repo, issue_number: pr,
      body: `🛑 Review-fix changed the closing references in this PR's description (${env.REFS_DETAIL || 'detail unavailable'}), after ${spent}.\n\n` +
        `\`agent-merge\` closes, and the exhaustion path releases, every issue those \`Closes\`/\`Fixes\`/\`Resolves\` references name — text an agent must not change. ` +
        `${esc ? '`needs-human` is applied' : '⚠️ `needs-human` could NOT be applied — apply it by hand'}: restore the references by hand before this PR goes any further. ` +
        `\`${label}\` stays: the attempt was spent. Job log: ${runUrl}`});
    core.setFailed(`agent-noop: review-fix on #${pr} changed the PR's closing references (${env.REFS_DETAIL || ''}) — needs-human ${esc ? 'applied' : 'NOT applied'}, job failed (#463).`);
    return;
  }
  // Only a run that did NOT error: is_error is a no-op whatever else is true (see the header), so
  // an errored comment-only run falls through to the produced-then-errored branch below, which
  // fails loudly and says it errored (round-1 review on #466).
  if (env.ONLY_COMMENT === 'true' && env.NOOP === 'false') {
    // The fixer answered — a rebuttal, or a finding it cannot reach — and pushed nothing. That is
    // what the prompt tells it to do, so the job does not fail and the attempt stays spent; but
    // the loop cannot advance from here on its own, and every other terminal state applies
    // `needs-human`. This one now does too (#463).
    let esc = true;
    try { await github.rest.issues.addLabels({ owner, repo, issue_number: pr, labels: ['needs-human'] }); }
    catch (e) { esc = false; core.warning(`agent-noop: could NOT apply \`needs-human\` to #${pr} (${(e && e.status) || ''}) — apply it by hand`); }
    await github.rest.issues.createComment({
      owner, repo, issue_number: pr,
      body: `🧑 Review-fix answered without a commit (${spent}).\n\n${why}\n\n` +
        `The fixer's comment above is a rebuttal, a finding it could not act on, or a description-only correction that was not followed by the empty commit its prompt asks for. No new head means no new review, so the loop stops here: ` +
        `${esc ? '`needs-human` is applied' : '⚠️ `needs-human` could NOT be applied — apply it by hand'}. Rule on the rebuttal, or push the change it asked for, then remove the label. ` +
        `\`${label}\` stays: the attempt was spent. Job log: ${runUrl}`});
    core.notice(`agent-noop: review-fix on #${pr} answered without a commit — needs-human ${esc ? 'applied' : 'NOT applied'}; attempt ${label} spent (#463).`);
    return;
  }
  if (produced) {
    // It pushed or commented, then errored: the attempt is SPENT, whatever is_error says. Handing
    // it back understated review_fix_max_attempts on a run that changed the branch (round-3 red team).
    let esc = true;
    try { await github.rest.issues.addLabels({ owner, repo, issue_number: pr, labels: ['needs-human'] }); }
    catch (e) { esc = false; core.warning(`agent-noop: could NOT apply \`needs-human\` to #${pr} (${(e && e.status) || ''}) — apply it by hand`); }
    await github.rest.issues.createComment({
      owner, repo, issue_number: pr,
      body: `🛑 Review-fix errored AFTER producing output, after ${spent}.\n\n${why}\n\n` +
        `\`${label}\` stays: this attempt is **spent** — it changed the branch or commented before erroring, so it counts against \`review_fix_max_attempts\`. ` +
        `${esc ? '`needs-human` is applied' : '⚠️ `needs-human` could NOT be applied — apply it by hand'}: read the run before letting the loop continue.\n\n` +
        `The per-attempt figures are above and in the job log: ${runUrl}`});
    core.setFailed(`agent-noop: review-fix on #${pr} errored after producing output after ${spent} — ${env.REASON || ''}; attempt spent, needs-human ${esc ? 'applied' : 'NOT applied'}, job failed (#361).`);
    return;
  }
  // The gate applies `review-fix-<n>` BEFORE this job runs (review-fix.yml:161), so a run that
  // produced nothing has already spent one of the four attempts. Hand it back.
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

module.exports = { detect, finalize, readFigures, isVerdict, isFixer, retryBudget, closingRefs, closingIssues };
