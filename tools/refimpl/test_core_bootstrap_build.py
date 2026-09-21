"""The bootstrap g++ path compiles `core/*.cpp` into every Catch2 binary and l3_helper.

`specs/003-host-core-engine/tasks.md` T002. `stage_build`'s `command -v cmake`
else-branch (pipeline.sh) is the fallback build for hosts without cmake, and
CLAUDE.md's "Build & test" promises it uses "identical sources and sanitizers" to
the CMake preset. It knew about `l3/` and `link/` only, so the first `core/`
source to land (T015, `core/core_engine.cpp`) would have compiled on the CMake
path and silently vanished from the bootstrap one — an undefined reference at
link time on exactly the constrained hosts the fallback exists for. Written
first: run against the pre-T002 pipeline.sh, the THREE `core` cases below fail —
the Catch2 source case, the Catch2 `-Icore` case and the l3_helper case, which
fails on both halves (measured at this head, not asserted; the other three cases
pass on both pipeline.sh versions, which is what makes the three informative).

WHY A FAKE TREE AND A STUB COMPILER, stated because it bounds what these tests
establish. Two constraints, neither negotiable here:

* The bootstrap branch cannot be reached on a normal host or runner at all. Both
  CI's `native` job and the agent hosts install cmake, and apt puts it in
  `/usr/bin`, so even `PATH=/usr/bin:/bin ./pipeline.sh` takes the CMake branch
  (measured; `.github/workflows/agent-dispatch.yml:157-165` and
  docs/OPEN-QUESTIONS.md 2026-09-15 "#62 (T044) AC2" record the same finding).
  These tests therefore build a PATH from scratch out of symlinks to the
  handful of tools `stage_build` runs, and ASSERT cmake is unreachable through
  it before running anything — the branch is entered by construction, not hoped
  for.
* `g++` is replaced by a stub that RECORDS its argv and compiles nothing. What
  T002 changes is which sources and include flags reach the compiler, and that
  is exactly what the stub captures; a real build of 20 sanitized binaries per
  case would cost minutes and prove nothing extra. So these tests establish the
  COMMAND LINES, not that the resulting objects link.

What this does NOT establish: that the real bootstrap path is green end to end.
ci.yml's `bootstrap` job (ruled 2026-09-16, docs/OPEN-QUESTIONS.md "#62 (T044)
AC2" option B) removes every cmake it can find, asserts none is reachable, runs
all local stages and greps its own log for the branch's tell — but read its LOG,
not its pass/fail: its pipeline step pipes into `tee` without `shell: bash`, so
the step's status is `tee`'s and the job is green whatever `pipeline.sh` exits
(ci.yml:156; reported by review round 2, not to be repaired from this PR — T3).
So these tests are the fast, targeted half; that job's log lines are the
end-to-end half, and its green tick on its own is not evidence.
"""
from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PIPELINE = ROOT / "pipeline.sh"

# Everything stage_build shells out to, plus bash itself. A PATH built from these and
# nothing else cannot reach cmake — which is the point (see the module docstring).
NEEDED_TOOLS = (
    "bash", "sh", "dirname", "basename", "mkdir", "ls", "find", "sort", "uniq",
    "readlink", "cat", "rm", "grep", "sed", "stat", "tr", "xargs", "awk", "cut", "head", "tail",
)

# The fake tree's sources, one per role stage_build knows about. Contents are never
# compiled (the g++ stub records argv and exits 0), so a marker comment is enough.
FAKE_SOURCES = (
    "tests/unit/test_alpha.cpp",          # an ordinary Catch2 binary
    "tests/unit/test_smoke.cpp",          # keeps its own main; must stay on the bare compile line
    "tests/support/support_stub.cpp",
    "tools/canonical.cpp",
    "tools/l3_helper_dispatch.cpp",
    "tools/l3_helper.cpp",
    "tools/crc_helper.cpp",
    "third_party/catch2/catch_amalgamated.cpp",
    "l3/l3_stub.cpp",                     # control: already compiled in before T002
    "link/link_stub.cpp",                 # control: already compiled in before T002
)
# TWO core sources, not one, and every assertion below names both: `ls core/*.cpp` expands
# to a list, so a regression that compiles only a SUBSET of it (a stray `| head -1`, a
# `$(ls core/*.cpp | ...)` pipeline) is invisible to a suite that only ever checks one file.
# Both are flat: `core/*.cpp` is one level deep, matching the l3srcs/linksrcs form T002 asked
# to mirror and the flat core/ that specs/003-host-core-engine/plan.md:118-125 plans. A source
# in a core/ SUBDIRECTORY is not compiled by that glob (red team round 3, F1) — out of scope
# here and left to a follow-up, so no assertion below claims otherwise.
CORE_SOURCES = ("core/core_stub.cpp", "core/core_second.cpp")


