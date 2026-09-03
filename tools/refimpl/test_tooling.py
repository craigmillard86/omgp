"""Tests for the deep-verify tooling scripts (spec 001 T053, written first):
tools/fuzz-smoke.sh and tools/mutate.sh must fail loudly when their tool is missing (with
the disclosure line), scope correctly, and finish fast when nothing is in scope."""
from __future__ import annotations

import configparser
import json
import os
import pathlib
import re
import subprocess
import sys
import time

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
FUZZ = ROOT / "tools" / "fuzz-smoke.sh"
MUTATE = ROOT / "tools" / "mutate.sh"
CFG = ROOT / "tools" / "mutate.cfg"
DIFFCHECK = ROOT / "tools" / "diffcheck.py"
L3_HELPER = ROOT / "build" / "native" / "l3_helper"


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


def test_diffcheck_frames_only_discloses_its_blind_spot():
    # --frames-only skips the crc/message/invalid/descriptor corpora (contracts/tooling.md
    # "every fast/partial path states its blind spot"); the summary line must say so, not
    # just print the same "C++ and Python agree" sentence a full run prints.
    # Otherwise-hermetic (no other test in this file needs a native build): skip honestly
    # rather than fail when build/native/l3_helper hasn't been built yet, so
    # `python -m pytest tools/refimpl/` stays runnable standalone on a fresh checkout
    # (CLAUDE.md: "python -m pytest tools/refimpl/" is documented as a standalone command).
    # This is an existence check, not a freshness one: a binary left over from an earlier
    # or different build is used as-is, same as every other consumer of build/native/.
    if not L3_HELPER.exists():
        pytest.skip(f"{L3_HELPER} not built (run ./pipeline.sh build first, or the full pipeline)")
    # `--frames` is passed alongside `--frames-only` deliberately (review on #121): the
    # flag is a documented no-op today (frames run by default), and this pins that passing
    # it stays harmless — if the corpora ever move behind an opt-in, this invocation is the
    # first thing that must keep working.
    r = subprocess.run([sys.executable, str(DIFFCHECK), "--frames-only", "--frames"], capture_output=True,
                       text=True, cwd=ROOT, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "blind spot" in r.stdout and "descriptor" in r.stdout
    # T025 criterion pinned (review on #121): the summary line carries the frame and
    # torture counts — dropping or renaming either field must fail here, not regress
    # silently with the stage still exiting 0.
    assert "frames " in r.stdout and "torture " in r.stdout, r.stdout


def test_mutate_cfg_parses_and_pins():
    cp = configparser.ConfigParser()
    cp.read(CFG)
    m = cp["mull"]
    assert m["version"] and m["clang_major"].isdigit()
    # Ruling 2026-08-29: triage gate, no percentage. The constant is T3 — this test pins
    # the value so a relaxation is a visible, reviewable diff, never a quiet config edit.
    assert int(cp["policy"]["max_unlabelled_survivors"]) == 0
    assert "threshold_pct" not in cp["policy"]
    assert set(cp["policy"]["label_categories"].split()) == {"equivalent", "accepted"}
    assert "l3" in cp["policy"]["scope_dirs"].split()


# --- tools/mutate.sh: the oracle follows the changed directories -------------------------------
# PR #94 (2026-08-30, first link/ source): the runner list was hard-coded to the three
# test_l3_* binaries, so link/ mutants were never executed and the blind-spot rule failed
# the run. The oracle for a change under <dir> is every tests/unit binary named
# test_<dir>_* (CMakeLists.txt omgp_add_catch_test); property tests are never used.

def unit_binaries():
    text = (ROOT / "CMakeLists.txt").read_text()
    return {m.group(1) for m in re.finditer(r"omgp_add_catch_test\((test_\w+)\s+tests/unit/", text)}


def oracle_line(out):
    lines = [l for l in out.splitlines() if l.startswith("mutation: oracle:")]
    assert len(lines) == 1, out
    return lines[0].split(":", 2)[2].split()


def test_mutate_dry_run_lists_a_unit_oracle_for_every_scope_dir_with_sources():
    rc, out, _ = run(MUTATE, "--dry-run")
    assert rc == 0, out
    oracle = oracle_line(out)
    assert oracle and set(oracle) <= unit_binaries(), (oracle, out)
    scope_dirs = configparser.ConfigParser()
    scope_dirs.read(CFG)
    for d in scope_dirs["policy"]["scope_dirs"].split():
        if any((ROOT / d).rglob("*.cpp")) or any((ROOT / d).rglob("*.hpp")):
            assert any(b.startswith(f"test_{d}_") for b in oracle), (d, oracle)
    assert not any("roundtrip" in b for b in oracle), oracle  # property tests are not the oracle


def shared_clone(tmp_path, rel_path, content):
    """A --shared clone with one extra commit touching rel_path (mutate.sh cds to its own root)."""
    clone = tmp_path / "clone"
    subprocess.run(["git", "clone", "-q", "--shared", "--no-checkout", str(ROOT), str(clone)], check=True)
    subprocess.run(["git", "checkout", "-q", "HEAD"], cwd=clone, check=True)
    # The script under test is the working-tree one, not whatever HEAD has committed.
    for rel in ("tools/mutate.sh", "tools/mutate.cfg", "CMakeLists.txt"):
        (clone / rel).write_text((ROOT / rel).read_text())
    p = clone / rel_path
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(content)
    subprocess.run(["git", "add", rel_path], cwd=clone, check=True)
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "x"], cwd=clone, check=True)
    return clone


