"""Regression tests for the story-release workflows (ready-gate.yml, promote-queued.yml).

The workflows only fire on real label/close events, so their github-script bodies are
extracted from the YAML here and executed VERBATIM by tests/workflows/story_gate_harness.js
against a mocked GitHub API. Added after the review of PR #90 found that the evidence for a
section-parser fix was a one-off manual check with nothing committed (CLAUDE.md rule 8).
Cases include the two reported bugs: #26's bold section titles reading as "all sections
missing", and a bold aside inside a section ending it early (hiding a later dependency)."""
from __future__ import annotations

import json
import pathlib
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
    f.write_text(json.dumps({"route": _script("ci-failure-router.yml", "route")}))
    r = subprocess.run(["node", str(ROUTER_HARNESS), str(f), str(ROOT)], capture_output=True, text=True, cwd=ROOT, timeout=120)
    print(r.stdout)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "FAIL" not in r.stdout
    assert "cases passed" in r.stdout


def test_ci_failure_router_wiring():
    """The bounds that make the loop safe live in the YAML, not only in the script."""
    wf = yaml.safe_load(ROUTER.read_text())
    on = wf[True] if True in wf else wf["on"]           # PyYAML reads the bare key `on` as True
    assert set(on["workflow_run"]["workflows"]) == {"ci", "security"}
    assert on["workflow_run"]["types"] == ["completed"]
    assert "head_branch" in wf["concurrency"]["group"] and wf["concurrency"]["cancel-in-progress"] is False
    route, autofix = wf["jobs"]["route"], wf["jobs"]["autofix"]
    # least privilege per job (review on #96): the always-running router holds no OIDC/contents/actions write
    assert "permissions" not in wf
    assert autofix["permissions"]["id-token"] == "write" and autofix["permissions"]["contents"] == "write"
    assert "id-token" not in route["permissions"] and route["permissions"].get("contents", "read") == "read" and route["permissions"]["actions"] == "read"
    assert "conclusion == 'failure'" in route["if"] and "head_repository.full_name == github.repository" in route["if"]
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
    # The sweep decides nothing about the failure: it re-delivers, the bounds stay in `route`.
    for must in ("agent-authored", "needs-human", "auto-fix-1", "ci-failure-router sha=", "createWorkflowDispatch"):
        assert must in script, must
    assert "addLabels" not in script and "claude" not in script.lower()
    route = wf["jobs"]["route"]
    assert "workflow_dispatch" in route["if"] and "inputs.run_id" in route["if"]


def test_main_ci_failures_reach_triage():
    """The router files the issue; agent-triage handles it exactly like nightly-failure."""
    triage = yaml.safe_load((ROOT / ".github" / "workflows" / "agent-triage.yml").read_text())
    assert "'ci-failure'" in triage["jobs"]["triage"]["if"] and "'nightly-failure'" in triage["jobs"]["triage"]["if"]


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
            "agent-converge-audit.yml", "agent-triage.yml"]
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
    for l in ("auto-fix-1", "auto-fix-2", "ci-failure",
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
