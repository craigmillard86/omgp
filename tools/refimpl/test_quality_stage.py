"""The `quality` stage is a CI gate with a pinned formatter (#135, written first).

Until #135 no CI job ran `./pipeline.sh quality`, although pipeline.sh's comment and
docs/GOVERNANCE.md §2 both said it ran on every merge, and a runner without clang-format
would have skipped the formatting half silently. These tests pin the wiring (the `native`
job names the stage), the pin (tools/requirements.txt carries an exact clang-format
version that the stage checks against), the CI-vs-local behaviour of a missing or
mismatched formatter, and the two negative controls #135's acceptance criteria name: a
mis-formatted file reds the stage, and a check_embedded finding reds it.

The stage is run for real (`bash pipeline.sh quality`) inside a scratch copy of the
tracked tree, so the negative controls mutate the copy and never the checkout. "Formatter
absent / wrong version" is simulated through the stage's own OMGP_CLANG_FORMAT override,
as tools/refimpl/test_tooling.py does for fuzz/mutate tools."""
from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess

import pytest
import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
REQUIREMENTS = ROOT / "tools" / "requirements.txt"
PIN_RE = re.compile(r"^clang-format==(\d+\.\d+\.\d+)\s*(#.*)?$", re.M)


def pinned_version() -> str:
    m = PIN_RE.search(REQUIREMENTS.read_text())
    assert m, "tools/requirements.txt must pin clang-format==MAJOR.MINOR.PATCH exactly (#135)"
    return m.group(1)


def copy_tree(tmp_path: pathlib.Path) -> pathlib.Path:
    """Scratch copy of every tracked file (working-tree contents, so an uncommitted
    pipeline.sh edit is what gets exercised)."""
    dst = tmp_path / "tree"
    dst.mkdir()
    files = subprocess.run(["git", "ls-files", "-z"], capture_output=True, cwd=ROOT, check=True).stdout
    for rel in files.split(b"\0"):
        if not rel:
            continue
        src = ROOT / os.fsdecode(rel)
        if not src.is_file():
            continue
        out = dst / os.fsdecode(rel)
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, out)
    return dst


def fake_formatter(tmp_path: pathlib.Path, version: str) -> pathlib.Path:
    """A clang-format stand-in that reports `version` and accepts every file as formatted —
    isolates the version check and the check_embedded control from the real formatter."""
    p = tmp_path / "fake-clang-format"
    p.write_text("#!/usr/bin/env bash\n"
                 f'[ "${{1:-}}" = "--version" ] && {{ echo "clang-format version {version}"; exit 0; }}\n'
                 "exit 0\n")
    p.chmod(0o755)
    return p


def run_quality(tree: pathlib.Path, *, ci: bool, formatter: str | None = None):
    env = {k: v for k, v in os.environ.items() if k not in ("CI", "GITHUB_ACTIONS", "OMGP_CLANG_FORMAT")}
    if ci:
        env["GITHUB_ACTIONS"] = "true"
    if formatter is not None:
        env["OMGP_CLANG_FORMAT"] = formatter
    r = subprocess.run(["bash", str(tree / "pipeline.sh"), "quality"], capture_output=True, text=True,
                       cwd=tree, env=env, timeout=300)
    return r.returncode, r.stdout + r.stderr


def real_formatter_matches_pin() -> bool:
    exe = shutil.which("clang-format")
    if exe is None:
        return False
    out = subprocess.run([exe, "--version"], capture_output=True, text=True).stdout
    m = re.search(r"clang-format version (\d+\.\d+\.\d+)", out)
    return bool(m) and m.group(1) == pinned_version()


def need_pinned_formatter():
    """Negative controls need the real pinned formatter. Locally that is a disclosed skip;
    on a CI runner it is exactly the silent hole #135 closes, so it fails instead."""
    if real_formatter_matches_pin():
        return
    msg = (f"clang-format {pinned_version()} not on PATH (pip install -r tools/requirements.txt) — "
           "blind spot: formatting negative control not exercised")
    if os.environ.get("GITHUB_ACTIONS"):
        pytest.fail(msg)
    pytest.skip(msg)


# --- wiring -------------------------------------------------------------------------------