def _write(path: pathlib.Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def make_tree(tmp_path: pathlib.Path, *, with_core: bool) -> tuple[pathlib.Path, pathlib.Path, str]:
    """A minimal tree in which `bash pipeline.sh build` takes the bootstrap branch.

    Returns (tree, gxx_log, path) — the REAL pipeline.sh is copied from the working
    tree, not from git, so an uncommitted edit is what gets exercised (the same rule
    test_quality_stage.py's copy_tree() follows).
    """
    tree = tmp_path / "tree"
    tree.mkdir()
    shutil.copy2(PIPELINE, tree / "pipeline.sh")
    for rel in FAKE_SOURCES:
        _write(tree / rel, f"// fake source for the bootstrap-path test: {rel}\n")
    if with_core:
        for rel in CORE_SOURCES:
            _write(tree / rel, f"// fake source for the bootstrap-path test: {rel}\n")
    else:
        (tree / "core").mkdir(exist_ok=True)   # present but empty, as core/ really is until T015
    (tree / "tests/property").mkdir(parents=True, exist_ok=True)   # exists and empty, as in the real tree at this head

    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    for tool in NEEDED_TOOLS:
        real = shutil.which(tool)
        if real:
            (bin_dir / tool).symlink_to(real)
    gxx_log = tmp_path / "gxx.log"
    gxx = bin_dir / "g++"
    # One line per invocation, argv joined by spaces. No fake source name contains a
    # space, so the joined line is unambiguous for the substring assertions below.
    gxx.write_text('#!/bin/sh\nprintf \'%s\\n\' "$*" >> "$GXX_LOG"\n')
    gxx.chmod(0o755)
    return tree, gxx_log, str(bin_dir)


def run_build(tmp_path: pathlib.Path, *, with_core: bool) -> tuple[subprocess.CompletedProcess, list[str]]:
    tree, gxx_log, path = make_tree(tmp_path, with_core=with_core)
    # The branch is entered by construction, not assumed: if cmake were reachable here,
    # stage_build would take the CMake branch and every assertion below would pass or
    # fail for reasons that have nothing to do with T002.
    assert shutil.which("cmake", path=path) is None, \
        "the constructed PATH must not reach cmake, or stage_build does not take the bootstrap branch"
    env = {"PATH": path, "GXX_LOG": str(gxx_log), "HOME": str(tmp_path), "LC_ALL": "C"}
    r = subprocess.run([shutil.which("bash"), "pipeline.sh", "build"],
                       cwd=tree, env=env, capture_output=True, text=True, timeout=300)
    lines = gxx_log.read_text().splitlines() if gxx_log.exists() else []
    return r, lines


def only_line(lines: list[str], needle: str) -> str:
    hits = [l for l in lines if needle in l]
    assert len(hits) == 1, f"expected exactly one g++ invocation mentioning {needle!r}, got {len(hits)}: {hits}"
    return hits[0]


@pytest.fixture(scope="module")
def with_core(tmp_path_factory):
    r, lines = run_build(tmp_path_factory.mktemp("core"), with_core=True)
    assert r.returncode == 0, f"bootstrap build failed with a core/*.cpp present:\n{r.stdout}\n{r.stderr}"
    assert "cmake not found -> bootstrap g++ build" in r.stdout, \
        f"stage_build did not take the bootstrap branch:\n{r.stdout}"
    assert lines, "the g++ stub recorded no invocation at all"
    return lines


def test_catch_binary_compiles_core_sources(with_core):
    """T002's first clause. Before it, this line named l3/ and link/ only, so a test
    using a symbol from core/*.cpp failed with an undefined reference on this path. EVERY
    core source must be on the line, not merely one: compiling a subset is the same
    undefined-reference failure for whichever source was dropped."""
    line = only_line(with_core, "tests/unit/test_alpha.cpp")
    assert "l3/l3_stub.cpp" in line and "link/link_stub.cpp" in line, \
        f"control: the pre-existing l3/ and link/ sources are on the line at all:\n{line}"
    for src in CORE_SOURCES:
        assert src in line, f"core/*.cpp is not compiled into a Catch2 binary ({src} missing):\n{line}"


def test_catch_binary_gets_core_include_dir(with_core):
    """-Icore is load-bearing for includers OUTSIDE core/ — a test or a tools/ source
    writing `#include "core_types.hpp"` (what T011/T015's headers are named). It is NOT
    needed by a core/ source including its own sibling header by bare name: the quoted
    form searches the including file's own directory first, so that case compiles either
    way. Distinct failure from the one above: a missing include is a compile error, a
    missing source a link error."""
    line = only_line(with_core, "tests/unit/test_alpha.cpp")
    assert "-Icore" in line, f"-Icore is not on the Catch2 compile line:\n{line}"


def test_l3_helper_links_core_sources(with_core):
    """T002's clause "and into l3_helper" — the task text, not an analogy. Both halves,
    as on the Catch2 line: without -Icore a tools/ source including a core/ header by
    bare name fails to compile here while building fine on the CMake path (omgp_core's
    INTERFACE include dirs). Asserting the source alone left dropping -Icore from this
    line a surviving mutant (red team, round 1)."""
    line = only_line(with_core, "tools/l3_helper.cpp")
    for src in CORE_SOURCES:
        assert src in line, f"core/*.cpp is not linked into l3_helper ({src} missing):\n{line}"
    assert "-Icore" in line, f"-Icore is not on the l3_helper line:\n{line}"


def test_smoke_keeps_its_bare_compile_line(with_core):
    """test_smoke has its own main, so it never links Catch2's — and never core/'s
    either. Unchanged by T002; asserted so a future widening of the compile line is a
    failure here rather than a duplicate-symbol build error."""
    line = only_line(with_core, "tests/unit/test_smoke.cpp")
    for other in (*CORE_SOURCES, "l3/l3_stub.cpp", "link/link_stub.cpp", "catch2.o"):
        assert other not in line, f"test_smoke's compile line must stay bare, found {other}:\n{line}"


def test_empty_core_directory_is_not_an_error(tmp_path):
    """core/ holds only .gitkeep until T015, so the glob must tolerate zero matches —
    the guarded `ls ... 2>/dev/null || true` form, not a bare glob that would pass the
    literal `core/*.cpp` to g++ (and, under `set -e`/`pipefail`, kill the stage)."""
    r, lines = run_build(tmp_path, with_core=False)
    assert r.returncode == 0, f"bootstrap build failed with an empty core/:\n{r.stdout}\n{r.stderr}"
    assert "cmake not found -> bootstrap g++ build" in r.stdout
    line = only_line(lines, "tests/unit/test_alpha.cpp")
    assert "core/*.cpp" not in line, f"an unmatched glob was passed through literally:\n{line}"


def test_clang_tidy_directory_set_covers_core():
    """T002's second clause. Already satisfied at e272d93 (`find core link l3` in
    stage_quality), so T002 verifies rather than duplicates it — this is the check that
    keeps it true. A control on the file's current contents, not a guarantee."""
    text = PIPELINE.read_text()
    m = re.search(r"^\s*find ([a-z0-9 ]+) -name '\*\.cpp' 2>/dev/null \\\n\s*\| xargs -r clang-tidy", text, re.M)
    assert m, "stage_quality's clang-tidy invocation no longer matches its expected shape"
    assert "core" in m.group(1).split(), f"clang-tidy directory set does not cover core/: {m.group(1)!r}"
