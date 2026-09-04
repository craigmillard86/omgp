"""Regression tests for the story-release workflows (ready-gate.yml, promote-queued.yml).

The workflows only fire on real label/close events, so their github-script bodies are
extracted from the YAML here and executed VERBATIM by tests/workflows/story_gate_harness.js
against a mocked GitHub API. Added after the review of PR #90 found that the evidence for a
section-parser fix was a one-off manual check with nothing committed (CLAUDE.md rule 8).
Cases include the two reported bugs: #26's bold section titles reading as "all sections
missing", and a bold aside inside a section ending it early (hiding a later dependency)."""
from __future__ import annotations

import json
import os
import pathlib
import re
import shutil
import subprocess

import pytest
import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
HARNESS = ROOT / "tests" / "workflows" / "story_gate_harness.js"


def _script(workflow: str, job: str) -> str:
    wf = yaml.safe_load((ROOT / ".github" / "workflows" / workflow).read_text())
    steps = wf["jobs"][job]["steps"]
    return next(s["with"]["script"] for s in steps if "actions/github-script" in s.get("uses", ""))


@pytest.mark.skipif(shutil.which("node") is None,
                    reason="node not present (blind spot: workflow scripts not exercised in this environment)")
def test_story_release_scripts_against_mocked_github(tmp_path):
    scripts = {"gate": _script("ready-gate.yml", "validate"), "promote": _script("promote-queued.yml", "promote")}
    f = tmp_path / "scripts.json"
    f.write_text(json.dumps(scripts))
    r = subprocess.run(["node", str(HARNESS), str(f), str(ROOT)], capture_output=True, text=True, cwd=ROOT, timeout=120)
    print(r.stdout)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "FAIL" not in r.stdout
    assert "cases passed" in r.stdout


# --- ci-failure-router.yml (GOVERNANCE.md §4 "CI-failure auto-resolution") ---------------------

ROUTER = ROOT / ".github" / "workflows" / "ci-failure-router.yml"
ROUTER_HARNESS = ROOT / "tests" / "workflows" / "ci_failure_router_harness.js"


@pytest.mark.skipif(shutil.which("node") is None,
                    reason="node not present (blind spot: workflow scripts not exercised in this environment)")
def test_ci_failure_router_routing_against_mocked_github(tmp_path):
    f = tmp_path / "scripts.json"
    f.write_text(json.dumps({"route": _script("ci-failure-router.yml", "route"),
                             "sweep": _script("ci-failure-router.yml", "sweep")}))
    r = subprocess.run(["node", str(ROUTER_HARNESS), str(f), str(ROOT)], capture_output=True, text=True, cwd=ROOT, timeout=120)
    print(r.stdout)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "FAIL" not in r.stdout
    assert "cases passed" in r.stdout


def _run_cfg_step(cfg_script: str, cwd: pathlib.Path, tmp_path: pathlib.Path) -> str:
    """Execute a cfg `run:` block verbatim the way the runner would, returning the max= value."""
    gh_out = tmp_path / f"gh_out_{abs(hash(cfg_script + str(cwd)))}"
    gh_out.write_text("")
    subprocess.run(["bash", "-euo", "pipefail", "-c", cfg_script], cwd=cwd, check=True, timeout=30,
                   env={**os.environ, "GITHUB_OUTPUT": str(gh_out)})
    return dict(l.split("=", 1) for l in gh_out.read_text().splitlines() if "=" in l).get("max", "")


