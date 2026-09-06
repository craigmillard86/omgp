"""The unit stage proves the test SET, not only the check COUNT (#133, #143).

`UNIT_TEST_FLOOR` catches a mass drop of assertions. It cannot see one
`tests/{unit,property}/test_*.cpp` that is (1) compiled but never registered with
`add_test`, (2) present in the tree but compiled by no target (a CMakeLists line
re-pointed elsewhere), or (3) registered but not executed (DISABLED, or filtered
out) — as long as the remaining binaries keep the sum above the floor. The
`unit` stage therefore runs `tools/check_test_set.py`, which reads the BUILD
ARTEFACTS (compile_commands.json, the object files, `ctest --show-only`) and THIS
RUN's `--output-junit` record, and names every source that escaped. The floor
check stays, independent of it: each failure is reported on its own.

The execution evidence is ctest's JUnit record, not `LastTest.log`: the log
interleaves ctest's own framing with each test's verbatim stdout, so a test could
print `N/M Testing:` / `Command:` lines and mint execution evidence for a binary
that never ran (PR #172 red team, finding 1). In the JUnit file a test's stdout is
XML-escaped text inside its own `<testcase>`, the status is ctest's, and the test
is matched by NAME to `ctest --show-only`'s command, whose command[0] must BE the
target's binary (`build/native/<target>`; red team @94f2462 finding 2) and must
carry no arguments (a filtered run is not execution of the source — @94f2462
finding 1; @7dbf9d8 finding 2 had asked for tolerance because the LOG's argv
quoting broke the match, which the record does not need). Raw bytes in the output
(@7dbf9d8 finding 3) are ctest's to replace. The record must postdate every
registered binary (finding 4), is deleted before ctest runs so a stale one cannot
vouch (@94f2462 finding 3), and the source set must be non-empty (finding 5).

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
    args=None,            # {target: "argument"} — add_test registers the command WITH that argument
    scripts=None,         # {target: bash body} — what the fake binary runs instead of the EXECUTED echo
    props=None,           # {target: "PROP value"} — an extra set_tests_properties line
) -> Path:
    """A minimal tree in which `bash pipeline.sh unit` runs for real."""
    compiled = dict.fromkeys(sources, None) if compiled is None else compiled
    compiled = {t: (s or t) for t, s in compiled.items()}
    registered = tuple(sources) if registered is None else tuple(registered)
    binaries = tuple(sources) if binaries is None else tuple(binaries)
    args = args or {}
    scripts = scripts or {}
    props = props or {}
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
        body = scripts.get(t, f'echo "EXECUTED: {executed}"')
        _executable(build / t, f"#!/usr/bin/env bash\n{body}\n")
    if ctest:
        reg = []
        for t in registered:
            arg = f' "{args[t]}"' if t in args else ""
            reg.append(f'add_test([=[{t}]=] "{build / t}"{arg})')
            if t in disabled:
                reg.append(f"set_tests_properties([=[{t}]=] PROPERTIES DISABLED TRUE)")
            if t in props:
                reg.append(f"set_tests_properties([=[{t}]=] PROPERTIES {props[t]})")
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

    # --- PR #172 red team @7dbf9d8 --------------------------------------------------------

    def test_forged_ctest_framing_in_test_stdout_mints_nothing(self, tmp_path):
        # Finding 1: test_b is DISABLED; test_a prints the exact lines a LastTest.log parser
        # took as ctest's own framing for a test_b block. The record must be ctest's, not the
        # tests' stdout. (The red team's reproducer, verbatim in shape.)
        forge = (f'echo "EXECUTED: 600000"\n'
                 f'echo "2/2 Testing: forged"\n'
                 f'echo "Command: \\"$(dirname "$0")/test_b\\""\n'
                 f'echo "EXECUTED: 1"')
        root = make_tree(tmp_path, disabled=("test_b",), scripts={"test_a": forge})
        r = run_unit(root)
        assert r.returncode != 0, r.stdout + r.stderr
        assert "tests/unit/test_b.cpp" in r.stderr and "did not run" in r.stderr, r.stderr

    def test_registered_with_an_argument_is_refused(self, tmp_path):
        # Red team @7dbf9d8 finding 2 asked for arguments to be TOLERATED (the log's argv
        # quoting broke the match); red team @94f2462 finding 1 [HIGH] showed why they must
        # not be: `add_test(… test_x "[tag]")` runs a filtered subset, and a spec matching
        # nothing plus --allow-running-no-tests exits 0, status="run", `EXECUTED: 0` — twelve
        # of the real seventeen binaries fit inside the floor's slack at once. CMakeLists
        # registers `COMMAND ${name}` bare; anything else is not execution of the source.
        root = make_tree(tmp_path, args={"test_b": "[sometag]"})
        r = run_unit(root)
        assert r.returncode != 0, r.stdout + r.stderr
        assert "tests/unit/test_b.cpp" in r.stderr and "argument" in r.stderr, r.stderr
        assert "verified" not in r.stdout

    def test_executed_zero_is_not_execution(self, tmp_path):
        # The count-bearing half of the same finding: a run that reports `EXECUTED: 0` ran no
        # test case (the listener prints on testRunEnded regardless). Not execution.
        root = make_tree(tmp_path, scripts={"test_b": 'echo "EXECUTED: 0"'})
        r = run_unit(root)
        assert r.returncode != 0, r.stdout + r.stderr
        assert "tests/unit/test_b.cpp" in r.stderr and "EXECUTED: 0" in r.stderr, r.stderr

    def test_registration_of_a_same_named_binary_elsewhere_is_refused(self, tmp_path):
        # Red team @94f2462 finding 2 [MEDIUM]: registration was matched on the BASENAME of
        # command[0], so an add_test pointing at /somewhere/else/test_b vouched for the source
        # while the real build/native/test_b had no add_test at all — escape #1 of #133.
        root = make_tree(tmp_path, registered=("test_a",))
        decoy = tmp_path / "decoy" / "test_b"
        _executable(decoy, '#!/usr/bin/env bash\necho "EXECUTED: 600000"\n')
        with (root / "build/native/CTestTestfile.cmake").open("a") as f:
            f.write(f'add_test([=[decoy]=] "{decoy}")\n')
        r = run_unit(root)
        assert r.returncode != 0, r.stdout + r.stderr
        assert "tests/unit/test_b.cpp" in r.stderr and "not this target's binary" in r.stderr, r.stderr

    def test_stale_record_does_not_survive_a_failed_ctest(self, tmp_path):
        # Red team @94f2462 finding 3 [LOW]: `rm -f` of the record was an unkilled mutant. A
        # CTestTestfile with a syntax error makes ctest exit 8 WITHOUT writing a record
        # (measured, CMake 3.22); the previous run's record must not still be there to vouch.
        root = make_tree(tmp_path)
        r = run_unit(root)
        assert r.returncode == 0, r.stdout + r.stderr
        record = root / "build/native/Testing/junit.xml"
        assert record.exists()
        (root / "build/native/CTestTestfile.cmake").write_text('add_test([=[x]=] "/bin/true"\n')  # unclosed
        r = run_unit(root)
        assert r.returncode != 0, r.stdout + r.stderr
        assert not record.exists(), "the previous run's record survived a run in which ctest wrote none"
        r = run_tool("--ctest", cwd=root)
        assert r.returncode != 0 and "no execution record" in r.stderr, r.stderr

    def test_non_utf8_test_output_is_tolerated(self, tmp_path):
        # Finding 3: the property tests push arbitrary bytes through the codecs; a CAPTURE of a
        # raw buffer or an ASan report puts non-UTF-8 in the output. Not the gate's business.
        root = make_tree(tmp_path, scripts={"test_a": 'printf "raw \\xff byte\\n"\necho "EXECUTED: 600000"'})
        r = run_unit(root)
        assert r.returncode == 0, r.stdout + r.stderr
        assert "Traceback" not in r.stderr
        assert VERIFIED.search(r.stdout).group(1) == "2", r.stdout

    def test_execution_record_older_than_a_binary_is_refused(self, tmp_path):
        # Finding 4: "this run's" record was asserted, not checked. A binary relinked AFTER the
        # run is not what the record executed; the tool must say so rather than vouch.
        root = make_tree(tmp_path)
        r = run_unit(root)
        assert r.returncode == 0, r.stdout + r.stderr
        record = root / "build/native/Testing/junit.xml"
        assert record.exists(), "stage_unit must leave ctest's JUnit record in the build tree"
        later = record.stat().st_mtime + 60
        os.utime(root / "build/native/test_b", (later, later))
        r = run_tool("--ctest", cwd=root)
        assert r.returncode != 0, r.stdout
        assert "tests/unit/test_b.cpp" in r.stderr and "predates" in r.stderr, r.stderr
        assert r.stderr.count("unit: tests/") == 1, r.stderr

    def test_compile_entry_whose_object_is_missing_is_named(self, tmp_path):
        # Held finding: the "object missing" branch was reached by no test (a mutant deleting
        # it survived). compile_commands.json names the object; it is not on disk.
        root = make_tree(tmp_path)
        (root / "build/native/CMakeFiles/test_b.dir/tests/unit/test_b.cpp.o").unlink()
        r = run_unit(root)
        assert r.returncode != 0
        assert "tests/unit/test_b.cpp" in r.stderr and "missing" in r.stderr, r.stderr

    def test_skipped_test_that_printed_its_count_did_not_run(self, tmp_path):
        # A SKIP_RETURN_CODE test prints EXECUTED: and then exits with the skip code: ctest is
        # green, the floor is cleared, the record says status="notrun" (measured, CMake 3.22).
        # The status is load-bearing on its own — a mutant ignoring it survived the other cases.
        root = make_tree(tmp_path, props={"test_b": "SKIP_RETURN_CODE 77"},
                         scripts={"test_b": 'echo "EXECUTED: 600000"\nexit 77'})
        r = run_unit(root)
        assert r.returncode != 0, r.stdout + r.stderr
        assert "tests/unit/test_b.cpp" in r.stderr and "did not run" in r.stderr, r.stderr

    def test_ran_without_an_executed_line_did_not_count(self, tmp_path):
        # The converse: status="run", exit 0, but no EXECUTED: line — a binary that is not the
        # Catch2 harness at all (or whose reporter was replaced). The line is load-bearing too.
        root = make_tree(tmp_path, scripts={"test_b": 'echo "hello"'})
        r = run_unit(root)
        assert r.returncode != 0, r.stdout + r.stderr
        assert "tests/unit/test_b.cpp" in r.stderr and "did not run" in r.stderr, r.stderr

    def test_missing_record_is_a_named_failure(self, tmp_path):
        # stage_unit deletes the record before ctest; if ctest then writes none, the tool must
        # say so by name rather than fall over in the XML parser.
        root = make_tree(tmp_path)
        r = run_unit(root)
        assert r.returncode == 0, r.stdout + r.stderr
        (root / "build/native/Testing/junit.xml").unlink()
        r = run_tool("--ctest", cwd=root)
        assert r.returncode != 0, r.stdout
        assert "no execution record" in r.stderr and "Traceback" not in r.stderr, r.stderr

    def test_no_sources_is_a_failure_not_a_vacuous_pass(self, tmp_path):
        # Finding 5 / review MEDIUM: `verified 0 test binaries`, exit 0 is a dead gate with a
        # green line. An empty source set is the loudest possible escape.
        root = make_tree(tmp_path)
        empty = tmp_path / "empty"
        (empty / "tests" / "unit").mkdir(parents=True)
        r = run_tool("--ctest", "--root", str(empty), "--build", str(root / "build/native"))
        assert r.returncode != 0, r.stdout
        assert "no test sources" in r.stderr, r.stderr
        assert "verified" not in r.stdout


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

    def test_no_sources_is_a_failure_here_too(self, tmp_path):
        # Finding 5's bootstrap half: the source walk over nothing must not print
        # `verified 0` and exit 0.
        root = make_tree(tmp_path, ctest=False, sources=())
        r = run_unit(root)
        assert r.returncode != 0, r.stdout + r.stderr
        assert "no test sources" in r.stderr, r.stderr
        assert "verified" not in r.stdout


# --- controls on the real tree ------------------------------------------------------------

def test_real_build_tree_verifies_every_source():
    # Positive control: the tool, over the real build artefacts and the latest ctest record,
    # verifies exactly the sources in the tree. Needs a cmake build + a `unit` run (CI runs
    # build -> unit -> refimpl in that order); the tool itself refuses a record older than
    # any registered binary, so a stale record fails here rather than vouching.
    _need(TOOL.exists(), f"{TOOL.name} missing")
    _need((ROOT / "build/native/CTestTestfile.cmake").exists(), "no cmake build tree at build/native")
    _need((ROOT / "build/native/Testing/junit.xml").exists(), "no ctest record: run `./pipeline.sh unit` first")
    r = run_tool("--ctest")
    assert r.returncode == 0, r.stdout + r.stderr
    assert len(SOURCES) > 0
    assert int(VERIFIED.search(r.stdout).group(1)) == len(SOURCES), (r.stdout, SOURCES)


def test_real_artefacts_minus_one_registration_name_that_source(tmp_path):
    # Negative control on REAL artefacts, no rebuild: the real tests/, the real
    # compile_commands.json (its object paths still point into the real build tree), the real
    # ctest record, and the real CTestTestfile.cmake with test_link_master's lines deleted.
    _need(TOOL.exists(), f"{TOOL.name} missing")
    real = ROOT / "build/native"
    _need((real / "CTestTestfile.cmake").exists(), "no cmake build tree at build/native")
    _need((real / "Testing/junit.xml").exists(), "no ctest record: run `./pipeline.sh unit` first")
    root = tmp_path / "scratch"
    build = root / "build" / "native"
    (build / "Testing" / "Temporary").mkdir(parents=True)
    (root / "tests").symlink_to(ROOT / "tests", target_is_directory=True)
    shutil.copy(real / "compile_commands.json", build / "compile_commands.json")
    shutil.copy2(real / "Testing/junit.xml", build / "Testing/junit.xml")   # copy2: keep the mtime
    # The registration must run THIS build dir's <target> (red team @94f2462 finding 2): the
    # scratch tree's binaries are symlinks to the real ones, so realpath() meets on both sides.
    # Without these links every source would be named — the decoy-path refusal, not this control.
    for src in SOURCES:
        target = Path(src).stem
        (build / target).symlink_to(real / target)
    kept = [ln for ln in (real / "CTestTestfile.cmake").read_text().splitlines()
            if "test_link_master" not in ln and not ln.startswith("subdirs(")]
    (build / "CTestTestfile.cmake").write_text("\n".join(kept) + "\n")
    r = run_tool("--ctest", "--root", str(root), "--build", str(build))
    assert r.returncode != 0, r.stdout
    assert "tests/unit/test_link_master.cpp" in r.stderr and "not registered" in r.stderr, r.stderr
    # Only that one: every other source is still consistent with the same artefacts.
    assert r.stderr.count("unit: tests/") == 1, r.stderr
