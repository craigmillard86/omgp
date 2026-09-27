"""Shape guard for the ESP32-S3 `omgp_core` component (spec 003 T003, issue #674).

Why this exists. The `esp32` stage runs only inside the pinned `espressif/idf` image, so on
any path without docker — a bootstrap g++ run, a review reading the diff — nothing reads
`esp32-host/` at all except `test_esp32_link_smoke.py` and this file. What the component
buys is not one more green build: it is the only toolchain in the tree that COMPILES
`core/` with `-fno-exceptions -fno-rtti`, which is `docs/OPEN-QUESTIONS.md`'s 2026-09-27
option C — the recommended way CLAUDE.md rule 5's no-RTTI clause for `core/` stops being a
`tools/check_embedded.py` grep over the repo's current contents and becomes a toolchain
guarantee. A silent regression in this one file (a renamed `SRC_DIRS`, a dropped flag, a
`REQUIRES` line that stops naming the component in `main`) takes that away while every
stage stays green, because an ESP-IDF component that compiles nothing still builds.

So this pins, as SOURCE SHAPE:

  1. the component registers the repo's `core/` through `SRC_DIRS` — `idf_component_register`
     does not glob, so `SRCS` here would name no file and compile nothing;
  2. `-fno-exceptions -fno-rtti` reach `${COMPONENT_LIB}`, and `core/` actually has a source
     for them to act on (a flag on an empty component is decoration, not a guarantee). The
     component's existence is what gets `core/` compiled by that toolchain at all; the
     explicit flags pin them for `core/` if the project-wide sdkconfig default that also
     supplies them ever changes — see the component's own header comment for exactly how
     much each half is worth;
  3. `esp32-host/main/CMakeLists.txt`'s `REQUIRES` names `omgp_core`, without which ESP-IDF
     leaves the directory out of the build graph entirely;
  4. the component's own `REQUIRES` is exactly the set of sibling components `core/`'s
     sources include from — neither short (an unresolved symbol on the day something links
     it) nor speculative (a dependency no source uses, which #674's acceptance criteria
     forbid).

What this does NOT establish, stated rather than left to be assumed (CLAUDE.md rule 11):
these are static assertions about two CMake files, not evidence that the Xtensa build
configures or compiles — only the `esp32` stage can show that, and the PR cites it. Nor do
they establish that `core/`'s object files are LINKED into the firmware image. They are
not, at this head: nothing in `app_main` references a `core/` symbol, so every member of
`libomgp_core.a` is dropped from the link exactly as `test_esp32_link_smoke.py`'s docstring
measured for `omgp_link`'s own T003 stub. T045 (#716)'s `core_smoke.cpp` is what closes
that. Compilation under the flags is what this task buys, and compile time is when
`-fno-rtti` acts.
"""
from __future__ import annotations

import os.path
import pathlib
import re

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
COMPONENT = ROOT / "esp32-host" / "components" / "omgp_core" / "CMakeLists.txt"
MAIN_CMAKE = ROOT / "esp32-host" / "main" / "CMakeLists.txt"
CORE_DIR = ROOT / "core"

# The sibling ESP-IDF component for each portable directory `core/` is allowed to include
# from (specs/003-host-core-engine/contracts/core-cpp.md; research.md R-02). `core/` itself
# and the generated header arrive through this component's own INCLUDE_DIRS, not a REQUIRES.
COMPONENT_FOR_DIR = {"link": "omgp_link", "l3": "omgp_l3"}


def cmake_code(path: pathlib.Path) -> str:
    """File contents with `#` comments removed (CMake has no string-literal ambiguity here)."""
    return "\n".join(re.sub(r"#.*$", "", line) for line in path.read_text().splitlines())


def includes_of(path: pathlib.Path) -> set[str]:
    """Top-level directories `path` includes from, e.g. {"link"} for `#include "link/x.hpp"`.

    Comments are cut first, and `check_embedded.strip_comments_and_strings` is deliberately
    NOT reused for this: it blanks string literals, which is exactly where an include path
    lives. The prose in `core/core_engine.hpp` names `link/master.hpp` in a `//` comment
    without a `#include`, so a scanner that matched anywhere in the file would read the
    dependency surface off the documentation instead of off the code.
    """
    dirs: set[str] = set()
    in_block = False
    for raw in path.read_text().splitlines():
        line = raw
        if in_block:
            if "*/" not in line:
                continue
            line = line.split("*/", 1)[1]
            in_block = False
        if "/*" in line:
            head, _, tail = line.partition("/*")
            if "*/" in tail:
                line = head + tail.split("*/", 1)[1]
            else:
                line, in_block = head, True
        line = line.split("//", 1)[0]
        m = re.match(r'\s*#\s*include\s*"([^"/]+)/', line)
        if m:
            dirs.add(m.group(1))
    return dirs


def register_args(code: str) -> str:
    """The argument text of the `idf_component_register(...)` call."""
    m = re.search(r"idf_component_register\s*\((.*?)\)", code, re.S)
    assert m, "no idf_component_register(...) call"
    return m.group(1)


KEYWORDS = ("SRCS", "SRC_DIRS", "INCLUDE_DIRS", "PRIV_INCLUDE_DIRS", "REQUIRES",
            "PRIV_REQUIRES", "EMBED_FILES", "EMBED_TXTFILES", "LDFRAGMENTS", "WHOLE_ARCHIVE")