def test_mutate_diff_oracle_is_the_changed_dirs_unit_tests(tmp_path):
    clone = shared_clone(tmp_path, "link/zz_probe.cpp", "int zz_probe() { return 1; }\n")
    rc, out, _ = run(clone / "tools" / "mutate.sh", "--diff", "HEAD~1", "--dry-run")
    assert rc == 0, out
    oracle = oracle_line(out)
    assert oracle and all(b.startswith("test_link_") for b in oracle), oracle


def test_mutate_oracle_ignores_commented_out_registrations(tmp_path):
    # A `# omgp_add_catch_test(...)` line is not a binary; picking it up would fail the run
    # later as "oracle binary was not built" — a spurious failure, not a blind spot.
    clone = shared_clone(tmp_path, "link/zz_probe.cpp", "int zz_probe() { return 1; }\n")
    cm = clone / "CMakeLists.txt"
    cm.write_text(cm.read_text() + "\n# omgp_add_catch_test(test_link_phantom tests/unit/test_link_phantom.cpp)\n"
                  "   #omgp_add_catch_test(test_link_ghost tests/unit/test_link_ghost.cpp)\n")
    rc, out, _ = run(clone / "tools" / "mutate.sh", "--diff", "HEAD~1", "--dry-run")
    assert rc == 0, out
    oracle = oracle_line(out)
    assert "test_link_phantom" not in oracle and "test_link_ghost" not in oracle, oracle
    assert "test_link_interfaces" in oracle, oracle


def test_mutate_diff_with_no_unit_oracle_fails_closed(tmp_path):
    # A scope dir that has no test_<dir>_* unit binary at all can never kill a mutant; that
    # must fail before any build, with the blind spot named, not run and report 0 mutants.
    clone = shared_clone(tmp_path, "core/zz_probe.cpp", "int zz_probe() { return 1; }\n")
    rc, out, _ = run(clone / "tools" / "mutate.sh", "--diff", "HEAD~1", "--dry-run")
    assert rc == 1, out
    assert "blind spot" in out and "core" in out and "no unit-test oracle" in out, out


# --- tools/mutate_report.py: the triage gate on synthetic Elements reports --------------------

REPORT = ROOT / "tools" / "mutate_report.py"


def _mutant(line, col, mutator, status):
    return {"id": f"{mutator}:{line}:{col}", "mutatorName": mutator, "status": status,
            "location": {"start": {"line": line, "column": col}, "end": {"line": line, "column": col + 1}}}


def _setup(tmp_path, source_lines, mutants, ranges=None):
    """A fake repo root with l3/x.cpp and one Elements report naming its mutants."""
    root = tmp_path / "repo"
    (root / "l3").mkdir(parents=True, exist_ok=True)
    (root / "l3" / "x.cpp").write_text("\n".join(source_lines) + "\n")
    reports = tmp_path / "reports"
    reports.mkdir(exist_ok=True)
    (reports / "test_x.json").write_text(json.dumps(
        {"files": {str(root / "l3" / "x.cpp"): {"mutants": mutants}}}))
    (tmp_path / "ranges.json").write_text(json.dumps(ranges if ranges is not None else {}))
    return root, reports


def _report(tmp_path, root, reports, *extra):
    r = subprocess.run([sys.executable, str(REPORT), "--reports", str(reports), "--root", str(root),
                        "--scope-dirs", "l3 link core", "--ranges", str(tmp_path / "ranges.json"),
                        "--out", str(tmp_path / "report.json"), *extra],
                       capture_output=True, text=True, cwd=ROOT)
    doc = json.loads((tmp_path / "report.json").read_text()) if (tmp_path / "report.json").exists() else {}
    return r.returncode, r.stdout + r.stderr, doc