def test_ci_failure_router_wiring(tmp_path):
    """The bounds that make the loop safe live in the YAML, not only in the script."""
    wf = yaml.safe_load(ROUTER.read_text())
    on = wf[True] if True in wf else wf["on"]           # PyYAML reads the bare key `on` as True
    assert set(on["workflow_run"]["workflows"]) == {"ci", "security"}
    assert on["workflow_run"]["types"] == ["completed"]
    assert "head_branch" in wf["concurrency"]["group"] and wf["concurrency"]["cancel-in-progress"] is False
    route, autofix = wf["jobs"]["route"], wf["jobs"]["autofix"]
    # The cfg step is the bound's only real input, so it is EXECUTED here, not just grepped
    # (red-team round 2 on #120: a copy-paste of the sed to a different key left the ruling
    # inert with the whole suite green). Against the real config it must yield the configured
    # digits; against an operator's `0  # OFF` it must yield 0 — the off-switch fails CLOSED.
    cfg = next(st for st in route["steps"] if st.get("id") == "cfg")["run"]
    assert "auto_fix_max_attempts" in cfg
    # Shape pin accepting every DOCUMENTED form (review+red-team round 4 on #120: the first
    # shape pin rejected `0  # OFF`, `-1` and quoted values — the very forms agent-config.yml
    # documents — so pulling the off-switch turned ci-gate red repo-wide). The VALUE
    # semantics (fail-closed 2, <1 disables, clamp 10) are pinned by harness cases.
    _KNOB = re.compile(r'^auto_fix_max_attempts: *"?(-?\d+)"?( *#.*)?$', re.M)
    _m = _KNOB.findall((ROOT / ".github" / "agent-config.yml").read_text())
    assert _m, "auto_fix_max_attempts missing or in an undocumented form"
    want = _m[-1][0]  # last match: YAML last-key-wins, matching the cfg step's tail -1
    assert _run_cfg_step(cfg, ROOT, tmp_path) == want
    # The sed's whole contract (red-team round 4 on #120): documented forms pass through,
    # digit-PREFIX values do NOT truncate — they emit nothing and the route script fails
    # closed to 2. `0x10` truncating to `0` silently disabled auto-fix.
    synth = tmp_path / "synth"
    (synth / ".github").mkdir(parents=True)
    for raw, expect in [("0  # OFF during incident", "0"), ("-1", "-1"), ('"4"', "4"),
                        ("1e3", ""), ("0x10", ""), ("4.9", ""), ("4 attempts", "")]:
        (synth / ".github" / "agent-config.yml").write_text(f"auto_fix_max_attempts: {raw}\n")
        assert _run_cfg_step(cfg, synth, tmp_path) == expect, raw
    # least privilege per job (review on #96): the always-running router holds no OIDC/contents/actions write
    assert "permissions" not in wf
    assert autofix["permissions"]["id-token"] == "write" and autofix["permissions"]["contents"] == "write"
    assert "id-token" not in route["permissions"] and route["permissions"].get("contents", "read") == "read" and route["permissions"]["actions"] == "read"
    # Hard-failure conclusions all route (red-team round 2): the guard, the script re-check
    # and the sweep filter share the same three-conclusion set.
    assert '["failure", "timed_out", "startup_failure"]' in route["if"] and "head_repository.full_name == github.repository" in route["if"]
    _st = next(st for st in wf["jobs"]["route"]["steps"] if "actions/github-script" in st.get("uses", ""))
    assert "MAX_ATTEMPTS" in _st.get("env", {})   # bound is the agent-config knob (ruling 2026-09-03)
    # red-team #120 F1: the sweep holds NO bound — it re-delivers even at/after exhaustion,
    # because the escalation is the router decision that needs the delivery backstop most.
    _sw = next(st for st in wf["jobs"]["sweep"]["steps"] if "actions/github-script" in st.get("uses", ""))
    assert "MAX_ATTEMPTS" not in _sw.get("env", {})
    # (the documented-forms knob pin lives above, beside the executed cfg step)
    assert autofix["needs"] == "route" and "route.outputs.route == 'autofix'" in autofix["if"]
    action = next(s for s in autofix["steps"] if "claude-code-action" in s.get("uses", ""))
    assert "claude_code_oauth_token" in action["with"]
    for must in ("CLAUDE.md", "OPERATING-POLICY", "gh run view --log-failed", "./pipeline.sh", "environmental", "gh run rerun"):
        assert must in action["with"]["prompt"], must
    tools = action["with"]["claude_args"]
    assert "Bash(git*)" in tools and "Bash(gh run rerun*)" in tools and "Bash(gh pr create*)" not in tools
    assert autofix["steps"][0]["with"]["ref"] == "${{ needs.route.outputs.branch }}"
    # docs/OPEN-QUESTIONS.md 2026-08-31 fix (a): a push carrying GITHUB_TOKEN credentials
    # suppresses the workflow_run delivery for the CI run it starts, so the router never saw
    # its own attempt's failure. The agent must push with the Claude App token instead.
    assert autofix["steps"][0]["with"]["persist-credentials"] is False
    # ...and the autofix job must read the failure from the router's outputs, so the
    # dispatch path (belt (b)) routes through exactly the same code as the event path.
    assert "github.event.workflow_run" not in yaml.dump(autofix)


