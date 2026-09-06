"""The unit stage proves the test SET, not only the check COUNT (#133, #143).

`UNIT_TEST_FLOOR` catches a mass drop of assertions. It cannot see one
`tests/{unit,property}/test_*.cpp` that is (1) compiled but never registered with
`add_test`, (2) present in the tree but compiled by no target (a CMakeLists line
re-pointed elsewhere), or (3) registered but not executed (DISABLED, or filtered
out) — as long as the remaining binaries keep the sum above the floor. The
`unit` stage therefore runs `tools/check_test_set.py`, which reads the BUILD
ARTEFACTS (compile_commands.json, the object files, `ctest --show-only`) and THIS
RUN's `LastTest.log`, and names the first source that escaped. The floor check
stays, independent of it: each failure is reported on its own.

Every scenario below runs the real `stage_unit` (`bash pipeline.sh unit`) in a
FAKE tree — the pipeline script, the presets, two stub sources, a hand-written
compile_commands.json / CTestTestfile.cmake and two fake binaries that print an
`EXECUTED:` line — because a real cmake build per escape would cost minutes each
and prove nothing extra: the stage never reads the sources' contents. Two
controls then tie the fake to the real tree: the tool over the real build
artefacts verifies every real source, and the same artefacts with one binary's
registration removed name that source.
"""
import json
import os
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "check_test_set.py"
SOURCES = sorted(
    p.relative_to(ROOT).as_posix()
    for d in ("tests/unit", "tests/property")
    for p in (ROOT / d).glob("test_*.cpp")
)
ON_CI = bool(os.environ.get("GITHUB_ACTIONS"))
HAVE_CTEST = shutil.which("ctest") is not None


def _need(cond, why):
    if cond:
        return
    if ON_CI:
        pytest.fail(f"{why} (this is the ctest path CI runs; the check cannot be skipped here)")
    pytest.skip(why)


