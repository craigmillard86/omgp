"""Every CMake target that reaches `core/` links `omgp_core`, and no other target does.

`specs/003-host-core-engine/tasks.md` T016 (issue #687), the last Foundational task: once
T015 (#686) turned `omgp_core` from an INTERFACE stub into a STATIC library with a real
source, top up any consumer that needs it and is not reached by T001's
`omgp_add_catch_test` link set.

THE AUDIT T016 ASKED FOR, AND ITS RESULT AT THIS HEAD. Measured, not assumed — every
target declared in the build was checked, not only the four the issue names:

  target              created by            reaches core/?   links omgp_core?   change
  omgp_test_support   add_library             no               no               none
  omgp_canon          add_library             no               no               none
  omgp_l3_helper      add_library             no               no               none
  fuzz_header/_payload/_descriptor/_roundtrip/_frame
                      omgp_add_fuzz_target    no               no               none
  test_smoke          add_executable          no               no               none
  crc_helper          add_executable          no               no               none
  l3_helper           add_executable          no               no               none
  catch2_amalgamated  add_library             no               no               none
  test_core_types, test_core_callback_queue
                      omgp_add_catch_test     YES              yes (T001)       none

So T016's diff adds `omgp_core` to nothing: the only two sources in the tree that include a
`core/` header are the two Catch2 tests, and the function that builds every Catch2 binary
already links it. This file is the durable form of that result — the audit re-run on every
`refimpl` stage rather than recorded once in a PR body and left to rot.

WHAT IT ESTABLISHES, AND WHAT IT DOES NOT (CLAUDE.md rule 11). These are static assertions
over the CMake text and the `#include` graph, not evidence that anything links; the PR's
chained `cmake --preset native && cmake --build --preset native` is that. Their value is
that they fail EARLIER and in places the linker cannot speak for: `omgp_canon`,
`omgp_l3_helper` and the fuzz targets are libraries or preset-gated binaries, so a missing
`omgp_core` on one of them surfaces only once something links it under the right preset — a
`fuzz` build is not even configured by `./pipeline.sh`. A spurious ADD has no linker
symptom at all (a library nothing references is silently dropped), which is why the
opposite direction is asserted too: T016 forbids linking `omgp_core` into targets that do
not reference it.

The include scan is a control over the repo's current contents, not a guarantee: it reads
`#include "..."` lines outside comments, follows them while they resolve inside the repo,
and treats `third_party/` as opaque (vendored Catch2 cannot include `core/`, and walking
the amalgamated source would buy nothing). A source that reached `core/` through a
generated header, a `<>` include or a preprocessor trick would not be seen here — the
linker would still catch that one, in the direction that has a linker symptom.
"""
from __future__ import annotations

import pathlib
import re

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]

CORE_LIB = "omgp_core"
CORE_DIR = "core"
CATCH_FN = "omgp_add_catch_test"

# Where a quoted include is looked for, after the including file's own directory. Mirrors the
# include dirs the build hands round (root CMakeLists.txt's `include_directories(build/gen)`,
# the portable libraries' `target_include_directories(... PUBLIC <dir> <root> <root>/build/gen)`
# and `omgp_test_support`'s tests/support). A path that resolves nowhere here is simply not
# followed — see the module docstring on what that costs.
SEARCH_DIRS = ("", "tools", "tests/support", "build/gen", "l3", "link", "core")

# Sources under these prefixes are not scanned. Vendored third-party code is opaque by the
# terms of its vendoring; treating it as a normal source would walk the whole amalgamated
# Catch2 translation unit to reach the same answer.
OPAQUE_PREFIXES = ("third_party/",)

# Tokens that appear before the source list in add_library()/add_executable().
TYPE_KEYWORDS = {"STATIC", "SHARED", "MODULE", "OBJECT", "INTERFACE", "IMPORTED",
                 "ALIAS", "EXCLUDE_FROM_ALL", "GLOBAL"}
SCOPE_KEYWORDS = {"PUBLIC", "PRIVATE", "INTERFACE"}


# --------------------------------------------------------------------------- CMake reading

