"""The unit-test floor's VALUE lives in tests/unit-test-floor.txt, not inside pipeline.sh.

Why it moved (2026-09-12, autonomy gates). `pipeline.sh` is CODEOWNERS-owned because it is the
gate DEFINITION, and every user-story checkpoint raises the floor — so every checkpoint PR touched
an owned path and could never merge autonomously, whatever its tier. Measured over the last 25
merges: of the 8 PRs that were `task/*` AND `risk:t2` (so past the branch and tier gates), 5 were
blocked by exactly one file, `pipeline.sh`, and every one of those five was a floor raise.

What did NOT move is the rule. `pipeline.sh` still reads the datum, still enforces the floor on
both the ctest and bootstrap paths, and still fails closed on a datum it cannot parse — so
weakening the gate itself remains an owned-path change needing the maintainer.

The ratchet is the point of this file. "Raise when tests are added; NEVER lower" was previously
protected only by `pipeline.sh` being owner-reviewed; with the number in an unowned file that
protection has to be mechanical, so a decrease against origin/main fails here. Deleting this test
is itself a reduction of test content, which GOVERNANCE.md §3 scores T3 — so it cannot ride in on
an autonomous merge either, and the ratchet cannot be removed in the same breath as lowering.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
FLOOR_FILE = ROOT / "tests" / "unit-test-floor.txt"
PIPELINE = ROOT / "pipeline.sh"
ON_CI = bool(os.environ.get("GITHUB_ACTIONS"))


def parse_floor(text: str):
    """The datum: the first line that is exactly an integer. Comments (#) and blanks are the
    raise history, which lives with the number so a raise never touches the owned script."""
    for line in text.splitlines():
        if re.fullmatch(r"\d+", line.strip()):
            return int(line.strip())
    return None


def test_the_floor_datum_exists_and_parses():
    assert FLOOR_FILE.exists(), f"{FLOOR_FILE.relative_to(ROOT)} must hold the floor"
    value = parse_floor(FLOOR_FILE.read_text())
    assert value is not None, "the datum file must contain a bare integer line"
    assert value > 0


def test_pipeline_reads_the_datum_and_hardcodes_no_floor():
    """A literal left behind in pipeline.sh would mean checkpoints still edit the owned file."""
    text = PIPELINE.read_text()
    literal = re.search(r"^UNIT_TEST_FLOOR=\d+", text, re.M)
    assert not literal, f"pipeline.sh still hard-codes the floor: {literal.group(0) if literal else ''}"
    assert "unit-test-floor.txt" in text, "pipeline.sh must read the datum file"


def test_pipeline_still_enforces_the_floor():
    """The datum moved; the gate did not."""
    text = PIPELINE.read_text()
    assert text.count('-lt "$UNIT_TEST_FLOOR"') == 2, \
        "both the ctest and bootstrap paths must still compare against the floor"


def _committed_floor(ref: str):
    r = subprocess.run(["git", "show", f"{ref}:tests/unit-test-floor.txt"],
                       cwd=ROOT, capture_output=True, text=True)
    return parse_floor(r.stdout) if r.returncode == 0 else None


def test_the_floor_is_never_lowered():
    """The ratchet. A decrease against the default branch fails — that is the whole rule."""
    base = _committed_floor("origin/main")
    if base is None:
        # The file is new on this branch, or origin/main is not fetched. On CI (fetch-depth 0)
        # that is a real failure to check; locally it is a disclosed skip, never a silent pass.
        if ON_CI and _committed_floor("HEAD") is not None and \
                subprocess.run(["git", "rev-parse", "--verify", "origin/main"],
                               cwd=ROOT, capture_output=True).returncode == 0:
            pytest.fail("origin/main is fetched but carries no floor datum — the ratchet cannot be checked")
        pytest.skip("no origin/main floor datum to compare against (new file, or origin/main not fetched)")
    here = parse_floor(FLOOR_FILE.read_text())
    assert here >= base, (
        f"the unit-test floor was LOWERED: {base} on origin/main -> {here} here. "
        "Raise when tests are added; never lower to get green (that change is itself T3).")


@pytest.mark.skipif(shutil.which("bash") is None, reason="no bash")
@pytest.mark.parametrize("content,why", [
    ("not-a-number\n", "garbage"),
    ("# only a comment, no value\n", "no integer line"),
    (None, "file absent"),
])
def test_an_unreadable_datum_fails_closed(tmp_path, content, why):
    """A floor that cannot be read must stop the run, never default to 0 and pass everything."""
    tree = tmp_path / "tree"
    (tree / "tests").mkdir(parents=True)
    shutil.copy(PIPELINE, tree / "pipeline.sh")
    if content is not None:
        (tree / "tests" / "unit-test-floor.txt").write_text(content)
    r = subprocess.run(["bash", "pipeline.sh", "unit"], cwd=tree,
                       capture_output=True, text=True, timeout=120)
    assert r.returncode != 0, f"a {why} datum must fail the run, not pass it\n{r.stdout}{r.stderr}"
    assert "floor datum" in (r.stdout + r.stderr), \
        f"the failure must name the datum ({why}), not fail obscurely\n{r.stdout}{r.stderr}"
