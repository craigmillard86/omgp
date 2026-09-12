"""Unit tests for the shared agent no-op detector (tools/ci/agent-noop.js).

Direct unit tests of the module; test_workflow_scripts.py pins that agent-dispatch.yml and
review-fix.yml actually require() it, on steps that run on SUCCESS — a silent no-op is not a
failure, which is why the `if: failure()` guards those two workflows already carried could never
fire (#361)."""
from __future__ import annotations

import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
TEST_JS = ROOT / "tests" / "workflows" / "agent_noop_harness.js"


@pytest.mark.skipif(shutil.which("node") is None, reason="node not present")
def test_agent_noop_module():
    r = subprocess.run(["node", str(TEST_JS)], capture_output=True, text=True, cwd=ROOT, timeout=60)
    print(r.stdout)
    assert r.returncode == 0, r.stdout + r.stderr
    # Line-anchored, not a substring scan: several case NAMES contain "FAILS the job" (the job
    # failing loudly is the behaviour under test), and a bare `"FAIL" not in stdout` would report
    # a clean 29/29 run as a failure. Only a line the harness itself prefixes with FAIL counts.
    failed = [l for l in r.stdout.splitlines() if l.startswith("FAIL")]
    assert not failed, "\n".join(failed) + "\n" + r.stdout
    assert "cases passed" in r.stdout