def _native_steps():
    wf = yaml.safe_load((ROOT / ".github" / "workflows" / "ci.yml").read_text())
    return wf["jobs"]["native"]["steps"]


def test_native_job_runs_the_quality_stage():
    steps = _native_steps()
    pipeline = [i for i, s in enumerate(steps) if "./pipeline.sh" in (s.get("run") or "")]
    assert pipeline, "native job has no ./pipeline.sh step"
    stages = steps[pipeline[0]]["run"].split("./pipeline.sh", 1)[1].split()
    assert "quality" in stages, f"native job runs {stages} — quality is not a CI gate (#135)"
    deps = [i for i, s in enumerate(steps) if "pip install -r tools/requirements.txt" in (s.get("run") or "")]
    assert deps and deps[0] < pipeline[0], "the pinned formatter is installed by the Python-deps step, which must precede the stage"


def test_requirements_pin_clang_format_exactly():
    assert pinned_version()


def test_documented_stage_list_matches_the_default():
    default = re.search(r"^STAGES=\(\"\$\{@:-([^}]*)\}\"\)", (ROOT / "pipeline.sh").read_text(), re.M).group(1).split()
    claude = (ROOT / "CLAUDE.md").read_text()
    documented = re.search(r"Stages: `([^`]*)`", claude).group(1).split()
    assert [s for s in default if s not in documented] == [], f"CLAUDE.md documents {documented}; the default is {default}"


# --- formatter presence ---------------------------------------------------------------------

def test_quality_on_ci_fails_without_the_pinned_formatter(tmp_path):
    rc, out = run_quality(copy_tree(tmp_path), ci=True, formatter="/nonexistent/clang-format")
    assert rc != 0, out
    assert pinned_version() in out and "pip install -r tools/requirements.txt" in out, out


def test_quality_on_ci_fails_on_a_version_mismatch(tmp_path):
    rc, out = run_quality(copy_tree(tmp_path), ci=True, formatter=str(fake_formatter(tmp_path, "0.0.1")))
    assert rc != 0, out
    assert pinned_version() in out and "0.0.1" in out, out


def test_quality_locally_discloses_a_missing_formatter_and_still_runs_check_embedded(tmp_path):
    rc, out = run_quality(copy_tree(tmp_path), ci=False, formatter="/nonexistent/clang-format")
    assert rc == 0, out
    assert "formatting NOT checked" in out and "pip install -r tools/requirements.txt" in out, out
    assert "check_embedded:" in out and "clean" in out, out


def test_quality_with_the_pinned_version_reports_the_files_it_checked(tmp_path):
    rc, out = run_quality(copy_tree(tmp_path), ci=True, formatter=str(fake_formatter(tmp_path, pinned_version())))
    assert rc == 0, out
    m = re.search(rf"quality: clang-format {re.escape(pinned_version())} clean \((\d+) files\)", out)
    assert m and int(m.group(1)) > 0, out


# --- negative controls (#135: "a deliberately mis-formatted file reds the job; check_embedded
# findings red it too"). Each pairs with the positive control above / below so it cannot pass
# by failing for an unrelated reason.

def test_quality_reds_a_misformatted_file(tmp_path):
    need_pinned_formatter()
    tree = copy_tree(tmp_path)
    rc, out = run_quality(tree, ci=True)
    assert rc == 0, f"positive control: the unmodified tree must be clean\n{out}"
    target = tree / "link" / "master.cpp"
    target.write_text("int   misformatted ;\n" + target.read_text())
    rc, out = run_quality(tree, ci=True)
    assert rc != 0 and "code should be clang-formatted" in out and "link/master.cpp" in out, out


def test_quality_reds_a_check_embedded_finding(tmp_path):
    tree = copy_tree(tmp_path)
    fake = str(fake_formatter(tmp_path, pinned_version()))
    rc, out = run_quality(tree, ci=True, formatter=fake)
    assert rc == 0, f"positive control: the unmodified tree must be clean\n{out}"
    target = tree / "l3" / "l3_payload.cpp"
    target.write_text(target.read_text() + "\nvoid omgp_forbidden() { throw 1; }\n")
    rc, out = run_quality(tree, ci=True, formatter=fake)
    assert rc != 0 and "l3/l3_payload.cpp" in out and "exceptions (throw)" in out, out