def test_ci_failure_router_delivery_backstop():
    """Belt (b) from the same entry: workflow_run delivery is not trustworthy for this loop,
    so a scheduled sweep re-dispatches failures the router never saw. Demonstrated on #108:
    neither the auto-fix push's CI failure nor a manual `gh run rerun` produced a router run,
    and attempt 2 could not fire by any existing mechanism."""
    wf = yaml.safe_load(ROUTER.read_text())
    on = wf[True] if True in wf else wf["on"]
    assert "schedule" in on and on["schedule"], "no scheduled sweep"
    assert "run_id" in on["workflow_dispatch"]["inputs"], "no manual routing path for a suppressed failure"
    sweep = wf["jobs"]["sweep"]
    assert sweep["permissions"]["actions"] == "write"           # dispatching the router
    assert sweep["permissions"].get("contents", "read") == "read"   # the sweep never writes code
    assert "schedule" in sweep["if"] and "inputs.run_id" in sweep["if"]
    script = next(s for s in sweep["steps"] if "actions/github-script" in s.get("uses", ""))["with"]["script"]
    # The sweep decides nothing about the failure: it re-delivers, the bounds stay in `route`
    # (red-team #120 F1: it must NOT stop at the bound — exhaustion needs delivery too).
    for must in ("agent-authored", "needs-human", "ci-failure-router sha=", "createWorkflowDispatch"):
        assert must in script, must
    assert "MAX_ATTEMPTS" not in script
    assert "addLabels" not in script and "claude" not in script.lower()
    route = wf["jobs"]["route"]
    assert "workflow_dispatch" in route["if"] and "inputs.run_id" in route["if"]


def test_main_ci_failures_reach_triage():
    """The router files the issue; agent-triage handles it exactly like nightly-failure."""
    triage = yaml.safe_load((ROOT / ".github" / "workflows" / "agent-triage.yml").read_text())
    assert "'ci-failure'" in triage["jobs"]["triage"]["if"] and "'nightly-failure'" in triage["jobs"]["triage"]["if"]


# --- WIP cap as a knob, counting stories (ruling 2026-09-03) -----------------------------------

PICK_HARNESS = ROOT / "tests" / "workflows" / "agent_pick_harness.js"


@pytest.mark.skipif(shutil.which("node") is None,
                    reason="node not present (blind spot: workflow scripts not exercised in this environment)")
def test_agent_pick_wip_cap_against_mocked_github(tmp_path):
    f = tmp_path / "scripts.json"
    f.write_text(json.dumps({"pick": _script("agent-dispatch.yml", "pick")}))
    r = subprocess.run(["node", str(PICK_HARNESS), str(f), str(ROOT)], capture_output=True, text=True, cwd=ROOT, timeout=120)
    print(r.stdout)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "FAIL" not in r.stdout
    assert "cases passed" in r.stdout


def test_wip_cap_wiring():
    """The knob is a T3 config value read by the workflow, not a constant in the script."""
    cfg = (ROOT / ".github" / "agent-config.yml").read_text()
    assert "wip_cap: 2" in cfg
    wf = yaml.safe_load((ROOT / ".github" / "workflows" / "agent-dispatch.yml").read_text())
    steps = wf["jobs"]["pick"]["steps"]
    script_step = next(s for s in steps if "actions/github-script" in s.get("uses", ""))
    assert "WIP_CAP" in script_step.get("env", {})
    gov = (ROOT / "docs" / "GOVERNANCE.md").read_text()
    assert "wip_cap" in gov


# --- model tiers for agent workflows (ruling 2026-08-31) ---------------------------------------

def _claude_steps(workflow):
    wf = yaml.safe_load((ROOT / ".github" / "workflows" / workflow).read_text())
    return [s for j in wf["jobs"].values() for s in j.get("steps", []) if "claude-code-action" in s.get("uses", "")]


def test_model_tiers_judgement_loops_on_opus_volume_loops_on_default():
    """Ruling 2026-08-31: the judgement-heavy loops (reviews that back approvals, red team,
    story planning/enrichment, spec-drift audit, failure triage) run claude-opus-5; the
    high-volume implementation loops (dispatch, router auto-fix, mentions, review-fix) stay on the
    action default (claude-sonnet-5) behind their mechanical gates."""
    OPUS = ["claude-review.yml", "red-team.yml", "story-enrich.yml",
            "agent-converge-audit.yml", "agent-triage.yml", "continuous-improvement.yml"]
    DEFAULT = ["agent-dispatch.yml", "ci-failure-router.yml", "claude-mention.yml", "review-fix.yml"]
    for wfn in OPUS:
        steps = _claude_steps(wfn)
        assert steps, wfn
        for s in steps:
            assert "--model claude-opus-5" in s["with"].get("claude_args", ""), (wfn, s.get("name"))
    for wfn in DEFAULT:
        for s in _claude_steps(wfn):
            assert "--model" not in s["with"].get("claude_args", ""), (wfn, s.get("name"))


