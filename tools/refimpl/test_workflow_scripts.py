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
    assert autofix["steps"][0]["with"]["ref"] == "${{ github.event.workflow_run.head_branch }}"
    # the triage loop also handles main-branch CI failures (same handling as nightly-failure)
    triage = yaml.safe_load((ROOT / ".github" / "workflows" / "agent-triage.yml").read_text())
    assert "'ci-failure'" in triage["jobs"]["triage"]["if"] and "'nightly-failure'" in triage["jobs"]["triage"]["if"]


# --- agent PR approval below T3 (ruling 2026-08-31) --------------------------------------------

APPROVE_HARNESS = ROOT / "tests" / "workflows" / "agent_approve_harness.js"


@pytest.mark.skipif(shutil.which("node") is None,
                    reason="node not present (blind spot: workflow scripts not exercised in this environment)")
def test_agent_approval_against_mocked_github(tmp_path):
    f = tmp_path / "scripts.json"
    f.write_text(json.dumps({"approve": _script("claude-review.yml", "approve")}))
    r = subprocess.run(["node", str(APPROVE_HARNESS), str(f), str(ROOT)], capture_output=True, text=True, cwd=ROOT, timeout=120)
    print(r.stdout)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "FAIL" not in r.stdout
    assert "cases passed" in r.stdout


def test_agent_approval_wiring():
    """The safety properties live in the YAML: verdicts are produced per head, approval is a
    separate minimal-permission job, and the knob is a T3 config value."""
    review = yaml.safe_load((ROOT / ".github" / "workflows" / "claude-review.yml").read_text())
    on = review[True] if True in review else review["on"]
    assert "synchronize" in on["pull_request"]["types"]          # verdicts must exist for the CURRENT head
    prompt = next(s for s in review["jobs"]["review"]["steps"] if "claude-code-action" in s.get("uses", ""))["with"]["prompt"]
    assert "VERDICT(review):" in prompt and "head" in prompt
    approve = review["jobs"]["approve"]
    assert approve["needs"] == "review"
    assert "pull_request" in approve["if"] and "head.repo.full_name == github.repository" in approve["if"]
    redteam = yaml.safe_load((ROOT / ".github" / "workflows" / "red-team.yml").read_text())
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
                          ("ci-failure-router.yml", "github-actions")]:  # workflow_run caused by bot pushes/dispatches
        wf = yaml.safe_load((ROOT / ".github" / "workflows" / workflow).read_text())
        actions = [s for j in wf["jobs"].values() for s in j.get("steps", []) if "claude-code-action" in s.get("uses", "")]
        assert actions, workflow
        for a in actions:
            assert bot in a["with"].get("allowed_bots", ""), (workflow, "allowed_bots must include " + bot)


def test_router_labels_are_provisioned():
    setup = (ROOT / "tools" / "gh-setup.sh").read_text()
    for l in ("auto-fix-1", "auto-fix-2", "ci-failure"):
        assert f"L {l} " in setup or f'L "{l}"' in setup, l


def test_both_workflows_declare_the_same_seven_sections():
    gate = _script("ready-gate.yml", "validate")
    promote = _script("promote-queued.yml", "promote")
    names = ["Intent", "Spec references", "Acceptance criteria", "Evidence required",
             "Out of scope", "Dependencies", "Expected risk tier"]
    for n in names:
        assert f"'{n}'" in gate and f"'{n}'" in promote, n