def keyword_values(args: str, keyword: str) -> list[str]:
    """Tokens following `keyword` up to the next register keyword, quotes stripped."""
    tokens = args.replace('"', " ").split()
    if keyword not in tokens:
        return []
    out = []
    for tok in tokens[tokens.index(keyword) + 1:]:
        if tok in KEYWORDS:
            break
        out.append(tok)
    return out


def resolved(value: str) -> pathlib.Path:
    """A register path argument as an absolute path, OMGP_ROOT expanded.

    Not `.resolve()`d against the filesystem: `build/gen/` legitimately does not exist until
    `./pipeline.sh codegen` has run, and this guard must hold on a fresh checkout too.
    """
    expanded = value.replace("${OMGP_ROOT}", str(COMPONENT.parent / ".." / ".." / ".."))
    return pathlib.Path(os.path.normpath(expanded))


@pytest.fixture(scope="module")
def component() -> str:
    assert COMPONENT.is_file(), f"{COMPONENT.relative_to(ROOT)} does not exist (T003, #674)"
    return cmake_code(COMPONENT)


def test_component_registers_the_repo_core_directory_via_src_dirs(component):
    args = register_args(component)
    assert not keyword_values(args, "SRCS"), (
        "SRCS lists files literally and idf_component_register does not glob, so SRCS here "
        "would compile nothing; SRC_DIRS is the form the sibling components use")
    src_dirs = [resolved(v) for v in keyword_values(args, "SRC_DIRS")]
    assert src_dirs == [CORE_DIR], f"SRC_DIRS should be exactly core/, got {src_dirs}"


def test_component_include_dirs_cover_core_the_generated_header_and_the_repo_root(component):
    # core/ for a bare `core_types.hpp`, build/gen for `omgp_protocol.h` (CLAUDE.md rule 4's
    # timing symbols), and the repo root because core/ and link/ headers include each other
    # root-relative (`#include "link/master.hpp"`).
    include_dirs = [resolved(v) for v in keyword_values(register_args(component), "INCLUDE_DIRS")]
    for required in (CORE_DIR, ROOT / "build" / "gen", ROOT):
        assert required in include_dirs, f"INCLUDE_DIRS is missing {required}: {include_dirs}"


def test_embedded_path_flags_reach_the_component_library(component):
    # The point of the whole file: docs/OPEN-QUESTIONS.md 2026-09-27 option C. Both flags,
    # on ${COMPONENT_LIB}, not on a directory-scope add_compile_options that a sibling
    # component would also inherit.
    m = re.search(r"target_compile_options\s*\(\s*\$\{COMPONENT_LIB\}\s+PRIVATE([^)]*)\)",
                  component)
    assert m, "no target_compile_options(${COMPONENT_LIB} PRIVATE ...) call"
    flags = m.group(1).split()
    for flag in ("-fno-exceptions", "-fno-rtti"):
        assert flag in flags, f"{flag} is not applied to the component library: {flags}"


def test_the_flags_are_not_vacuous_core_has_a_source_for_them_to_act_on():
    # A source-less ESP-IDF component registers as an INTERFACE library and compiles nothing,
    # so the flags above would be decoration and the no-RTTI guarantee would be empty. core/
    # gained core_engine.cpp in T015 (#686); this asserts the precondition rather than
    # trusting that it stays true.
    sources = sorted(p.name for p in CORE_DIR.glob("*.cpp"))
    assert sources, "core/ has no .cpp for SRC_DIRS to compile: the flags above prove nothing"


def test_main_requires_the_component_so_idf_puts_it_in_the_build_graph():
    # Without this, esp32-host/components/omgp_core/ is a directory on disk that the target
    # build never reads, and `./pipeline.sh esp32` stays green while compiling no core/ file.
    requires = keyword_values(register_args(cmake_code(MAIN_CMAKE)), "REQUIRES")
    assert "omgp_core" in requires, f"main's REQUIRES does not name omgp_core: {requires}"


def test_component_requires_exactly_the_siblings_core_includes_from(component):
    expected = set()
    for source in sorted(CORE_DIR.glob("*.[ch]pp")):
        for directory in includes_of(source):
            if directory in COMPONENT_FOR_DIR:
                expected.add(COMPONENT_FOR_DIR[directory])
    args = register_args(component)
    declared = set(keyword_values(args, "REQUIRES")) | set(keyword_values(args, "PRIV_REQUIRES"))
    assert declared == expected, (
        f"component REQUIRES {sorted(declared)} but core/'s sources include from "
        f"{sorted(expected)}: a missing one is an unresolved symbol the day anything links "
        f"core/, an extra one is the speculative dependency #674 rules out")


def test_the_include_scanner_reads_code_not_prose(tmp_path):
    # Discriminating control for the guard above, in the shape test_esp32_link_smoke.py uses
    # for its own matcher: on a file that mentions a directory only in comments, the scanner
    # must find nothing. Without this, that guard could be passing on the file headers.
    prose = tmp_path / "prose.hpp"
    prose.write_text('// includes link/master.hpp and l3/l3_types.hpp\n'
                     '/* #include "l3/l3_payload.hpp" */\n'
                     '#include "link/health.hpp"\n')
    assert includes_of(prose) == {"link"}