# --- agent PR approval below T3 (ruling 2026-08-31) --------------------------------------------

APPROVE_HARNESS = ROOT / "tests" / "workflows" / "agent_approve_harness.js"


@pytest.mark.skipif(shutil.which("node") is None,
                    reason="node not present (blind spot: workflow scripts not exercised in this environment)")
def test_agent_approval_against_mocked_github(tmp_path):
    f = tmp_path / "scripts.json"
    f.write_text(json.dumps({"approve": _script("agent-approve.yml", "approve")}))
    r = subprocess.run(["node", str(APPROVE_HARNESS), str(f), str(ROOT)], capture_output=True, text=True, cwd=ROOT, timeout=120)
    print(r.stdout)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "FAIL" not in r.stdout
    assert "cases passed" in r.stdout


def test_agent_approval_wiring():
    """The safety properties live in the YAML. Hardened per the Copilot review on #103:
    approval runs from the DEFAULT-BRANCH workflow definition (issue_comment trigger), so a
    PR cannot rewrite the gate, the tier resolver, or the knob in its own diff — the
    pull_request-triggered claude-review runs the PR's own workflow version and must
    therefore never hold the approval logic."""
    review = yaml.safe_load((ROOT / ".github" / "workflows" / "claude-review.yml").read_text())
    on = review[True] if True in review else review["on"]
    assert "synchronize" in on["pull_request"]["types"]          # verdicts must exist for the CURRENT head
    prompt = next(s for s in review["jobs"]["review"]["steps"] if "claude-code-action" in s.get("uses", ""))["with"]["prompt"]
    assert "VERDICT(review):" in prompt and "head" in prompt
    assert "approve" not in review["jobs"]                       # never in the PR-controlled workflow
    approve_wf = yaml.safe_load((ROOT / ".github" / "workflows" / "agent-approve.yml").read_text())
    aon = approve_wf[True] if True in approve_wf else approve_wf["on"]
    assert aon["issue_comment"]["types"] == ["created"]          # default-branch definition runs
    approve = approve_wf["jobs"]["approve"]
    assert "issue.pull_request" in approve["if"] and "VERDICT(" in approve["if"]
    assert approve["permissions"] == {"contents": "read", "pull-requests": "write"}
    steps = approve["steps"]
    checkout = next(s for s in steps if "actions/checkout" in s.get("uses", ""))
    assert "ref" not in checkout.get("with", {})                 # issue_comment checks out the DEFAULT branch
    redteam = yaml.safe_load((ROOT / ".github" / "workflows" / "red-team.yml").read_text())
    rt_on = redteam[True] if True in redteam else redteam["on"]
    # At T2 approval needs a red-team verdict for the CURRENT head, so red-team must re-run
    # per push exactly as claude-review does (live gap: no red-team run existed for #108's
    # auto-fixed head 58e4e91, 2026-08-31).
    assert "synchronize" in rt_on["pull_request"]["types"]
    rt_prompt = next(s for s in redteam["jobs"]["attack-pr"]["steps"] if "claude-code-action" in s.get("uses", ""))["with"]["prompt"]
    assert "VERDICT(red-team):" in rt_prompt
    cfg = (ROOT / ".github" / "agent-config.yml").read_text()
    assert "auto_approve_max_tier: 2" in cfg