def _calls(text: str):
    """(command, [tokens]) for every call in `text`, comments stripped, in file order.

    Deliberately shallow: a flat scan is enough for the declarative subset these files use
    (no nested calls, no quoted parentheses), and the alternative is a CMake parser.
    """
    code = "\n".join(re.sub(r"#.*$", "", line) for line in text.splitlines())
    for m in re.finditer(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(([^()]*)\)", code, re.S):
        yield m.group(1).lower(), m.group(2).replace('"', " ").split()


def _subst(tokens: list[str], name: str, argn: list[str]) -> list[str]:
    """Expand a wrapper function's body tokens for one call: `${name}` and `${ARGN}`."""
    out: list[str] = []
    for tok in tokens:
        if tok == "${ARGN}":
            out.extend(argn)
        else:
            out.append(tok.replace("${name}", name))
    return out


class Project:
    """The target graph of a CMake tree: sources, direct link libraries, wrapper functions.

    Targets created through a wrapper function (`omgp_add_catch_test`,
    `omgp_add_fuzz_target`) are expanded by reading the function body once and substituting
    `${name}`/`${ARGN}` per call, so the audit covers them without the function's link list
    being duplicated here — the list stays authoritative in CMakeLists.txt.
    """

    def __init__(self, root: pathlib.Path):
        self.root = root
        self.targets: dict[str, dict] = {}
        self.functions: dict[str, list] = {}
        self._read(pathlib.Path("."))

    # -- loading

    def _read(self, reldir: pathlib.Path) -> None:
        path = self.root / reldir / "CMakeLists.txt"
        if not path.is_file():
            return
        fn_name: str | None = None
        for command, tokens in _calls(path.read_text()):
            if command == "function":
                fn_name = tokens[0]
                self.functions[fn_name] = []
                continue
            if command == "endfunction":
                fn_name = None
                continue
            if fn_name is not None:
                self.functions[fn_name].append((command, tokens))
                continue
            if command == "add_subdirectory":
                self._read(reldir / tokens[0])
            elif command in ("add_library", "add_executable"):
                self._declare(reldir, tokens[0], tokens[1:], via=None)
            elif command == "target_link_libraries":
                self._link(tokens[0], tokens[1:])
            elif command in self.functions:
                self._expand(reldir, command, tokens)

    def _declare(self, reldir, name, rest, via) -> None:
        sources = [str((reldir / t).as_posix()).removeprefix("./")
                   for t in rest if t not in TYPE_KEYWORDS]
        self.targets.setdefault(name, {"dir": reldir, "sources": [], "links": [], "via": via})
        self.targets[name]["sources"].extend(sources)
        self.targets[name]["via"] = via

    def _link(self, name, rest) -> None:
        if name not in self.targets:
            return
        self.targets[name]["links"].extend(t for t in rest if t not in SCOPE_KEYWORDS)

    def _expand(self, reldir, fn, tokens) -> None:
        name, argn = tokens[0], tokens[1:]
        for command, body in self.functions[fn]:
            body = _subst(body, name, argn)
            if command in ("add_library", "add_executable"):
                self._declare(reldir, body[0], body[1:], via=fn)
            elif command == "target_link_libraries":
                self._link(body[0], body[1:])

    # -- queries

    def link_closure(self, name: str) -> set[str]:
        """Every library `name` links, directly or through another target in this tree."""
        seen: set[str] = set()
        stack = list(self.targets[name]["links"])
        while stack:
            lib = stack.pop()
            if lib in seen:
                continue
            seen.add(lib)
            if lib in self.targets:
                stack.extend(self.targets[lib]["links"])
        return seen

    def reaches_core(self, name: str) -> bool:
        """True if any of `name`'s sources includes a `core/` header, transitively."""
        return bool(self.core_includers(name))

    def core_includers(self, name: str) -> list[str]:
        """The target's own sources whose include graph reaches `core/`, for the message."""
        return [src for src in self.targets[name]["sources"]
                if any(p.startswith(CORE_DIR + "/") for p in self._include_walk(src))]

    def _include_walk(self, start: str) -> set[str]:
        seen: set[str] = set()
        stack = [start]
        while stack:
            rel = stack.pop()
            if rel in seen or rel.startswith(OPAQUE_PREFIXES):
                continue
            seen.add(rel)
            path = self.root / rel
            if not path.is_file():
                continue
            for quoted in includes_of(path):
                target = self._resolve(rel, quoted)
                if target is not None:
                    stack.append(target)
        seen.discard(start)
        return seen

    def _resolve(self, includer: str, quoted: str) -> str | None:
        """A quoted include, as a repo-relative path, or None if it resolves outside the tree."""
        bases = [pathlib.PurePosixPath(includer).parent, *(pathlib.PurePosixPath(d) for d in SEARCH_DIRS)]
        for base in bases:
            rel = pathlib.PurePosixPath(str((base / quoted))).as_posix()
            rel = str(pathlib.PurePosixPath(_normpath(rel)))
            if rel.startswith(".."):
                continue
            if (self.root / rel).is_file():
                return rel
        return None


def _normpath(rel: str) -> str:
    parts: list[str] = []
    for part in rel.split("/"):
        if part in ("", "."):
            continue
        if part == ".." and parts and parts[-1] != "..":
            parts.pop()
        else:
            parts.append(part)
    return "/".join(parts)


def includes_of(path: pathlib.Path) -> list[str]:
    """Quoted include paths in `path`, reading code and not comments.

    Same reason `test_esp32_core_component.py` carries its own scanner rather than reusing
    `check_embedded.strip_comments_and_strings`: that one blanks string literals, which is
    exactly where an include path lives. Prose naming a header must not count — several
    files in this tree name `core/core_engine.hpp` in a comment without including it.
    """
    out: list[str] = []
    in_block = False
    for raw in path.read_text(errors="ignore").splitlines():
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
        m = re.match(r'\s*#\s*include\s*"([^"]+)"', line)
        if m:
            out.append(m.group(1))
    return out


# --------------------------------------------------------------------------- the audit

@pytest.fixture(scope="module")
def project() -> Project:
    return Project(ROOT)


def audit(project: Project) -> list[str]:
    """Every violation of T016's rule, as messages. Empty is the recorded result at HEAD.

    Two directions, because only one of them has a linker symptom:

    * reaches core/ but does not link omgp_core -> undefined `omgp::core::` references, at
      whatever point something finally links the target (never, for a preset-gated one);
    * links omgp_core without reaching core/ -> T016's forbidden spurious add, silent.

    Targets built by `omgp_add_catch_test` are exempt from the second direction only: the
    function applies ONE uniform link set to every Catch2 binary (root CMakeLists.txt), and
    T016 puts refactoring that shape out of scope. They are fully subject to the first.
    """
    problems = []
    for name in sorted(project.targets):
        if name == CORE_LIB:
            continue  # core/ includes its own headers; it does not link itself
        reaches = project.reaches_core(name)
        closure = project.link_closure(name)
        direct = project.targets[name]["links"]
        if reaches and CORE_LIB not in closure:
            problems.append(
                f"{name}: includes a core/ header ({', '.join(project.core_includers(name))}) "
                f"but does not link {CORE_LIB} (links: {sorted(closure) or 'nothing'})")
        if not reaches and CORE_LIB in direct and project.targets[name]["via"] != CATCH_FN:
            problems.append(
                f"{name}: links {CORE_LIB} but no source of it includes a core/ header "
                f"(sources: {', '.join(project.targets[name]['sources']) or 'none'})")
    return problems


def test_the_audit_is_clean(project):
    """T016's own criterion, as a standing check rather than a one-off PR-body table."""
    assert audit(project) == []


def test_the_audit_actually_saw_the_targets_it_is_meant_to_cover(project):
    """Discriminating control for the test above: an empty audit is also what a parser that
    found no targets would report. Names the four link sets the issue enumerates plus the
    two Catch2 binaries that DO reach core/, so a rename or a removal is loud here."""
    for name in ("omgp_test_support", "omgp_canon", "omgp_l3_helper", "fuzz_frame",
                 "test_smoke", "crc_helper", "l3_helper",
                 "test_core_types", "test_core_callback_queue"):
        assert name in project.targets, f"{name} is not in the parsed target graph"


def test_the_catch_test_helper_is_what_links_core_for_the_tests(project):
    """T001's line, on which the whole "nothing to add" result rests. Every Catch2 binary
    gets `omgp_core` from the one function, by construction — asserted so that removing it
    fails here as well as at the link step."""
    for name in ("test_core_types", "test_core_callback_queue"):
        assert project.targets[name]["via"] == CATCH_FN
        assert CORE_LIB in project.targets[name]["links"]


def test_only_catch_binaries_reach_core_today(project):
    """The audit's RESULT, recorded: no host-only library or tool references `core/`, which
    is why T016 adds `omgp_core` to none of them. Stays true as core/ grows — it is the day
    a tools/ or tests/support source starts using CoreEngine that a human should look."""
    reaching = sorted(n for n in project.targets
                      if n != CORE_LIB and project.reaches_core(n))
    via_catch = [n for n in reaching if project.targets[n]["via"] != CATCH_FN]
    assert via_catch == [], f"non-Catch2 targets now reach core/: {via_catch}"
    assert reaching, "no target reaches core/ at all — the scanner found nothing, not the tree"


def test_the_lower_layers_never_link_core(project):
    """The inverse direction of link/CMakeLists.txt's own note: `core/` is the layer ABOVE
    l3/ and link/, so a link edge pointing back down would invert the architecture
    (CLAUDE.md "Architecture invariants"). Nothing proposes it; this is the guard."""
    for name in ("omgp_l3", "omgp_link"):
        assert CORE_LIB not in project.link_closure(name), \
            f"{name} links {CORE_LIB}: core/ is above it, not below"


# --------------------------------------------------------------------------- controls

def _fake_tree(tmp_path: pathlib.Path, *, includes_core: bool, links_core: bool) -> pathlib.Path:
    """A two-target tree in the shape of the real one, for the controls below."""
    root = tmp_path / f"tree-{int(includes_core)}{int(links_core)}"  # one tree per call
    (root / "core").mkdir(parents=True)
    (root / "tools").mkdir()
    (root / "core" / "core_engine.hpp").write_text("#pragma once\n")
    (root / "core" / "CMakeLists.txt").write_text("add_library(omgp_core STATIC core_engine.cpp)\n")
    (root / "core" / "core_engine.cpp").write_text('#include "core_engine.hpp"\n')
    (root / "tools" / "thing.cpp").write_text(
        '#include "core/core_engine.hpp"\n' if includes_core else "// no core here\n")
    (root / "CMakeLists.txt").write_text(
        "add_subdirectory(core)\n"
        "add_library(omgp_thing STATIC tools/thing.cpp)\n"
        + (f"target_link_libraries(omgp_thing PUBLIC {CORE_LIB})\n" if links_core else ""))
    return root


def test_the_audit_flags_a_target_that_reaches_core_without_linking_it(tmp_path):
    """Without this, `test_the_audit_is_clean` could be green because the rule never fires."""
    problems = audit(Project(_fake_tree(tmp_path, includes_core=True, links_core=False)))
    assert len(problems) == 1 and "does not link omgp_core" in problems[0], problems


def test_the_audit_flags_a_spurious_core_link(tmp_path):
    """The direction with no linker symptom — T016's "no others" clause."""
    problems = audit(Project(_fake_tree(tmp_path, includes_core=False, links_core=True)))
    assert len(problems) == 1 and "but no source of it includes" in problems[0], problems


def test_the_audit_passes_a_correctly_wired_tree(tmp_path):
    """Both halves right, and both halves absent, are clean — so the two controls above are
    detecting the defect and not merely the fake tree."""
    assert audit(Project(_fake_tree(tmp_path, includes_core=True, links_core=True))) == []
    assert audit(Project(_fake_tree(tmp_path, includes_core=False, links_core=False))) == []


def test_the_include_scanner_reads_code_not_prose(tmp_path):
    """`core/CMakeLists.txt` and `mock_l3_node.hpp` both name core/ headers in comments. A
    scanner matching anywhere in the file would read the dependency graph off documentation."""
    prose = tmp_path / "prose.hpp"
    prose.write_text('// mentions core/core_engine.hpp\n'
                     '/* #include "core/core_types.hpp" */\n'
                     '#include "link/health.hpp"\n')
    assert includes_of(prose) == ["link/health.hpp"]


def test_the_wrapper_expansion_reads_the_link_list_from_cmake(tmp_path):
    """The Catch2 exemption is only sound if the link list really comes from the function
    body. A tree whose wrapper links nothing must show that, not the real tree's set."""
    root = tmp_path / "tree"
    (root / "tests").mkdir(parents=True)
    (root / "tests" / "t_a.cpp").write_text("int main() { return 0; }\n")
    (root / "CMakeLists.txt").write_text(
        "function(wrap name)\n"
        "  add_executable(${name} ${ARGN})\n"
        "  target_link_libraries(${name} PRIVATE only_this)\n"
        "endfunction()\n"
        "wrap(t_a tests/t_a.cpp)\n")
    project = Project(root)
    assert project.targets["t_a"]["sources"] == ["tests/t_a.cpp"]
    assert project.targets["t_a"]["links"] == ["only_this"]
    assert project.targets["t_a"]["via"] == "wrap"
