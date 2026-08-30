"""Tests for tools/check_embedded.py (spec 001 T014 — tests first for the tool itself)."""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "check_embedded.py"
YAML = ROOT / "protocol" / "omgp-protocol.yaml"


def run(tmp_path: pathlib.Path, files: dict[str, str], dirname: str = "l3"):
    d = tmp_path / dirname
    d.mkdir()
    for name, body in files.items():
        (d / name).write_text(body)
    r = subprocess.run([sys.executable, str(TOOL), "--dirs", str(d), "--yaml", str(YAML)],
                       capture_output=True, text=True)
    return r.returncode, r.stdout


CLEAN = "// protocol-l3 §3\n#include <cstdint>\nuint8_t f(uint8_t x) { return x + 1; }\n"


def test_clean_file_passes(tmp_path):
    rc, out = run(tmp_path, {"a.cpp": CLEAN})
    assert rc == 0, out
    assert "clean" in out


def test_flags_new_throw_vector(tmp_path):
    body = ("// protocol-l3 §3\n#include <vector>\nvoid f() {\n  int* p = new int(1);\n"
            "  std::vector<int> v;\n  throw 1;\n}\n")
    rc, out = run(tmp_path, {"bad.cpp": body})
    assert rc == 1
    assert "dynamic allocation (new)" in out
    assert "allocating STL type" in out
    assert "exceptions (throw)" in out
    assert "forbidden header" in out


def test_flags_bare_protocol_literal(tmp_path):
    body = "// protocol-l3 §3.1\nbool is_error(unsigned op) { return op == 0x7F; }\n"
    rc, out = run(tmp_path, {"lit.cpp": body})
    assert rc == 1
    # 0x7F is both OP_ERROR and ADDR_module_max; every symbol sharing the value is named
    assert "protocol literal 0x7F duplicates" in out
    assert "OP_ERROR" in out and "ADDR_module_max" in out


def test_literal_ok_escape_accepted(tmp_path):
    body = ("// protocol-l3 §3.1\n"
            "unsigned mask() { return 0x7F; } // literal-ok: 7-bit mask, not the opcode\n")
    rc, out = run(tmp_path, {"ok.cpp": body})
    assert rc == 0, out


def test_literal_in_comment_or_string_ignored(tmp_path):
    body = ('// protocol-l3 §3.1 — ERROR is 0x7F\n'
            'const char* s() { return "0x7F 2048"; } /* 4095 */\n')
    rc, out = run(tmp_path, {"c.cpp": body})
    assert rc == 0, out


def test_small_values_not_policed(tmp_path):
    body = "// protocol-l3 §3\nunsigned f(unsigned x) { return (x & 0x01) | 0x02 | 4; }\n"
    rc, out = run(tmp_path, {"small.cpp": body})
    assert rc == 0, out


def test_missing_citation_flagged_in_l3(tmp_path):
    rc, out = run(tmp_path, {"nocite.cpp": "#include <cstdint>\nuint8_t g() { return 1; }\n"})
    assert rc == 1
    assert "no spec citation" in out


def test_citation_not_required_outside_cite_dirs(tmp_path):
    rc, out = run(tmp_path, {"nocite.cpp": "#include <cstdint>\nuint8_t g() { return 1; }\n"},
                  dirname="core")
    assert rc == 0, out


def test_missing_citation_flagged_in_link(tmp_path):
    rc, out = run(tmp_path, {"nocite.cpp": "#include <cstdint>\nuint8_t g() { return 1; }\n"},
                  dirname="link")
    assert rc == 1
    assert "no spec citation" in out


def test_flags_restated_trunk_timing_literal_in_link(tmp_path):
    body = "// trunk §9\nunsigned t_resp_us() { return 200; }\n"
    rc, out = run(tmp_path, {"timing.cpp": body}, dirname="link")
    assert rc == 1
    assert "TRUNK_T_resp_us" in out