def test_bot_triggered_agent_workflows_allow_their_bot_actors():
    """A repository_dispatch sent with GITHUB_TOKEN runs as github-actions[bot], and
    claude-code-action refuses bot actors unless named (first hit: #77 on claude-review;
    live failure: run 33332211854 — the #93 backlog-changed nudge claimed #28, then
    implement died 'Workflow initiated by non-human actor: github-actions'). Every
    workflow whose claude-code-action step can be reached from a bot-caused trigger
    must declare that bot in allowed_bots."""
    for workflow, bot in [("agent-dispatch.yml", "github-actions"),   # repository_dispatch: backlog-changed (ready-gate, GITHUB_TOKEN)
                          ("agent-triage.yml", "github-actions"),     # issues.labeled by nightly/ci-failure-router
                          ("ci-failure-router.yml", "github-actions"),  # workflow_run caused by bot pushes/dispatches
                          ("claude-review.yml", "claude"),            # agent PRs are opened by the Claude App
                          ("claude-review.yml", "github-actions"),    # synchronize from a router auto-fix push runs as github-actions (live: run 33435939888 on #108)
                          ("red-team.yml", "claude"),
                          ("red-team.yml", "github-actions"),     # same synchronize path once red-team gains it; opened-by-bot today
                          ("review-fix.yml", "claude")]:          # the trigger is claude[bot]'s own findings verdict
        wf = yaml.safe_load((ROOT / ".github" / "workflows" / workflow).read_text())
        actions = [s for j in wf["jobs"].values() for s in j.get("steps", []) if "claude-code-action" in s.get("uses", "")]
        assert actions, workflow
        for a in actions:
            assert bot in a["with"].get("allowed_bots", ""), (workflow, "allowed_bots must include " + bot)


def test_router_labels_are_provisioned():
    setup = (ROOT / "tools" / "gh-setup.sh").read_text()
    # auto-fix-1..10: the knob clamps at 10 and the first-free-index write can reach any of
    # them, so every label the router can create is provisioned (review round 3 on #120).
    for l in (*[f"auto-fix-{n}" for n in range(1, 11)], "ci-failure",
              "review-fix-1", "review-fix-2", "review-fix-3", "review-fix-4"):
        assert f"L {l} " in setup or f'L "{l}"' in setup, l


# --- review-finding auto-resolution (review-fix.yml) -------------------------------------------

REVIEW_FIX = ROOT / ".github" / "workflows" / "review-fix.yml"
REVIEW_FIX_HARNESS = ROOT / "tests" / "workflows" / "review_fix_harness.js"


@pytest.mark.skipif(shutil.which("node") is None,
                    reason="node not present (blind spot: workflow scripts not exercised in this environment)")
def test_review_fix_gate_against_mocked_github(tmp_path):
    f = tmp_path / "scripts.json"
    f.write_text(json.dumps({"gate": _script("review-fix.yml", "gate")}))
    r = subprocess.run(["node", str(REVIEW_FIX_HARNESS), str(f), str(ROOT)], capture_output=True, text=True, cwd=ROOT, timeout=120)
    print(r.stdout)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "FAIL" not in r.stdout
    assert "cases passed" in r.stdout


def test_review_fix_wiring():
    """The loop that closes the review-findings gap: claude-review and red-team are
    read-only and agent-approve only WITHHOLDS approval, so before this workflow a
    green-CI agent PR carrying findings stalled with no agent able to act. Its safety
    properties live in the YAML, like the CI router's."""
    wf = yaml.safe_load(REVIEW_FIX.read_text())
    on = wf[True] if True in wf else wf["on"]
    # Default-branch definition runs (same rationale as agent-approve): a PR cannot rewrite
    # this loop's own bounds in its own diff.
    assert on["issue_comment"]["types"] == ["created"]
    gate, fix = wf["jobs"]["gate"], wf["jobs"]["fix"]
    assert "claude[bot]" in gate["if"] and "VERDICT(" in gate["if"]
    checkout = next(s for s in gate["steps"] if "actions/checkout" in s.get("uses", ""))
    assert "ref" not in checkout.get("with", {})            # issue_comment checks out the DEFAULT branch
    # least privilege: only the fix job may write code
    assert gate["permissions"].get("contents", "read") == "read" and "id-token" not in gate["permissions"]
    assert fix["permissions"]["contents"] == "write" and fix["permissions"]["id-token"] == "write"
    assert fix["needs"] == "gate" and "gate.outputs.go == 'yes'" in fix["if"]
    assert fix["steps"][0]["with"]["ref"] == "${{ needs.gate.outputs.branch }}"
    assert fix["steps"][0]["with"]["persist-credentials"] is False   # the push must re-trigger review/CI
    action = next(s for s in fix["steps"] if "claude-code-action" in s.get("uses", ""))
    prompt = action["with"]["prompt"]
    for must in ("CLAUDE.md", "OPERATING-POLICY", "./pipeline.sh", "tests/vectors/",
                 "protocol/omgp-protocol.yaml", "HIGH and MEDIUM", "DEFERRED"):
        assert must in prompt, must
    # The bound lives in agent-config.yml, not in the workflow: retuning it must not need a
    # workflow-scope push, because the fixer agent itself cannot edit .github/workflows/*.
    gate_script = next(s for s in gate["steps"] if "actions/github-script" in s.get("uses", ""))["with"]["script"]
    assert "review_fix_max_attempts" in gate_script and "ATTEMPT_LABELS" in gate_script
    assert "review-fix-1" not in gate_script.replace("`review-fix-${i + 1}`", "")   # no hard-coded attempt list
    assert "review_fix_max_attempts: 4" in (ROOT / ".github" / "agent-config.yml").read_text()
    # The severity policy is the point of the loop: LOW findings are not chased on their own.
    assert "Fix a LOW finding ONLY if it is in code you are already" in prompt
    assert "If EVERY finding is LOW, change no code at all" in prompt
    # The fixer must not touch the loop's own bounds, open PRs, or approve anything.
    assert ".github/workflows/" in prompt and "Do not approve" in prompt
    tools = action["with"]["claude_args"]
    assert "Bash(gh pr create*)" not in tools and "Bash(gh pr review*)" not in tools and "Bash(gh pr merge*)" not in tools
    # Both reviewers must emit the severity tokens the policy routes on.
    for wfn, job in (("claude-review.yml", "review"), ("red-team.yml", "attack-pr")):
        p = next(s for s in yaml.safe_load((ROOT / ".github" / "workflows" / wfn).read_text())["jobs"][job]["steps"]
                 if "claude-code-action" in s.get("uses", ""))["with"]["prompt"]
        assert "`[HIGH]`" in p and "`[MEDIUM]`" in p and "`[LOW]`" in p, wfn


