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


INCLUDE_LINE = re.compile(r'#\s*include\s*[<"]([^">]+)[">]')


def test_link_never_includes_l3():
    """FR-013 / CLAUDE.md 'L2 is opaque to L3': link/ must never #include anything from l3/.

    Guard against the direction check_embedded.py's literal/citation scan doesn't cover —
    this passes today (link/ holds only header-only crc16.hpp) and stays the tripwire for
    every later engine task (T007-T044) that adds .cpp files under link/.
    """
    link_dir = ROOT / "link"
    violations = []
    for f in sorted(link_dir.rglob("*")):
        if f.suffix not in (".cpp", ".hpp", ".h", ".cc"):
            continue
        for n, line in enumerate(
            f.read_text(encoding="utf-8", errors="replace").splitlines(), start=1
        ):
            m = INCLUDE_LINE.search(line)
            if m and "l3/" in m.group(1):
                violations.append(f"{f.relative_to(ROOT)}:{n}: {line.strip()}")
    assert not violations, "link/ must not #include l3/ (FR-013): " + "; ".join(violations)