SRC = [
    "int f(int a) {",                                                      # 1
    "    if (a >= 4)",                                                     # 2  survivor, no label
    "        return 1;",                                                   # 3
    "    if (a < 0) // mutant-ok(equivalent): a is unsigned upstream",     # 4  survivor, labelled
    "        return 2;",                                                   # 5
    "    // mutant-ok(accepted, cxx_gt_to_ge): cap check; only detail bytes differ",  # 6 label above
    "    return a > 9 ? 3 : 4;",                                           # 7  two mutants: one named
    "}",
]
MUTANTS = [_mutant(2, 11, "cxx_ge_to_gt", "Survived"), _mutant(4, 11, "cxx_lt_to_le", "Survived"),
           _mutant(7, 14, "cxx_gt_to_ge", "Survived"), _mutant(7, 14, "cxx_gt_to_lt", "Killed")]


def test_report_diff_mode_fails_on_unlabelled_survivor_and_names_it(tmp_path):
    root, reports = _setup(tmp_path, SRC, MUTANTS, {"l3/x.cpp": [[1, 8]]})
    rc, out, doc = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1, out
    assert "UNLABELLED survivor: l3/x.cpp:2:11 cxx_ge_to_gt" in out
    assert doc["unlabelled"] == 1 and doc["survived"] == 3 and doc["killed"] == 1
    assert doc["labelled"] == {"equivalent": 1, "accepted": 1}
    assert "labelled (accepted): l3/x.cpp:7:14 cxx_gt_to_ge" in out   # label on the line above


def test_report_diff_mode_passes_when_every_survivor_is_labelled_or_killed(tmp_path):
    src = list(SRC)
    src[1] = "    if (a >= 4) // mutant-ok(equivalent): 4 is never reached, callers pass < 4"
    root, reports = _setup(tmp_path, src, MUTANTS, {"l3/x.cpp": [[1, 8]]})
    rc, out, doc = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 0, out
    assert doc["unlabelled"] == 0


def test_report_diff_mode_ignores_survivors_outside_changed_lines(tmp_path):
    root, reports = _setup(tmp_path, SRC, MUTANTS, {"l3/x.cpp": [[4, 5]]})
    rc, out, doc = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 0, out
    assert doc["survived"] == 1 and doc["unlabelled"] == 0


def test_report_named_mutator_label_covers_only_that_mutator(tmp_path):
    mutants = MUTANTS[:3] + [_mutant(7, 14, "cxx_gt_to_lt", "Survived")]   # now survives too
    src = list(SRC)
    src[1] = "    if (a >= 4) // mutant-ok(accepted): boundary is documented, not tested"
    root, reports = _setup(tmp_path, src, mutants, {"l3/x.cpp": [[1, 8]]})
    rc, out, doc = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1, out
    assert "UNLABELLED survivor: l3/x.cpp:7:14 cxx_gt_to_lt" in out


def test_report_trend_mode_never_gates_on_survivors(tmp_path):
    root, reports = _setup(tmp_path, SRC, MUTANTS)
    log = tmp_path / "trend.jsonl"
    rc, out, doc = _report(tmp_path, root, reports, "--trend-log", str(log))
    assert rc == 0, out
    assert doc["mode"] == "trend" and doc["unlabelled"] == 1
    line = json.loads(log.read_text().splitlines()[-1])
    assert line["survived"] == 3 and line["kill_rate"] == 25.0
    assert "mutation-trend:" in out


def test_report_malformed_label_fails_in_every_mode(tmp_path):
    src = list(SRC)
    src[3] = "    if (a < 0) // mutant-ok(whatever): not a category"
    root, reports = _setup(tmp_path, src, MUTANTS)
    rc, out, _ = _report(tmp_path, root, reports)
    assert rc == 1 and "unknown label category 'whatever'" in out
    src[3] = "    if (a < 0) // mutant-ok(equivalent):"
    root, reports = _setup(tmp_path, src, MUTANTS)
    rc, out, _ = _report(tmp_path, root, reports)
    assert rc == 1 and "malformed label" in out


def test_report_stale_label_is_a_warning_not_a_survivor(tmp_path):
    mutants = [_mutant(4, 11, "cxx_lt_to_le", "Killed")]   # the labelled line is now killed
    root, reports = _setup(tmp_path, SRC, mutants, {"l3/x.cpp": [[4, 4]]})
    rc, out, doc = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 0, out
    assert "stale label: l3/x.cpp:4" in out and doc["survived"] == 0


def test_report_no_reports_or_no_mutants_is_a_failure(tmp_path):
    root, reports = _setup(tmp_path, SRC, [])
    rc, out, _ = _report(tmp_path, root, reports)
    assert rc == 1 and "no mutants" in out
    for f in reports.glob("*.json"):
        f.unlink()
    rc, out, _ = _report(tmp_path, root, reports)
    assert rc == 1 and "no Mull reports" in out


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