def test_both_workflows_declare_the_same_seven_sections():
    gate = _script("ready-gate.yml", "validate")
    promote = _script("promote-queued.yml", "promote")
    names = ["Intent", "Spec references", "Acceptance criteria", "Evidence required",
             "Out of scope", "Dependencies", "Expected risk tier"]
    for n in names:
        assert f"'{n}'" in gate and f"'{n}'" in promote, n


# --- autonomous merge (agent-merge.yml; ruling 2026-09-02) --------------------------------------

MERGE = ROOT / ".github" / "workflows" / "agent-merge.yml"
MERGE_HARNESS = ROOT / "tests" / "workflows" / "agent_merge_harness.js"


@pytest.mark.skipif(shutil.which("node") is None,
                    reason="node not present (blind spot: workflow scripts not exercised in this environment)")
def test_agent_merge_against_mocked_github(tmp_path):
    f = tmp_path / "scripts.json"
    f.write_text(json.dumps({"merge": _script("agent-merge.yml", "merge")}))
    r = subprocess.run(["node", str(MERGE_HARNESS), str(f), str(ROOT)], capture_output=True, text=True, cwd=ROOT, timeout=120)
    print(r.stdout)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "FAIL" not in r.stdout
    assert "cases passed" in r.stdout


def test_agent_merge_wiring():
    """This is the only gate between agent work and `main` with no human in the loop, so its
    safety properties are asserted in the YAML as well as exercised in the harness."""
    wf = yaml.safe_load(MERGE.read_text())
    on = wf[True] if True in wf else wf["on"]
    # Default-branch definition runs (agent-approve's rationale): a PR cannot rewrite the
    # gate, the CODEOWNERS exception list or the tier knob in its own diff.
    assert on["issue_comment"]["types"] == ["created"]
    assert "schedule" in on and on["schedule"], "no sweep: the merge-ready moment is a check going green, not a comment"
    job = wf["jobs"]["merge"]
    checkout = next(s for s in job["steps"] if "actions/checkout" in s.get("uses", ""))
    assert "ref" not in checkout.get("with", {})
    assert job["permissions"]["contents"] == "write" and job["permissions"]["checks"] == "read"
    assert "id-token" not in job["permissions"]        # no agent runs here: this workflow only merges
    assert not [s for s in job["steps"] if "claude-code-action" in s.get("uses", "")]
    script = next(s for s in job["steps"] if "actions/github-script" in s.get("uses", ""))["with"]["script"]
    # The merge is pinned to the head the verdicts were issued for: with dismiss_stale_reviews
    # off, this is what stops an approval from carrying an unreviewed head to main.
    assert "sha: head" in script and "merge_method: 'merge'" in script
    # The claim release must never close a pull request (Copilot review on #117): issues and
    # PRs share a number namespace and this path goes through the issues API.
    assert "iss.pull_request" in script
    assert "tier >= 3" in script                        # T3 never
    assert "auto_merge_max_tier" in script
    for must in ("agent-authored", "needs-human", "VERDICT", "listForRef", "getCombinedStatusForRef", "CODEOWNERS"):
        assert must in script, must
    # Only the two paths OPERATING-POLICY §2 sanctions agents to write are exempt from the
    # CODEOWNERS refusal — nothing may be added here without a T3 ruling.
    assert "const SANCTIONED = ['/docs/OPEN-QUESTIONS.md', '/specs/**/tasks.md'];" in script
    cfg = (ROOT / ".github" / "agent-config.yml").read_text()
    assert "auto_merge_max_tier: 2" in cfg


