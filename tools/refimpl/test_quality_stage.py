"""The `quality` stage is a CI gate with a pinned formatter (#135, written first).

Until #135 no CI job ran `./pipeline.sh quality`, although pipeline.sh's comment and
docs/GOVERNANCE.md §2 both said it ran on every merge, and a runner without clang-format
would have skipped the formatting half silently. These tests pin the wiring (the `native`
job names the stage; every workflow that can run the stage on a runner installs the pin),
the pin (tools/requirements.txt carries an exact clang-format version that the stage
checks against), the CI-vs-local behaviour of a missing or mismatched formatter under
either runner variable, the set of files the stage formats, the clang-tidy policy, and
the two negative controls #135's acceptance criteria name: a mis-formatted file reds the
stage, and a check_embedded finding reds it.

The stage is run for real (`bash pipeline.sh quality`) inside a scratch copy of the
tracked tree, so the negative controls mutate the copy and never the checkout. "Formatter
absent / wrong version" is simulated by SHADOWING `clang-format` on PATH — the stage has
no override hook (red team on #167, finding 2: an env override honoured on CI would let
a stand-in vouch for files it never read)."""
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
WORKFLOWS = ROOT / ".github" / "workflows"
PIN_RE = re.compile(r"^clang-format==(\d+\.\d+\.\d+)\s*(#.*)?$", re.M)
# The directories the stage formats (mirrors pipeline.sh stage_quality; esp32-host/ and
# third_party/ are outside it — the former is a recorded follow-up, the latter vendored).
FORMATTED_DIRS = ("core", "link", "l3", "sim", "cli", "transport", "tools", "tests")


def pinned_version() -> str:
    m = PIN_RE.search(REQUIREMENTS.read_text())
    assert m, "tools/requirements.txt must pin clang-format==MAJOR.MINOR.PATCH exactly (#135)"
    return m.group(1)


def tracked_files() -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z"], capture_output=True, cwd=ROOT, check=True).stdout
    return [os.fsdecode(r) for r in out.split(b"\0") if r]


def copy_tree(tmp_path: pathlib.Path) -> pathlib.Path:
    """Scratch copy of every tracked file (working-tree contents, so an uncommitted
    pipeline.sh edit is what gets exercised)."""
    dst = tmp_path / "tree"
    dst.mkdir()
    for rel in tracked_files():
        src = ROOT / rel
        if not src.is_file():
            continue
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, out)
    return dst


def shadow(tmp_path: pathlib.Path, *, clang_format: str | None = None, clang_tidy: pathlib.Path | None = None) -> pathlib.Path:
    """A bin dir to put FIRST on PATH. `clang_format`: the version the stand-in reports
    (it then accepts every file); None: a stand-in that answers nothing, i.e. the formatter
    is effectively absent even if a real one sits later on PATH. `clang_tidy`: a marker
    file the clang-tidy stand-in touches when invoked."""
    d = tmp_path / "shadow-bin"
    d.mkdir(exist_ok=True)
    cf = d / "clang-format"
    if clang_format is None:
        cf.write_text("#!/usr/bin/env bash\nexit 1\n")
    else:
        cf.write_text("#!/usr/bin/env bash\n"
                      f'[ "${{1:-}}" = "--version" ] && {{ echo "clang-format version {clang_format}"; exit 0; }}\n'
                      "exit 0\n")
    cf.chmod(0o755)
    if clang_tidy is not None:
        ct = d / "clang-tidy"
        ct.write_text(f"#!/usr/bin/env bash\ntouch '{clang_tidy}'\nexit 0\n")
        ct.chmod(0o755)
    return d


def run_quality(tree: pathlib.Path, *, ci: str | None, shadow_bin: pathlib.Path | None = None, extra_env=None):
    """`ci`: which runner variable to set — "GITHUB_ACTIONS", "CI", or None for local."""
    env = {k: v for k, v in os.environ.items() if k not in ("CI", "GITHUB_ACTIONS", "OMGP_CLANG_TIDY")}
    if ci:
        env[ci] = "true"
    if shadow_bin is not None:
        env["PATH"] = f"{shadow_bin}{os.pathsep}{env.get('PATH', '')}"
    env.update(extra_env or {})
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
    wf = yaml.safe_load((WORKFLOWS / "ci.yml").read_text())
    return wf["jobs"]["native"]["steps"]