def test_repo_embedded_dirs_are_clean():
    r = subprocess.run([sys.executable, str(TOOL)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout


INCLUDE_DIRECTIVE = re.compile(r'\s*#\s*include\s+(.*)')
INCLUDE_LITERAL = re.compile(r'^([<"])([^>"]*)[>"]$')
CMAKE_L3_MENTION = re.compile(r'omgp_l3|(?<!\w)l3(?!\w)')


def _splice_lines(text: str):
    r"""Join backslash-continued physical lines before matching.

    C++ phase-2 translation (line splicing) joins a backslash-terminated line with
    the next one before the preprocessor ever sees tokens, so `#include \` followed
    by `"l3/x.hpp"` on the next line is a real, compiling include, not a curiosity —
    matching only whole physical lines misses it. Yields (logical_line, first_lineno).
    """
    physical = text.splitlines()
    out = []
    i = 0
    while i < len(physical):
        start_lineno = i + 1
        buf = physical[i]
        i += 1
        trimmed = buf.rstrip(" \t\r")
        while trimmed.endswith("\\") and i < len(physical):
            buf = trimmed[:-1] + physical[i]
            i += 1
            trimmed = buf.rstrip(" \t\r")
        out.append((buf, start_lineno))
    return out


def _include_target(logical_line: str):
    """Classify a (splice-joined) logical line.

    Returns "" if it is not a #include directive at all; the literal path string if
    it is a #include with a directly-verifiable `<...>`/`"..."` target; None if it is
    a #include whose target is a macro or other token we cannot statically resolve
    (e.g. `#include L3_VICTIM_HDR`) — the caller must fail closed on None, since a
    macro can expand to anything, including an l3/ path.
    """
    m = re.match(INCLUDE_DIRECTIVE, logical_line)
    if not m:
        return ""
    lit = INCLUDE_LITERAL.match(m.group(1).strip())
    if not lit:
        return None
    return lit.group(2)


def test_link_never_includes_l3():
    """FR-013 / CLAUDE.md 'L2 is opaque to L3': link/ must never #include anything from l3/.

    Guard against the direction check_embedded.py's literal/citation scan doesn't cover —
    this passes today (link/ holds only header-only crc16.hpp) and stays the tripwire for
    every later engine task (T007-T044) that adds .cpp files under link/. Path components
    are compared as whole segments (not substring) so a directory like `control3/` can never
    false-positive on the trailing "l3". A #include whose target isn't a plain literal
    (line-spliced or macro-indirected) is treated as a violation: it cannot be statically
    proven not to resolve into l3/, and this guard fails closed.
    """
    link_dir = ROOT / "link"
    violations = []
    for f in sorted(link_dir.rglob("*")):
        if f.suffix not in (".cpp", ".hpp", ".h", ".cc"):
            continue
        text = f.read_text(encoding="utf-8", errors="replace")
        for logical_line, lineno in _splice_lines(text):
            target = _include_target(logical_line)
            if target == "":
                continue
            if target is None:
                violations.append(
                    f"{f.relative_to(ROOT)}:{lineno}: macro-indirected #include cannot be "
                    f"statically verified not to resolve into l3/: {logical_line.strip()}"
                )
                continue
            if "l3" in target.split("/")[:-1]:
                violations.append(f"{f.relative_to(ROOT)}:{lineno}: {logical_line.strip()}")
    assert not violations, "link/ must not #include l3/ (FR-013): " + "; ".join(violations)


def test_link_cmake_never_links_l3():
    """Companion to test_link_never_includes_l3: make the link/CMakeLists.txt comment's
    'omgp_link must never target_link_libraries(... omgp_l3)' rule mechanical too.

    Neither this test file nor check_embedded.py reads CMakeLists.txt, so before this test
    that rule was prose only — target_link_libraries(omgp_link PUBLIC omgp_l3) would pass
    the whole guard suite (and, since l3/'s include dirs are PUBLIC, would also put l3/ on
    omgp_link's effective include path, reopening the bare-filename gap this test also
    forecloses). Full-line CMake comments are stripped first so the rule's own prose
    (which names omgp_l3) doesn't self-trip.
    """
    cmake = ROOT / "link" / "CMakeLists.txt"
    code_lines = [
        line for line in cmake.read_text(encoding="utf-8").splitlines()
        if not line.strip().startswith("#")
    ]
    hits = [line for line in code_lines if CMAKE_L3_MENTION.search(line)]
    assert not hits, (
        "link/CMakeLists.txt must never reference omgp_l3 or an l3/ path (FR-013 — L2 is "
        "opaque to L3): " + "; ".join(hits)
    )