def test_codeowners_still_protects_ground_truth_and_governance():
    """agent-merge exempts two owner paths; every other owned path must still be owned, or
    the exemption list silently widens as CODEOWNERS changes."""
    owned = [l.split()[0] for l in (ROOT / ".github" / "CODEOWNERS").read_text().splitlines()
             if l.strip() and not l.strip().startswith("#")]
    for must in ("/protocol/", "/tests/vectors/", "/docs/protocol-l3.md", "/docs/GOVERNANCE.md",
                 "/docs/OPERATING-POLICY.md", "/.github/", "/CLAUDE.md", "/pipeline.sh"):
        assert must in owned, must


# --- weekly governance feedback loop (continuous-improvement.yml) -------------------------------

CI_IMPROVE = ROOT / ".github" / "workflows" / "continuous-improvement.yml"


def test_continuous_improvement_is_read_only_by_construction():
    """The loop proposes governance changes and must never make them: CLAUDE.md and the
    constitution are CODEOWNERS-owned, and GOVERNANCE.md §3 makes an agent-authored T3 change
    a policy breach signal. `contents: read` is what enforces that — the prompt only says it."""
    wf = yaml.safe_load(CI_IMPROVE.read_text(encoding="utf-8"))
    job = wf["jobs"]["analyse"]
    assert wf["permissions"]["contents"] == "read", "the analyser must not be able to commit"
    assert wf["permissions"]["issues"] == "write"          # the filing step's channel
    assert wf["permissions"]["pull-requests"] == "read"    # it reads reviews, never writes them
    # A job-level `permissions:` block REPLACES the workflow-level one, so asserting only the
    # top-level grant would let six lines under jobs.analyse hand the job contents: write with
    # this test still green — the guarantee test not testing the guarantee (red team on #125).
    assert "permissions" not in job, "a job-level permissions block would override the guarantee above"
    action = next(s for s in job["steps"] if "claude-code-action" in s.get("uses", ""))
    tools = action["with"]["claude_args"]
    # No Edit. NOTE (review on #125): `Write` is not path-restricted, so the tool list alone
    # does not stop the agent overwriting CLAUDE.md in the workspace — `contents: read`,
    # asserted above, is what stops that reaching the repository. This assertion narrows the
    # blast radius; it is not the guarantee.
    # An ALLOW-LIST, not a denylist. The previous version asserted `"gh api" not in tools`
    # and friends, which a WIDENING defeats rather than an addition: `Bash(git*)` re-admits
    # git push, `Bash(gh*)` re-admits gh api — and both broad forms already exist elsewhere
    # in this repo, so that is not a hypothetical edit shape (red team on #125). A denylist
    # over a wildcard grammar cannot establish "by construction"; enumeration can.
    # Exactly one --allowedTools flag, and no CLI escape hatch that makes the enumeration
    # moot: `--dangerously-skip-permissions` / `--permission-mode` bypass allowedTools
    # entirely and a second `--allowedTools` (the CLI takes the last) would leave this
    # `re.search` reading a stale one — both survived the previous, single-match version of
    # this test (review round 3, #125).
    allowedtools_matches = re.findall(r'--allowedTools "([^"]*)"', tools)
    assert len(allowedtools_matches) == 1, "exactly one --allowedTools flag expected"
    assert "--dangerously-skip-permissions" not in tools
    assert "--permission-mode" not in tools
    granted = set(allowedtools_matches[0].split(","))
    permitted = {
        # reads over the PR/issue/run stream the analysis is made of
        "Bash(gh pr list*)", "Bash(gh pr view*)", "Bash(gh pr diff*)",
        "Bash(gh issue list*)", "Bash(gh issue view*)",
        "Bash(gh run list*)", "Bash(gh run view*)",
        # history for Stage 7, and the date for the window
        "Bash(git log*)", "Bash(git show*)", "Bash(git diff*)", "Bash(git blame*)", "Bash(date*)",
        # the two report files; see the note below on why Write is not the guarantee
        "Read", "Grep", "Glob", "Write",
    }
    assert granted <= permitted, f"tool grant widened beyond the read-only set: {granted - permitted}"
    # Belt, in the grammar's own terms: no bare-verb wildcard can appear at all.
    for wide in ("Bash(git*)", "Bash(gh*)", "Bash(*)", "Edit", "Bash(gh api*)"):
        assert wide not in granted, wide


