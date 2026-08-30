"""Unit tests for the shared blocking-dependency parser (tools/ci/dep-refs.js).

Direct unit tests, not routed through the workflow-extraction harness in
test_workflow_scripts.py, because ready-gate.yml / promote-queued.yml still carry their own
old duplicated copy of this logic pending the require() wiring described in the #92 PR thread
(this module fixes a mixed-blocking-and-excused-ref bullet dropping a real dependency, and a
range regex that missed the bare-second-endpoint form; see tools/ci/dep-refs.js for detail)."""
from __future__ import annotations

import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
TEST_JS = ROOT / "tests" / "workflows" / "dep_refs.test.js"


@pytest.mark.skipif(shutil.which("node") is None, reason="node not present")
def test_dep_refs_module():
    r = subprocess.run(["node", str(TEST_JS)], capture_output=True, text=True, cwd=ROOT, timeout=30)
    print(r.stdout)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "FAIL" not in r.stdout
