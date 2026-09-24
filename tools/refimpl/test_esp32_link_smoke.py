"""Shape guard for the ESP32-S3 link-time smoke program (spec 002 T047, issue #65).

Why this exists, and what it is worth. `./pipeline.sh esp32` going green does NOT prove the
trunk L2 engines survive the Xtensa link: `omgp_link` is a static library, and an archive
member that no referenced symbol pulls in is silently dropped. Measured against the T003
stub on 2026-09-24, `esp32-host/build/omgp-host.map` contained exactly two mentions of
`omgp_link` — both `LOAD esp-idf/omgp_link/libomgp_link.a` — and no member of it, and no
`link_smoke.cpp.obj` either, because nothing in `app_main` referenced `omgp_link_smoke`.
The build was green throughout.

So the link-time check has two halves, and this file pins both as SOURCE SHAPE:

  1. `link_smoke.cpp` must reference one entry point from each of the four engine
     translation units (`frame.cpp`, `master.cpp`, `responder.cpp`, `health.cpp`), which is
     what pulls those members out of `libomgp_link.a`;
  2. `app_main` must call `omgp_link_smoke`, which is what pulls `link_smoke.cpp.obj` out of
     `libmain.a` in the first place. Without it (1) is unreachable and the whole file is
     dead weight — the exact state the T003 stub was in.

These are static assertions about the sources, NOT proof that the link succeeded: that is
the `esp32` stage's job, and only the IDF toolchain can run it. What they establish is that
the sources cannot silently regress to a shape in which the `esp32` stage has nothing to
check — which no other test in the tree covers, because nothing else reads `esp32-host/`.

Comments and string literals are stripped before matching (via check_embedded's own
stripper), so a symbol named only in a comment does not satisfy a reference.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_embedded  # noqa: E402  (needs the path insert above)

MAIN_DIR = ROOT / "esp32-host" / "main"
SMOKE = MAIN_DIR / "link_smoke.cpp"
MAIN_C = MAIN_DIR / "main.c"
TOOL = ROOT / "tools" / "check_embedded.py"
YAML = ROOT / "protocol" / "omgp-protocol.yaml"


def code(path: pathlib.Path) -> str:
    """File contents with comments and string literals blanked out."""
    return "\n".join(check_embedded.strip_comments_and_strings(path.read_text().splitlines()))


@pytest.fixture(scope="module")
def smoke() -> str:
    return code(SMOKE)


# One entry point per engine translation unit in link/ — the members the Xtensa link must
# pull out of libomgp_link.a. trunk §4 (frame codec), §3 (both engines), §6/§7 (health).
ENGINE_ENTRY_POINTS = {
    "frame.cpp": ("encode_frame", "Deframer"),
    "master.cpp": ("Master",),
    "responder.cpp": ("Responder", "RequestHandler"),
    "health.cpp": ("HealthTracker", "HealthListener"),
}


@pytest.mark.parametrize("unit,symbols", sorted(ENGINE_ENTRY_POINTS.items()))
def test_smoke_references_each_engine_translation_unit(smoke, unit, symbols):
    missing = [s for s in symbols if s not in smoke]
    assert not missing, f"link_smoke.cpp references nothing from link/{unit}: {missing}"


# The trivial in-memory Clock/ByteWire the engines are driven against
# (contracts/byte-wire-and-clock.md). No UART, no device: spec.md Assumptions.
@pytest.mark.parametrize("member", ["now_us", "transmit", "receive", "bit_rate", "set_bit_rate"])
def test_smoke_implements_the_wire_and_clock_interfaces(smoke, member):
    assert f"{member}(" in smoke, f"link_smoke.cpp implements no {member}() override"


@pytest.mark.parametrize("call", ["begin(", "poll(", "next_probe("])
def test_smoke_drives_the_engines_not_just_constructs_them(smoke, call):
    # Constructing an engine references only its constructor; a member call is what forces
    # the rest of the translation unit's code to be needed at link time.
    assert call in smoke, f"link_smoke.cpp never calls {call}"


def test_app_main_calls_the_link_smoke_entry_point():
    # The load-bearing half: libmain.a is an ordinary archive too, so link_smoke.cpp.obj is
    # dropped unless app_main names this symbol (measured against the T003 stub — see the
    # module docstring). l3_smoke.cpp is linked for exactly this reason and no other.
    main_c = code(MAIN_C)
    assert "omgp_link_smoke" in main_c, (
        "app_main does not call omgp_link_smoke(): link_smoke.cpp.obj is then dropped from "
        "libmain.a and the esp32 stage links none of the trunk L2 engines")
    assert main_c.count("omgp_link_smoke") >= 2, (
        "expected both a declaration and a call of omgp_link_smoke() in main.c")


def test_smoke_sources_are_embedded_path_clean():
    # CLAUDE.md rule 5: link_smoke.cpp sits on the -fno-exceptions -fno-rtti path. The
    # `quality` stage does not scan esp32-host/ (pipeline.sh stage_quality), so this runs
    # the same checker over it here: no heap, no exceptions, no RTTI, no wall clock, and no
    # integer literal duplicating a protocol constant.
    r = subprocess.run(
        [sys.executable, str(TOOL), "--dirs", str(MAIN_DIR), "--yaml", str(YAML),
         "--cite-dirs", "main"],
        capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
