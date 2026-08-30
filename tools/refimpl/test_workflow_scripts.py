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
    assert wf["permissions"]["id-token"] == "write"
    route, autofix = wf["jobs"]["route"], wf["jobs"]["autofix"]
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