def test_native_job_runs_the_quality_stage():
    steps = _native_steps()
    pipeline = [i for i, s in enumerate(steps) if "./pipeline.sh" in (s.get("run") or "")]
    assert pipeline, "native job has no ./pipeline.sh step"
    stages = steps[pipeline[0]]["run"].split("./pipeline.sh", 1)[1].split()
    assert "quality" in stages, f"native job runs {stages} — quality is not a CI gate (#135)"
    deps = [i for i, s in enumerate(steps) if "pip install -r tools/requirements.txt" in (s.get("run") or "")]
    assert deps and deps[0] < pipeline[0], "the pinned formatter is installed by the Python-deps step, which must precede the stage"


PIPELINE_CALL = re.compile(r"\./pipeline\.sh\b((?:[ \t]+[a-z0-9_-]+)*)")


def _job_texts():
    """(workflow, job, text of the job's steps) — YAML comments are gone after parsing,
    so a `./pipeline.sh` mention in a comment cannot count."""
    for p in sorted(WORKFLOWS.glob("*.yml")):
        wf = yaml.safe_load(p.read_text())
        for name, job in (wf.get("jobs") or {}).items():
            yield f"{p.name}:{name}", yaml.safe_dump(job.get("steps") or [])


def _can_reach_quality(text: str) -> bool:
    """A job reaches stage_quality if it grants an agent `Bash(./pipeline.sh*)` or runs
    ./pipeline.sh bare (default list) or with `quality` in its stage list."""
    if "Bash(./pipeline.sh*)" in text:
        return True
    for m in PIPELINE_CALL.finditer(text):
        stages = m.group(1).split()
        if not stages or "quality" in stages:
            return True
    return False


def test_every_job_that_can_run_the_quality_stage_installs_the_pin():
    # #167 review [HIGH] / red team finding 1: GITHUB_ACTIONS is set in EVERY job, so a job
    # that runs a bare ./pipeline.sh (or lets an agent run it) without installing the pin
    # reds on `quality` regardless of the change under test — and the agent loops
    # (agent-dispatch, review-fix, ci-failure-router) can then never reach green. Per JOB:
    # red-team.yml's first job installs the pin and its second did not.
    jobs = {job: text for job, text in _job_texts()}
    reaching = [j for j, t in jobs.items() if _can_reach_quality(t)]
    assert "ci.yml:native" in reaching and "agent-dispatch.yml:implement" in reaching, reaching   # both shapes seen
    missing = [j for j in reaching if "pip install -r tools/requirements.txt" not in jobs[j]]
    assert missing == [], f"these jobs can run stage_quality on a runner without the pin: {missing}"


def test_requirements_pin_clang_format_exactly():
    assert pinned_version()


def test_documented_stage_list_matches_the_default():
    default = re.search(r"^STAGES=\(\"\$\{@:-([^}]*)\}\"\)", (ROOT / "pipeline.sh").read_text(), re.M).group(1).split()
    claude = (ROOT / "CLAUDE.md").read_text()
    documented = re.search(r"Stages: `([^`]*)`", claude).group(1).split()
    assert [s for s in default if s not in documented] == [], f"CLAUDE.md documents {documented}; the default is {default}"


# --- formatter presence ---------------------------------------------------------------------

@pytest.mark.parametrize("ci", ["GITHUB_ACTIONS", "CI"])   # red team on #167, finding 3 (M2): both halves
def test_quality_on_ci_fails_without_the_pinned_formatter(tmp_path, ci):
    rc, out = run_quality(copy_tree(tmp_path), ci=ci, shadow_bin=shadow(tmp_path))
    assert rc != 0, out
    assert pinned_version() in out and "found 'none'" in out and "pip install -r tools/requirements.txt" in out, out


def test_quality_on_ci_fails_on_a_version_mismatch(tmp_path):
    rc, out = run_quality(copy_tree(tmp_path), ci="GITHUB_ACTIONS", shadow_bin=shadow(tmp_path, clang_format="0.0.1"))
    assert rc != 0, out
    assert pinned_version() in out and "0.0.1" in out, out


def test_quality_locally_discloses_a_missing_formatter_and_still_runs_check_embedded(tmp_path):
    rc, out = run_quality(copy_tree(tmp_path), ci=None, shadow_bin=shadow(tmp_path))
    assert rc == 0, out
    assert "formatting NOT checked" in out and "pip install -r tools/requirements.txt" in out, out
    assert "check_embedded:" in out and "clean" in out, out


