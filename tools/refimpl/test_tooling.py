"""Tests for the deep-verify tooling scripts (spec 001 T053, written first):
tools/fuzz-smoke.sh and tools/mutate.sh must fail loudly when their tool is missing (with
the disclosure line), scope correctly, and finish fast when nothing is in scope."""
from __future__ import annotations

import configparser
import os
import pathlib
import subprocess
import time

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
FUZZ = ROOT / "tools" / "fuzz-smoke.sh"
MUTATE = ROOT / "tools" / "mutate.sh"
CFG = ROOT / "tools" / "mutate.cfg"


# "Tool absent" is simulated through the scripts' own override variables (a nonexistent
# path), not by rebuilding PATH from /usr/bin — cheap, deterministic, and independent of
# what happens to be installed on the machine running the tests.
NO_CLANG = {"OMGP_FUZZ_CXX": "/nonexistent/clang++"}
NO_MULL = {"MULL_RUNNER": "/nonexistent/mull-runner", "MULL_PLUGIN": "/nonexistent/mull-ir-frontend"}


def run(script, *args, env_overrides=None, timeout=120):
    env = dict(os.environ)
    for k in ("OMGP_FUZZ_CXX", "MULL_RUNNER", "MULL_PLUGIN"):
        env.pop(k, None)
    env.update(env_overrides or {})
    t0 = time.monotonic()
    r = subprocess.run(["bash", str(script), *args], capture_output=True, text=True, cwd=ROOT, env=env,
                       timeout=timeout)
    return r.returncode, r.stdout + r.stderr, time.monotonic() - t0


def test_fuzz_smoke_without_clang_fails_with_disclosure():
    rc, out, _ = run(FUZZ, "1", env_overrides=NO_CLANG)
    assert rc == 1
    assert "blind spot" in out and "libFuzzer" in out


def test_mutate_cfg_parses_and_pins():
    cp = configparser.ConfigParser()
    cp.read(CFG)
    m = cp["mull"]
    assert m["version"] and m["clang_major"].isdigit()
    assert 0 < int(cp["policy"]["threshold_pct"]) <= 100
    assert "l3" in cp["policy"]["scope_dirs"].split()


def test_mutate_nothing_in_scope_is_fast(tmp_path):
    rc, out, dt = run(MUTATE, "--diff", "HEAD", timeout=90)
    assert rc == 0, out
    assert "nothing in scope" in out
    assert dt < 60


def _scoped_ref() -> str:
    """A commit that differs from HEAD under l3/ — the root of the clone's history. In a
    shallow CI checkout that is HEAD itself, so scope-dependent tests skip honestly."""
    root = subprocess.run(["git", "rev-list", "--max-parents=0", "HEAD"], capture_output=True, text=True,
                          cwd=ROOT).stdout.split()[-1]
    changed = subprocess.run(["git", "diff", "--name-only", root, "--", "l3"], capture_output=True, text=True,
                             cwd=ROOT).stdout
    if not changed.strip():
        pytest.skip("no l3/ history in this clone (shallow checkout) — scope tests need a differing ref")
    return root


def test_mutate_bad_ref_is_an_error_not_a_silent_pass():
    rc, out, _ = run(MUTATE, "--diff", "no-such-ref-xyz")
    assert rc == 2 and "not a commit" in out


def test_mutate_require_fails_without_mull():
    rc, out, _ = run(MUTATE, "--diff", _scoped_ref(), "--require", env_overrides=NO_MULL)
    assert rc == 1
    assert "required but not found" in out


def test_mutate_without_require_discloses_and_passes():
    rc, out, _ = run(MUTATE, "--diff", _scoped_ref(), env_overrides=NO_MULL)
    assert rc == 0, out
    assert "not present" in out and "blind spot" in out


def test_mutate_scope_lists_changed_embedded_files():
    rc, out, _ = run(MUTATE, "--diff", _scoped_ref(), "--dry-run")
    assert rc == 0, out
    assert "l3/" in out and "scope:" in out