def _executable(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def make_tree(
    tmp_path: Path,
    *,
    sources=("test_a", "test_b"),
    compiled=None,        # {target: source-name} — which source each target's object was built from
    registered=None,      # targets with an add_test line
    disabled=(),          # registered targets marked DISABLED
    binaries=None,        # targets with a binary on disk
    executed=600000,      # what each fake binary prints as EXECUTED: (two of them clear the floor)
    ctest=True,           # False: no CTestTestfile.cmake -> stage_unit takes the bootstrap path
) -> Path:
    """A minimal tree in which `bash pipeline.sh unit` runs for real."""
    compiled = dict.fromkeys(sources, None) if compiled is None else compiled
    compiled = {t: (s or t) for t, s in compiled.items()}
    registered = tuple(sources) if registered is None else tuple(registered)
    binaries = tuple(sources) if binaries is None else tuple(binaries)
    root = tmp_path / "tree"
    build = root / "build" / "native"
    build.mkdir(parents=True)
    shutil.copy(ROOT / "pipeline.sh", root / "pipeline.sh")
    shutil.copy(ROOT / "CMakePresets.json", root / "CMakePresets.json")
    (root / "tools").mkdir()
    if TOOL.exists():
        shutil.copy(TOOL, root / "tools" / TOOL.name)
    lines = []
    for s in sources:
        src = root / "tests" / "unit" / f"{s}.cpp"
        src.parent.mkdir(parents=True, exist_ok=True)
        src.write_text(f"// stub {s}\n")
        lines.append(f"omgp_add_catch_test({s} tests/unit/{s}.cpp)")
    # The grep-predicate trap: a NON-registration line that still mentions test_b.cpp, so a
    # `grep <src> CMakeLists.txt` control would pass on the re-pointed tree (recorded in the PR).
    lines.append("set(PARKED tests/unit/test_b.cpp)")
    (root / "CMakeLists.txt").write_text("\n".join(lines) + "\n")
    cc = []
    for target, s in compiled.items():
        obj = f"CMakeFiles/{target}.dir/tests/unit/{s}.cpp.o"
        (build / obj).parent.mkdir(parents=True, exist_ok=True)
        (build / obj).write_bytes(b"\x7fELF-stub")
        src = root / "tests" / "unit" / f"{s}.cpp"
        cc.append({"directory": str(build),
                   "command": f"/usr/bin/c++ -std=gnu++17 -o {obj} -c {src}",
                   "file": str(src)})
    (build / "compile_commands.json").write_text(json.dumps(cc, indent=2))
    for t in binaries:
        _executable(build / t, f'#!/usr/bin/env bash\necho "EXECUTED: {executed}"\n')
    if ctest:
        reg = []
        for t in registered:
            reg.append(f'add_test([=[{t}]=] "{build / t}")')
            if t in disabled:
                reg.append(f"set_tests_properties([=[{t}]=] PROPERTIES DISABLED TRUE)")
        (build / "CTestTestfile.cmake").write_text("\n".join(reg) + "\n")
    return root


def run_unit(root: Path):
    return subprocess.run(["bash", "pipeline.sh", "unit"], cwd=root,
                          capture_output=True, text=True, timeout=300)


def run_tool(*args, cwd=ROOT):
    return subprocess.run([sys.executable, str(TOOL), *args], cwd=cwd,
                          capture_output=True, text=True, timeout=300)


VERIFIED = re.compile(r"^unit: verified (\d+) test binar(?:y|ies) \(compiled, registered, executed; ctest path\)$", re.M)


# --- ctest path: the four outcomes -------------------------------------------------------

@pytest.mark.skipif(not HAVE_CTEST and not ON_CI, reason="no ctest on PATH: the ctest-path scenarios need it")
class TestCtestPath:
    def test_consistent_tree_is_verified_and_the_log_survives(self, tmp_path):
        root = make_tree(tmp_path)
        r = run_unit(root)
        assert r.returncode == 0, r.stdout + r.stderr
        assert VERIFIED.search(r.stdout).group(1) == "2", r.stdout
        assert "unit: executed 1200000 check(s) (ctest path)" in r.stdout
        # `ctest --show-only` REWRITES LastTest.log (measured: 0 EXECUTED lines after it). The
        # tool must leave the run's log as ctest wrote it, or the next reader sees nothing ran.
        log = (root / "build/native/Testing/Temporary/LastTest.log").read_text()
        assert log.count("EXECUTED: 600000") == 2

    def test_built_but_not_registered_is_named(self, tmp_path):
        # Escape 1: test_b compiles and links; its add_test line is gone. The floor is cleared
        # by test_a alone (600000 > 509360), so only the set check can see it.
        root = make_tree(tmp_path, registered=("test_a",))
        r = run_unit(root)
        assert r.returncode != 0
        assert "tests/unit/test_b.cpp" in r.stderr and "not registered" in r.stderr, r.stderr
        assert "below floor" not in r.stderr

    def test_source_compiled_by_no_target_is_named(self, tmp_path):
        # Escape 2: CMakeLists re-pointed test_b's target at test_a.cpp. Binary, registration
        # and execution of a `test_b` all exist; tests/unit/test_b.cpp is compiled by nothing.
        root = make_tree(tmp_path, compiled={"test_a": "test_a", "test_b": "test_a"})
        r = run_unit(root)
        assert r.returncode != 0
        assert "tests/unit/test_b.cpp" in r.stderr and "compiled by no" in r.stderr, r.stderr

    def test_registered_but_not_executed_is_named(self, tmp_path):
        # Escape 3: DISABLED. ctest exits 0 ("Not Run (Disabled)"), the log block has no
        # EXECUTED line, and the floor is still cleared by test_a.
        root = make_tree(tmp_path, disabled=("test_b",))
        r = run_unit(root)
        assert r.returncode != 0
        assert "tests/unit/test_b.cpp" in r.stderr and "did not run" in r.stderr, r.stderr

    def test_floor_and_set_check_fail_independently(self, tmp_path):
        # Below the floor with a consistent set: the floor message alone, and the set check
        # still reports its verified count (it ran; it passed).
        root = make_tree(tmp_path, executed=5)
        r = run_unit(root)
        assert r.returncode != 0
        assert "below floor" in r.stderr and "tests/unit/" not in r.stderr, r.stderr
        assert VERIFIED.search(r.stdout).group(1) == "2", r.stdout
        # Both broken: both messages, neither masks the other.
        root = make_tree(tmp_path / "both", registered=("test_a",), executed=5)
        r = run_unit(root)
        assert r.returncode != 0
        assert "below floor" in r.stderr and "tests/unit/test_b.cpp" in r.stderr, r.stderr


# --- bootstrap path: one binary per source, every one runs --------------------------------

class TestBootstrapPath:
    def test_consistent_tree_is_verified(self, tmp_path):
        root = make_tree(tmp_path, ctest=False)
        r = run_unit(root)
        assert r.returncode == 0, r.stdout + r.stderr
        assert "unit: verified 2 test binaries (built and executed; bootstrap path)" in r.stdout
        assert "unit: executed 1200000 check(s) (bootstrap path)" in r.stdout

    def test_source_without_a_binary_is_named(self, tmp_path):
        # Before #133 the loop walked build/native/test_* — a source whose binary was never
        # built was simply absent from the walk. Now the loop walks the SOURCES.
        root = make_tree(tmp_path, ctest=False, binaries=("test_a",))
        r = run_unit(root)
        assert r.returncode != 0
        assert "tests/unit/test_b.cpp" in r.stderr and "no binary" in r.stderr, r.stderr


# --- controls on the real tree ------------------------------------------------------------

def test_real_build_tree_verifies_every_source():
    # Positive control: the tool, over the real build artefacts and the latest ctest log,
    # verifies exactly the sources in the tree. Needs a cmake build + a `unit` run (CI runs
    # build -> unit -> refimpl in that order, so the log is this run's).
    _need(TOOL.exists(), f"{TOOL.name} missing")
    _need((ROOT / "build/native/CTestTestfile.cmake").exists(), "no cmake build tree at build/native")
    _need((ROOT / "build/native/Testing/Temporary/LastTest.log").exists(), "no ctest log: run `./pipeline.sh unit` first")
    r = run_tool("--ctest")
    assert r.returncode == 0, r.stdout + r.stderr
    assert int(VERIFIED.search(r.stdout).group(1)) == len(SOURCES), (r.stdout, SOURCES)


def test_real_artefacts_minus_one_registration_name_that_source(tmp_path):
    # Negative control on REAL artefacts, no rebuild: the real tests/, the real
    # compile_commands.json (its object paths still point into the real build tree), the real
    # ctest log, and the real CTestTestfile.cmake with test_link_master's lines deleted.
    _need(TOOL.exists(), f"{TOOL.name} missing")
    real = ROOT / "build/native"
    _need((real / "CTestTestfile.cmake").exists(), "no cmake build tree at build/native")
    _need((real / "Testing/Temporary/LastTest.log").exists(), "no ctest log: run `./pipeline.sh unit` first")
    root = tmp_path / "scratch"
    build = root / "build" / "native"
    (build / "Testing" / "Temporary").mkdir(parents=True)
    (root / "tests").symlink_to(ROOT / "tests", target_is_directory=True)
    shutil.copy(real / "compile_commands.json", build / "compile_commands.json")
    shutil.copy(real / "Testing/Temporary/LastTest.log", build / "Testing/Temporary/LastTest.log")
    kept = [ln for ln in (real / "CTestTestfile.cmake").read_text().splitlines()
            if "test_link_master" not in ln and not ln.startswith("subdirs(")]
    (build / "CTestTestfile.cmake").write_text("\n".join(kept) + "\n")
    r = run_tool("--ctest", "--root", str(root), "--build", str(build))
    assert r.returncode != 0, r.stdout
    assert "tests/unit/test_link_master.cpp" in r.stderr and "not registered" in r.stderr, r.stderr
    # Only that one: every other source is still consistent with the same artefacts.
    assert r.stderr.count("unit: tests/") == 1, r.stderr
