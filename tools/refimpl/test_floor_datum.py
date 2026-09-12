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
    """The datum: the file must hold EXACTLY one bare integer line; anything else is ambiguous.

    Comments (#) and blanks are the raise history. Taking the FIRST match (as both readers did
    before round-1 of #420) meant a raise appended the way this file's own instructions describe
    kept the old, lower floor silently. Comment stripping and blank handling mirror pipeline.sh
    exactly, so the two readers cannot disagree about a given file."""
    vals = []
    for line in text.splitlines():
        stripped = line.split("#", 1)[0].strip()
        if re.fullmatch(r"\d+", stripped):
            vals.append(int(stripped))
    return vals[0] if len(vals) == 1 else None


def test_the_floor_datum_exists_and_parses():
    assert FLOOR_FILE.exists(), f"{FLOOR_FILE.relative_to(ROOT)} must hold the floor"
    value = parse_floor(FLOOR_FILE.read_text())
    assert value is not None, "the datum file must contain a bare integer line"
    assert value > 0


def _pipeline_refuses(tmp_path, text: str) -> bool:
    """Does the REAL stage_unit refuse this datum? Re-implementing the shell reader here would
    prove nothing if the two drifted, so this runs pipeline.sh itself in a scratch tree: it either
    names the datum and stops, or gets past the read and dies later for want of test sources."""
    tree = tmp_path / f"tree{abs(hash(text)) % 10000}"
    (tree / "tests").mkdir(parents=True)
    shutil.copy(PIPELINE, tree / "pipeline.sh")
    (tree / "tests" / "unit-test-floor.txt").write_text(text)
    r = subprocess.run(["bash", "pipeline.sh", "unit"], cwd=tree,
                       capture_output=True, text=True, timeout=120)
    out = r.stdout + r.stderr
    assert "floor datum" in out or "no test sources" in out, f"unexpected outcome:\n{out}"
    return "floor datum" in out


def test_an_appended_raise_is_refused_not_silently_ignored(tmp_path):
    """The file documents the value as the LAST bare integer and tells raisers to append their
    arithmetic; both readers took the FIRST (round-1 red team, finding 5). A raise written the way
    the file's own instructions describe would therefore not take effect, and nothing would say so:
    the ratchet compares first-to-first and stays green. Ambiguity must be refused, not guessed."""
    raised = FLOOR_FILE.read_text() + "# 700000 = 700005 executed minus 5 slack — raised later.\n\n700000\n"
    assert parse_floor(raised) is None, "two bare integers is ambiguous — the reader must refuse it"
    assert _pipeline_refuses(tmp_path, raised), "pipeline.sh must refuse an ambiguous datum too"


def test_both_readers_agree_on_whitespace(tmp_path):
    """parse_floor() stripped, grep -E '^[0-9]+$' did not: an indented datum parsed here and
    stopped the build there (round-1 red team, finding 6)."""
    for text in ("  582360\n", "582360\t\n"):
        assert (parse_floor(text) is None) == _pipeline_refuses(tmp_path, text), \
            f"the two readers disagree on {text!r}"


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


def ratchet_verdict(base, here, on_ci):
    """ok | lowered | fail | skip. Pure, so the fail-closed rule itself is testable."""
    if base is None:
        return "fail" if on_ci else "skip"
    return "ok" if here >= base else "lowered"


@pytest.mark.parametrize("base,here,on_ci,want", [
    (582360, 582360, True, "ok"), (582360, 582400, True, "ok"),
    (582360, 582359, True, "lowered"), (582360, 582359, False, "lowered"),
    (None, 582360, True, "fail"),      # on CI an uncheckable ratchet is a failure, never a pass
    (None, 582360, False, "skip"),     # locally, offline, it is a disclosed skip
])
def test_ratchet_verdict_fails_closed_on_ci(base, here, on_ci, want):
    assert ratchet_verdict(base, here, on_ci) == want


def _base_floor():
    """The floor on the branch this PR targets.

    The `native` job checks out with no `fetch-depth` (ci.yml:29), i.e. depth 1, and ci.yml:126
    says it in the repo's own words: "a depth-1 clone has no such ref". So `origin/main` is absent
    in the only job that runs this test, the old code took the skip branch every time, and its
    ON_CI guard was unreachable because that guard needed the same ref (round-1 review on #420).
    Fetch the base explicitly instead."""
    floor = _committed_floor("origin/main")
    if floor is not None:
        return floor
    base = os.environ.get("GITHUB_BASE_REF") or "main"
    subprocess.run(["git", "fetch", "--depth=1", "origin", base],
                   cwd=ROOT, capture_output=True, text=True, timeout=120)
    return _committed_floor("FETCH_HEAD")


def test_the_floor_is_never_lowered():
    """The ratchet. A decrease against the branch this targets fails — that is the whole rule."""
    verdict = ratchet_verdict(_base_floor(), parse_floor(FLOOR_FILE.read_text()), ON_CI)
    if verdict == "skip":
        pytest.skip("no base floor datum reachable offline (new file, or no network) — disclosed, never a silent pass")
    assert verdict != "fail", (
        "the base floor could not be obtained on CI, so the never-lowered ratchet could not run; "
        "failing closed rather than passing unchecked")
    assert verdict == "ok", (
        f"the unit-test floor was LOWERED to {parse_floor(FLOOR_FILE.read_text())} from {_base_floor()}. "
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