def test_continuous_improvement_prompt_lives_outside_the_workflow():
    """The analysis is the part that will need tuning, and a prompt inlined in a workflow file
    can only be edited with the `workflow` OAuth scope the Claude App token does not carry
    (demonstrated on #113) — so the loop could never improve its own instructions."""
    wf = yaml.safe_load(CI_IMPROVE.read_text(encoding="utf-8"))
    action = next(s for s in wf["jobs"]["analyse"]["steps"] if "claude-code-action" in s.get("uses", ""))
    prompt = action["with"]["prompt"]
    assert ".github/agent-prompts/continuous-improvement.md" in prompt
    assert len(prompt) < 1200, "the prompt body belongs in the prompt file, not the workflow"
    body = (ROOT / ".github" / "agent-prompts" / "continuous-improvement.md").read_text(encoding="utf-8")
    # The issue is filed by a workflow step with the labels fixed in YAML, never by the agent:
    # an unconstrained `--label` would have let injected PR text reach `ready`, the label
    # agent-dispatch claims work by (HIGH, second review round on #125).
    filing = next(s for s in wf["jobs"]["analyse"]["steps"] if s.get("name") == "File the governance issue")
    assert "--label converge-audit --label needs-human" in filing["run"]
    assert "issue_title" in body and "duplicate_of" in body, "the agent must emit the issue as data"
    # The bindings that stop it reporting fiction about this repository's layout.
    assert ".specify/memory/constitution.md" in body, "constitution path binding missing"
    assert "docs/GOVERNANCE.md" in body
    assert "NO_UPDATE_REQUIRED" in body and "UPDATE_RECOMMENDED" in body
    for stage in range(1, 13):
        assert f"# Stage {stage} " in body, f"Stage {stage} missing"


def test_continuous_improvement_runs_weekly_and_no_update_is_a_success():
    wf = yaml.safe_load(CI_IMPROVE.read_text(encoding="utf-8"))
    on = wf[True] if True in wf else wf["on"]
    assert on["schedule"], "not scheduled"
    cron = on["schedule"][0]["cron"].split()
    assert cron[4] != "*", "not weekly (no day-of-week)"
    # Must not collide with agent-converge-audit, the other Monday-morning agent job.
    audit = yaml.safe_load((ROOT / ".github" / "workflows" / "agent-converge-audit.yml").read_text(encoding="utf-8"))
    aon = audit[True] if True in audit else audit["on"]
    assert on["schedule"][0]["cron"] != aon["schedule"][0]["cron"], "same slot as agent-converge-audit"
    # "No update required" must not fail the run: a self-improving system that cannot decline
    # to add a rule only ever adds rules. Assert the DECISION VALIDATOR accepts both literals
    # — the previous version of this test asserted `"exit 0" in run and "exit 1" not in run`,
    # which matched an unrelated branch and would have passed even if the validator were
    # narrowed to UPDATE_RECOMMENDED only, i.e. it did not test its own claim (review on
    # #125). It also forbade ever failing on missing outputs, locking in the blind spot the
    # next assertion now requires.
    steps = wf["jobs"]["analyse"]["steps"]
    check = next(s for s in steps if s.get("id") == "outputs")
    run = check["run"]
    assert "'UPDATE_RECOMMENDED','NO_UPDATE_REQUIRED'" in run.replace(", ", ",").replace('"', "'"), \
        "the decision validator must accept both literals"
    # ...and a run that produced NOTHING must fail, so "no update required" and "crashed" are
    # never the same observable outcome (review + red team on #125).
    assert "exit 1" in run, "missing outputs must fail the job, not warn"
    assert "::error::" in run