def test_quality_checks_every_tracked_cpp_and_hpp_in_the_formatted_dirs(tmp_path):
    # Red team on #167, finding 3 (M1): `> 0` let the formatted set shrink to core/link/l3
    # unnoticed. The count must equal the tracked .cpp/.hpp files under FORMATTED_DIRS.
    expected = sum(1 for f in tracked_files()
                   if f.split("/", 1)[0] in FORMATTED_DIRS and f.endswith((".cpp", ".hpp")))
    assert expected > 40, expected
    rc, out = run_quality(copy_tree(tmp_path), ci="GITHUB_ACTIONS", shadow_bin=shadow(tmp_path, clang_format=pinned_version()))
    assert rc == 0, out
    m = re.search(rf"quality: clang-format {re.escape(pinned_version())} clean \((\d+) files\)", out)
    assert m and int(m.group(1)) == expected, (out, expected)


# --- clang-tidy policy (#167 review [MEDIUM]: the automatic local run is NOT narrowed) ---------

def _tree_with_compile_commands(tmp_path):
    tree = copy_tree(tmp_path)
    (tree / "build" / "native").mkdir(parents=True)
    (tree / "build" / "native" / "compile_commands.json").write_text("[]\n")
    return tree


def test_clang_tidy_policy(tmp_path):
    tree = _tree_with_compile_commands(tmp_path)
    mark = tmp_path / "tidy-ran"
    sb = shadow(tmp_path, clang_format=pinned_version(), clang_tidy=mark)
    # Local, clang-tidy on PATH + compile_commands.json present: runs, as before #135.
    rc, out = run_quality(tree, ci=None, shadow_bin=sb)
    assert rc == 0 and mark.exists(), out
    mark.unlink()
    # On a runner: NOT automatic (the build tree is a cache artefact and the analyser is
    # unpinned, so the verdict would depend on cache state) — a disclosed skip …
    rc, out = run_quality(tree, ci="GITHUB_ACTIONS", shadow_bin=sb)
    assert rc == 0 and not mark.exists() and "clang-tidy not run" in out, out
    # … unless asked for explicitly, in which case it runs, and if asked for but unavailable
    # it fails rather than skips.
    rc, out = run_quality(tree, ci="GITHUB_ACTIONS", shadow_bin=sb, extra_env={"OMGP_CLANG_TIDY": "1"})
    assert rc == 0 and mark.exists(), out
    shutil.rmtree(tree / "build")
    rc, out = run_quality(tree, ci="GITHUB_ACTIONS", shadow_bin=sb, extra_env={"OMGP_CLANG_TIDY": "1"})
    assert rc != 0 and "OMGP_CLANG_TIDY is set but" in out, out


# --- negative controls (#135: "a deliberately mis-formatted file reds the job; check_embedded
# findings red it too"). Each pairs with the positive control above / below so it cannot pass
# by failing for an unrelated reason.

def test_quality_reds_a_misformatted_file(tmp_path):
    need_pinned_formatter()
    tree = copy_tree(tmp_path)
    rc, out = run_quality(tree, ci="GITHUB_ACTIONS")
    assert rc == 0, f"positive control: the unmodified tree must be clean\n{out}"
    target = tree / "link" / "master.cpp"
    target.write_text("int   misformatted ;\n" + target.read_text())
    rc, out = run_quality(tree, ci="GITHUB_ACTIONS")
    assert rc != 0 and "code should be clang-formatted" in out and "link/master.cpp" in out, out


def test_quality_reds_a_check_embedded_finding(tmp_path):
    # Local mode with the formatter shadowed away: check_embedded is the only gate left
    # standing, so the red is attributable to it alone (no stand-in vouches for formatting).
    tree = copy_tree(tmp_path)
    sb = shadow(tmp_path)
    rc, out = run_quality(tree, ci=None, shadow_bin=sb)
    assert rc == 0, f"positive control: the unmodified tree must be clean\n{out}"
    target = tree / "l3" / "l3_payload.cpp"
    target.write_text(target.read_text() + "\nvoid omgp_forbidden() { throw 1; }\n")
    rc, out = run_quality(tree, ci=None, shadow_bin=sb)
    assert rc != 0 and "l3/l3_payload.cpp" in out and "exceptions (throw)" in out, out
