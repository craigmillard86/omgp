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


# --- tools/fuzz-smoke.sh: the run-and-report half, with the five targets shimmed --------------
# The real script needs clang+libFuzzer and a cmake build; what these cases exercise is the
# part after the build — the wave scheduler, per-target exit-status attribution, the
# artefact and log grep, and the summary — using shell scripts in build/fuzz in place of the
# fuzzers and no-op cmake/clang++ shims on PATH (#141 review @d1fc20b, MEDIUM: the failure
# and parallel paths had no test, and the `grep || true` fix landed without one).
FUZZ_TARGETS = ("fuzz_header", "fuzz_payload", "fuzz_descriptor", "fuzz_roundtrip", "fuzz_frame")
_FUZZ_SHIM = r"""#!/usr/bin/env bash
# Stand-in for a libFuzzer target: prints the two stats lines the harness greps, forms a
# barrier with its siblings when asked (proves they ran at the same time), drops an artefact
# and sanitizer lines when asked, and exits with the status the spec dir names for it.
me=$(basename "$0"); spec=$FUZZ_SHIM_DIR
art=; for a in "$@"; do case "$a" in -artifact_prefix=*) art=${a#*=};; esac; done
: > "$spec/started.$me"
if [ -s "$spec/barrier" ]; then
  n=$(cat "$spec/barrier"); ok=0
  for _ in $(seq 50); do [ "$(ls "$spec"/started.* 2>/dev/null | wc -l)" -ge "$n" ] && { ok=1; break; }; sleep 0.1; done
  [ "$ok" = 1 ] || { echo "shim: a barrier of $n never formed — the targets did not run together"; exit 99; }
fi
echo "#1234 DONE cov: 42 ft: 7"
echo "stat::number_of_executed_units: 1234"
if [ -f "$spec/$me.crash" ]; then
  echo "==1==ERROR: AddressSanitizer: heap-buffer-overflow"; echo "SUMMARY: AddressSanitizer: heap-buffer-overflow"
  : > "${art}crash-deadbeef"
fi
exit "$(cat "$spec/$me.exit" 2>/dev/null || echo 0)"
"""


def _fuzz_rig(tmp_path):
    """A clone with the working-tree fuzz-smoke.sh, shim fuzzers in build/fuzz, no-op cmake and
    clang++ shims, and an empty spec dir. Returns (runner, spec)."""
    clone = tmp_path / "clone"
    subprocess.run(["git", "clone", "-q", "--shared", "--no-checkout", str(ROOT), str(clone)], check=True)
    subprocess.run(["git", "checkout", "-q", "HEAD"], cwd=clone, check=True)
    (clone / "tools" / "fuzz-smoke.sh").write_text(FUZZ.read_text())
    shim = tmp_path / "shim"
    shim.mkdir()
    for tool in ("cmake", "clang++"):
        (shim / tool).write_text("#!/usr/bin/env bash\nexit 0\n")
        (shim / tool).chmod(0o755)
    build = clone / "build" / "fuzz"
    build.mkdir(parents=True)
    for t in FUZZ_TARGETS:
        (build / t).write_text(_FUZZ_SHIM)
        (build / t).chmod(0o755)
    spec = tmp_path / "spec"
    spec.mkdir()

    def runner(jobs=None, budget="5"):
        env = {"OMGP_FUZZ_CXX": str(shim / "clang++"), "PATH": f"{shim}:{os.environ['PATH']}",
               "FUZZ_SHIM_DIR": str(spec)}
        if jobs is not None:
            env["OMGP_FUZZ_JOBS"] = jobs
        for f in spec.glob("started.*"):
            f.unlink()
        rc, out, _ = run(clone / "tools" / "fuzz-smoke.sh", budget, env_overrides=env, timeout=120)
        lines = {t: next((l for l in out.splitlines() if l.startswith(f"fuzz: {t} ")), None) for t in FUZZ_TARGETS}
        return rc, out, lines

    return runner, spec


def test_fuzz_smoke_serial_clean_run_reports_every_target(tmp_path):
    runner, _spec = _fuzz_rig(tmp_path)
    rc, out, lines = runner()
    assert rc == 0, out
    assert "fuzz: 1 target(s) at a time" in out and "clean rejections only across 5 targets" in out, out
    for t in FUZZ_TARGETS:
        assert lines[t] == f"fuzz: {t} runs=1234 cov=42 findings=0 exit=0", (t, out)


def test_fuzz_smoke_target_dying_without_a_sanitizer_line_is_reported_and_the_rest_still_run(tmp_path):
    """The bug the gate-budget PR found with an `exit 77` shim: no ERROR/SUMMARY line in the log,
    so the report grep matched nothing and `set -eo pipefail` aborted the script at that target
    — later targets unreported, no FINDINGS line. Every target must be reported, the dead one
    with its own status, and the run must fail."""
    runner, spec = _fuzz_rig(tmp_path)
    (spec / "fuzz_descriptor.exit").write_text("77\n")
    rc, out, lines = runner()
    assert rc == 1, out
    assert lines["fuzz_descriptor"] == "fuzz: fuzz_descriptor runs=1234 cov=42 findings=0 exit=77", out
    for t in ("fuzz_roundtrip", "fuzz_frame"):     # the targets AFTER the dead one
        assert lines[t] == f"fuzz: {t} runs=1234 cov=42 findings=0 exit=0", (t, out)
    assert "fuzz: FINDINGS" in out and "reproduce:" not in out, out


def test_fuzz_smoke_waves_attribute_each_status_and_artefact_to_its_own_target(tmp_path):
    """OMGP_FUZZ_JOBS=2 runs waves of (header, payload) (descriptor, roundtrip) (frame): a
    non-zero exit in the middle of a wave, a crash with an artefact, and a failure in the
    final one-target wave must each land on the right line — `pids[k]`/`wave[k]` alignment."""
    runner, spec = _fuzz_rig(tmp_path)
    (spec / "fuzz_payload.exit").write_text("3\n")
    (spec / "fuzz_roundtrip.crash").write_text("")
    (spec / "fuzz_roundtrip.exit").write_text("1\n")
    (spec / "fuzz_frame.exit").write_text("5\n")
    rc, out, lines = runner(jobs="2")
    assert rc == 1, out
    assert "fuzz: 2 target(s) at a time" in out, out
    assert lines["fuzz_header"].endswith("findings=0 exit=0"), out
    assert lines["fuzz_payload"].endswith("findings=0 exit=3"), out
    assert lines["fuzz_descriptor"].endswith("findings=0 exit=0"), out
    assert lines["fuzz_roundtrip"].endswith("findings=1 exit=1"), out
    assert lines["fuzz_frame"].endswith("findings=0 exit=5"), out
    assert "SUMMARY: AddressSanitizer: heap-buffer-overflow" in out, out
    assert "reproduce: build/fuzz/fuzz_roundtrip build/fuzz/artifacts/fuzz_roundtrip/crash-deadbeef" in out, out
    assert out.count("reproduce:") == 1, out


def test_fuzz_smoke_jobs_run_together_and_the_knob_is_clamped_and_validated(tmp_path):
    """OMGP_FUZZ_JOBS=5: all five form a barrier (each waits until five have started, else exits
    99), so a scheduler that ran them one at a time would report exit=99. 99 clamps to 5;
    a non-integer is refused legibly and runs serially, not `unbound variable` (#141 review
    @d1fc20b, LOW)."""
    runner, spec = _fuzz_rig(tmp_path)
    (spec / "barrier").write_text("5\n")
    rc, out, lines = runner(jobs="5")
    assert rc == 0 and "fuzz: 5 target(s) at a time" in out, out
    assert all(lines[t].endswith("exit=0") for t in FUZZ_TARGETS), out
    rc, out, _ = runner(jobs="99")
    assert rc == 0 and "fuzz: 5 target(s) at a time" in out, out
    (spec / "barrier").unlink()
    for bad in ("abc", "0", "-2", ""):
        rc, out, lines = runner(jobs=bad)
        assert rc == 0 and "fuzz: 1 target(s) at a time" in out, (bad, out)
        assert "unbound variable" not in out and "integer expression expected" not in out, (bad, out)
        if bad not in ("",):
            assert f"OMGP_FUZZ_JOBS='{bad}' is not a positive integer" in out, (bad, out)


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
                       text=True, cwd=ROOT, timeout=60)  # 60 pins SC-002 (torture < 60 s; review round 7)
    assert r.returncode == 0, r.stdout + r.stderr
    # The DISCLOSURE text itself, not the counts field (review round 3 on #121: plain
    # "descriptor" also matches the always-printed "descriptors <n>" count, so that
    # conjunct was true on every exit-0 run — evidence identical either way is no evidence).
    assert "blind spot: crc/message/invalid/descriptor" in r.stdout, r.stdout
    # round 14 (red-team LOW): the disclosure TEXT alone can become a lie — also assert the
    # counts the flag claims to zero really are zero.
    assert "crc 0, messages 0, invalid 0, descriptors 0" in r.stdout, r.stdout
    # T025 criterion pinned (review on #121, rounds 5-6): the summary line carries a frame
    # count AT OR ABOVE the contract threshold ("≥ 10 000" everywhere else — an equality pin
    # would go red the day FRAME_COUNT is raised) and a nonzero torture count.
    _frames = re.search(r"frames (\d+)", r.stdout)
    assert _frames and int(_frames.group(1)) >= 10_000, r.stdout
    assert re.search(r"torture [1-9]\d*", r.stdout), r.stdout
    assert re.search(r"streams [1-9]\d*", r.stdout), r.stdout


def test_diffcheck_default_path_stays_inside_the_sc003_budget():
    if not L3_HELPER.exists():
        pytest.skip(f"{L3_HELPER} not built (run ./pipeline.sh build first, or the full pipeline)")
    # review round 13 on #121: SC-002 (--frames-only < 60 s) got a timeout pin; SC-003
    # (whole diffcheck stage < 2 min) had none, while this PR quadrupled the corpus and
    # the ceiling was held only by a PR-body figure nothing re-checks.
    r = subprocess.run([sys.executable, str(DIFFCHECK)], capture_output=True, text=True,
                       cwd=ROOT, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr
    assert re.search(r"streams [1-9]\d*", r.stdout), r.stdout
    # round 14 (red-team MED): this is the ONE test on the path pipeline.sh runs, so the
    # frame + torture corpora running BY DEFAULT is pinned here — moving them behind an
    # opt-in previously erased 28000 of 43787 CI cases with the whole suite green.
    _frames = re.search(r"frames (\d+)", r.stdout)
    assert _frames and int(_frames.group(1)) >= 10_000, r.stdout
    assert re.search(r"torture [1-9]\d*", r.stdout), r.stdout
    # round 14 (red-team LOW): the printed total must equal the sum of its printed parts.
    m = re.search(r"diffcheck: (\d+) cases.*\(crc (\d+), messages (\d+), invalid (\d+), "
                  r"descriptors (\d+), frames (\d+), torture (\d+), streams (\d+)\)", r.stdout)
    assert m and int(m.group(1)) == sum(int(x) for x in m.groups()[1:]), r.stdout


def test_replay_flag_guards_reject_bad_combinations_and_ranges():
    # round 14 on #121: the round-5 fix (replay flags mutually exclusive) and the round-13
    # fix (--index range guard) were both unpinned — deleting either left the suite green
    # while --frame-index was silently ignored / --index -1 silently replayed case N-1.
    r = subprocess.run([sys.executable, str(DIFFCHECK), "--index", "3", "--frame-index", "4"],
                       capture_output=True, text=True, cwd=ROOT, timeout=60)
    assert r.returncode != 0 and "not allowed with" in r.stderr, r.stdout + r.stderr
    if not L3_HELPER.exists():
        pytest.skip(f"{L3_HELPER} not built (range guard runs after the helper spawns)")
    r = subprocess.run([sys.executable, str(DIFFCHECK), "--count", "50", "--index", "-1"],
                       capture_output=True, text=True, cwd=ROOT, timeout=60)
    assert r.returncode != 0, r.stdout + r.stderr
    assert "--index must be 0..49" in (r.stdout + r.stderr), r.stdout + r.stderr


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


# --- tools/mutate.sh: a test-only change is ATTESTED, never gated (#146) ------------------------
# Until now `--diff <ref>` scoped by changed SOURCES alone, so a PR whose whole purpose is
# killing surviving mutants took the `nothing in scope` fast path and got no machine
# attestation at all (observed on #142, where four kills rested only on the author's local
# runs). Ruling docs/OPEN-QUESTIONS.md 2026-09-14, Option A: a diff that changes
# tests/unit/test_<dir>_* and no source under <dir> derives a TEST scope, runs that dir's
# mutants in TREND mode and reports. No FINDING of that run gates — survivors, labels and the
# kill rate all sit on lines the diff did not change, and gating on one is the Option B that
# ruling rejected — while a run that cannot happen (no oracle, no source, no report, no
# mutant) still fails closed, as everywhere else in this harness. What stays exactly as it
# was: a diff that does change a source in scope.

def attest_line(out):
    lines = [l for l in out.splitlines() if l.startswith("mutation: mode=attest")]
    assert len(lines) == 1, out
    return lines[0]


def scope_line(out):
    lines = [l for l in out.splitlines() if l.startswith("mutation: scope:")]
    assert len(lines) == 1, out
    return lines[0].split(":", 2)[2].split()


def _commit(clone, rel, content):
    p = clone / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(content)
    subprocess.run(["git", "add", rel], cwd=clone, check=True)
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "x"],
                   cwd=clone, check=True)


TEST_EDIT = "// rewritten by test_tooling.py: a test-only change under tests/unit\n"


def test_mutate_test_only_change_is_attested_not_skipped(tmp_path):
    """A diff that touches only tests/unit/test_link_*.cpp must NOT take the fast path: it
    names one greppable attest line (mode, ref, dirs, the changed test files) and derives the
    same oracle the source path would. The source scope it derives is the whole attested
    directory — trend mode has no changed-line ranges to narrow it to."""
    clone = shared_clone(tmp_path, "tests/unit/test_link_master.cpp", TEST_EDIT)
    rc, out, dt = run(clone / "tools" / "mutate.sh", "--diff", "HEAD~1", "--dry-run")
    assert rc == 0, out
    assert "nothing in scope" not in out, out
    line = attest_line(out)
    assert "ref=HEAD~1" in line and "dirs=link" in line, line
    assert "tests=tests/unit/test_link_master.cpp" in line, line
    scope = scope_line(out)
    assert scope and all(f.startswith("link/") for f in scope), scope
    assert "link/master.cpp" in scope, scope
    oracle = oracle_line(out)
    assert oracle and all(b.startswith("test_link_") for b in oracle), oracle
    assert set(oracle) <= unit_binaries(), oracle
    assert dt < 60, dt


def test_mutate_unrelated_test_change_is_still_nothing_in_scope(tmp_path):
    """The test scope is `tests/unit/test_<dir>_*` for <dir> in scope_dirs, nothing wider:
    tests/unit/test_mock_wire.cpp names no scope dir, so the diff is still empty and the fast
    path (and its exact message) is unchanged."""
    clone = shared_clone(tmp_path, "tests/unit/test_mock_wire.cpp", TEST_EDIT)
    rc, out, dt = run(clone / "tools" / "mutate.sh", "--diff", "HEAD~1", timeout=90)
    assert rc == 0, out
    assert "mutation: nothing in scope (HEAD~1) — no changed sources under: l3 link core" in out, out
    assert "mode=attest" not in out, out
    assert dt < 60, dt


def test_mutate_test_only_attestation_never_gates_on_survivors(tmp_path):
    """Two halves, labelled. (a) The shell states the mode it will run the reporter in, and
    passes it the empty ref that selects trend mode — proved for the printed claim by this
    run, and for the reporter call by reading the single call site (mutate.sh is not run to
    completion here: that needs Mull). (b) The reporter in that mode does not gate on the
    rule this case is about: the very input that exits 1 with --ref exits 0 without it, while
    still LISTING the unlabelled survivor and recording where the attestation came from.

    Survivors are ONE of mutate_report.py's exit-1 rules; the others are in
    test_mutate_attestation_gates_on_a_broken_run_never_on_its_findings, because generalising
    "does not gate" from this case alone is what made the #593 body's claim false."""
    clone = shared_clone(tmp_path, "tests/unit/test_link_master.cpp", TEST_EDIT)
    rc, out, _ = run(clone / "tools" / "mutate.sh", "--diff", "HEAD~1", "--dry-run")
    assert rc == 0, out
    line = attest_line(out)
    assert "report=trend" in line and "gate=blind-spot-only" in line, line
    sh = MUTATE.read_text()
    assert len(re.findall(r"^python3 tools/mutate_report\.py", sh, re.M)) == 1, "more than one reporter call site"
    assert '--ref "$REPORT_REF"' in sh, "the reporter must be called with the mode-selecting ref variable"
    assert re.search(r'^\s*ATTEST=1; REPORT_REF=""', sh, re.M), "attest mode must clear the reporter's ref"

    root, reports = _setup(tmp_path, SRC, MUTANTS, {"l3/x.cpp": [[1, 8]]})
    rc_gate, out_gate, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc_gate == 1, out_gate            # the discriminating half: this input DOES gate in diff mode
    # The attested dir is the one this report's mutants were executed in: attesting any other
    # is the blind spot test_mutate_attestation_fails_closed_when_an_attested_dir_ran_no_mutant
    # pins, not a survivor question.
    attest = ("--attest-ref", "origin/main", "--attest-tests", "tests/unit/test_l3_payload.cpp",
              "--attest-dirs", "l3")
    rc, out, doc = _report(tmp_path, root, reports, *attest)
    assert rc == 0, out
    assert doc["mode"] == "trend", doc["mode"]
    assert doc["unlabelled"] == 1 and any(s["label"] is None for s in doc["survivors"]), doc
    assert "UNLABELLED survivor: l3/x.cpp:2:11 cxx_ge_to_gt" in out, out
    # Provenance lives in the file, not only in stdout (data-model §7 keys are all still there).
    assert doc["attest"] == {"origin": "changed-tests", "diff_ref": "origin/main",
                             "tests": ["tests/unit/test_l3_payload.cpp"], "dirs": ["l3"]}, doc
    assert {"mode", "diff_ref", "mutants_total", "killed", "survived", "not_covered", "kill_rate",
            "labelled", "unlabelled", "max_unlabelled", "survivors", "stale_labels",
            "malformed_labels"} <= set(doc), sorted(doc)
    # And the two are mutually exclusive by construction: an attestation is never a diff-mode run.
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main", *attest)
    assert rc == 2 and "--attest-" in out, out


def test_mutate_attestation_is_never_appended_to_the_whole_tree_trend_log(tmp_path):
    """An attestation mutates one or two dirs; the trend log the nightly whole-tree run
    appends to (build/mutate/trend.jsonl, .github/workflows/nightly.yml) records whole-tree
    scores. Appending one to the other would silently put a partial score in the series, so
    --trend-log is disclosed and dropped on that path — and the decision is taken early enough
    that --dry-run shows it (this run needs no Mull)."""
    clone = shared_clone(tmp_path, "tests/unit/test_link_master.cpp", TEST_EDIT)
    log = tmp_path / "trend.jsonl"
    rc, out, _ = run(clone / "tools" / "mutate.sh", "--diff", "HEAD~1", "--trend-log", str(log), "--dry-run")
    assert rc == 0, out
    assert "--trend-log not appended for a test-only attestation" in out, out
    assert not log.exists(), log
    # The whole-tree run it belongs to still takes it.
    rc, out, _ = run(clone / "tools" / "mutate.sh", "--trend-log", str(log), "--dry-run")
    assert rc == 0 and "not appended" not in out, out


def test_mutate_source_change_scoping_is_unchanged(tmp_path):
    """Changing a test must never widen what can fail a PR: with a source in scope the run is
    byte-identical whether or not the same diff also touches tests/unit."""
    (tmp_path / "a").mkdir()
    (tmp_path / "b").mkdir()
    src_only = shared_clone(tmp_path / "a", "link/zz_probe.cpp", "int zz_probe() { return 1; }\n")
    rc_a, out_a, _ = run(src_only / "tools" / "mutate.sh", "--diff", "HEAD~1", "--dry-run")
    both = shared_clone(tmp_path / "b", "link/zz_probe.cpp", "int zz_probe() { return 1; }\n")
    _commit(both, "tests/unit/test_link_master.cpp", TEST_EDIT)
    rc_b, out_b, _ = run(both / "tools" / "mutate.sh", "--diff", "HEAD~2", "--dry-run")
    assert rc_a == 0 and rc_b == 0, out_a + out_b
    assert "mode=attest" not in out_b and "mode=attest" not in out_a, out_b
    assert scope_line(out_a) == scope_line(out_b) == ["link/zz_probe.cpp"], (out_a, out_b)
    assert oracle_line(out_a) == oracle_line(out_b), (out_a, out_b)
    assert out_a.replace("HEAD~1", "<ref>") == out_b.replace("HEAD~2", "<ref>"), (out_a, out_b)


def test_mutate_test_only_scope_without_oracle_fails_closed(tmp_path):
    """The attested dir's oracle is the same test_<dir>_* set the source path computes, and
    fails closed the same way: a test file naming a dir with no registered unit binary can
    never kill a mutant, so the run fails before any build, with the blind spot named — it
    does not fall back to the fast path and call that an attestation."""
    clone = shared_clone(tmp_path, "tests/unit/test_core_health.cpp",
                         "// a unit test for a dir with no registered unit binary\n")
    rc, out, _ = run(clone / "tools" / "mutate.sh", "--diff", "HEAD~1", "--dry-run")
    assert rc == 1, out
    assert "no unit-test oracle for changed dir 'core/'" in out and "blind spot" in out, out
    assert "nothing in scope" not in out, out


def test_mutate_test_scope_requires_a_source_extension(tmp_path):
    """The test scope is `tests/unit/test_<dir>_*.<ext>` for <ext> in source_ext — the one
    list. Without the extension anchor any `tests/unit/test_link_*` path buys a full link/
    mutation run: a note, a fixture, a JSON blob. Red-team round 1 on #593 showed the AC's
    cited evidence (test_source_extensions_have_one_source_of_truth) is as green with the
    anchor deleted as with it, so this is the case that discriminates — it fails if the
    `$EXT_RE` anchor is dropped from the test-scope grep (mutate.sh) and passes with it."""
    for rel in ("tests/unit/test_link_notes.md", "tests/unit/test_link_master.cpp.bak"):
        sub = tmp_path / rel.rsplit("/", 1)[1]
        sub.mkdir()
        clone = shared_clone(sub, rel, "not a source file\n")
        rc, out, _ = run(clone / "tools" / "mutate.sh", "--diff", "HEAD~1", timeout=90)
        assert rc == 0, (rel, out)
        assert "mode=attest" not in out, (rel, out)
        assert "mutation: nothing in scope (HEAD~1) — no changed sources under: l3 link core" in out, (rel, out)


def test_mutate_attested_dir_with_no_source_fails_closed(tmp_path):
    """The other blind spot on the attest path (mutate.sh, after the oracle rule): the changed
    tests name a dir that holds no source file, so no mutant can exist there and the tests
    attest nothing. That must fail closed, not report a green attestation over an empty scope.
    Unreachable in today's tree — every scope dir that owns a test_<dir>_* binary also owns
    sources — so the case is built: link/'s sources go in one commit (the directory itself
    stays, or `find` would fail the script before this rule), the test edit in the next, and
    the run is scoped to the second commit alone."""
    clone = shared_clone(tmp_path, "docs/zz_probe.md", "a file outside every scope dir\n")
    sources = [str(p.relative_to(clone)) for p in (clone / "link").iterdir() if p.is_file()]
    subprocess.run(["git", "rm", "-q", "--", *sources], cwd=clone, check=True)
    (clone / "link").mkdir(exist_ok=True)   # git drops the now-empty directory
    (clone / "link" / "notes.md").write_text("the dir survives its sources\n")
    subprocess.run(["git", "add", "link/notes.md"], cwd=clone, check=True)
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "x"],
                   cwd=clone, check=True)
    _commit(clone, "tests/unit/test_link_master.cpp", TEST_EDIT)
    rc, out, _ = run(clone / "tools" / "mutate.sh", "--diff", "HEAD~1", "--dry-run")
    assert rc == 1, out
    assert "no source file under the attested dir(s) 'link'" in out and "blind spot" in out, out
    assert "nothing in scope" not in out, out
    # The oracle rule ran first and was satisfied: this is the source blind spot, not that one.
    assert "no unit-test oracle" not in out, out
    assert oracle_line(out), out


def _strip_sources(clone, d):
    """Leave `d/` in the tree with no source file in it (the directory itself survives, or
    `find` would fail the script before the rule under test)."""
    sources = [str(p.relative_to(clone)) for p in (clone / d).iterdir() if p.is_file()]
    subprocess.run(["git", "rm", "-q", "--", *sources], cwd=clone, check=True)
    (clone / d).mkdir(exist_ok=True)
    (clone / d / "notes.md").write_text("the dir survives its sources\n")
    subprocess.run(["git", "add", f"{d}/notes.md"], cwd=clone, check=True)
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "x"],
                   cwd=clone, check=True)


def test_mutate_attested_dir_with_no_source_fails_closed_inside_a_union(tmp_path):
    """The same blind spot, per attested DIR rather than over their union (#593 red-team
    round 4). A union attestation is the ordinary shape #146 exists to serve
    (test_mutate_attestation_scope_is_the_union_of_the_changed_tests_dirs), and a whole-scope
    test of the derived source scope passes one through whenever ANY attested dir has
    sources: `find l3 link` is non-empty on l3/ alone, so `link` is named in `dirs=`, puts
    seven test_link_* binaries in the oracle, and attests a directory in which no mutant can
    exist. The control is the single-dir case above, which fails closed — if the exit were
    independent of link/ being sourceless, neither would be evidence."""
    clone = shared_clone(tmp_path, "docs/zz_probe.md", "a file outside every scope dir\n")
    _strip_sources(clone, "link")
    _commit(clone, "tests/unit/test_link_master.cpp", TEST_EDIT)
    _commit(clone, "tests/unit/test_l3_payload.cpp", TEST_EDIT)
    rc, out, _ = run(clone / "tools" / "mutate.sh", "--diff", "HEAD~2", "--dry-run")
    assert "dirs=l3 link" in attest_line(out), attest_line(out)   # the union really is derived
    assert rc == 1, out
    assert "no source file under the attested dir(s) 'link'" in out and "blind spot" in out, out
    # It names the sourceless dir and only it: l3/ does carry sources and attests fine.
    assert "'l3 link'" not in out and "'l3'" not in out, out
    assert "nothing in scope" not in out, out
    assert "no unit-test oracle" not in out, out


def test_mutate_attestation_scope_is_the_union_of_the_changed_tests_dirs(tmp_path):
    """Nothing bounds an attestation to one directory: a diff touching both dirs' unit tests
    attests both. This is the reachable MAXIMUM, and the number `deep-verify`'s 45-minute
    budget has to hold — the #146 estimate reasoned about link/ alone (7 of 17 binaries),
    which red-team round 1 on #593 showed is not the worst case. The wall clock itself stays
    unmeasured here (Mull is absent; see docs/OPEN-QUESTIONS.md 2026-09-15): what this case
    pins is the SIZE the measurement has to be taken at, not the time."""
    clone = shared_clone(tmp_path, "tests/unit/test_link_master.cpp", TEST_EDIT)
    _commit(clone, "tests/unit/test_l3_payload.cpp", TEST_EDIT)
    rc, out, _ = run(clone / "tools" / "mutate.sh", "--diff", "HEAD~2", "--dry-run")
    assert rc == 0, out
    assert "dirs=l3 link" in attest_line(out), attest_line(out)
    scope = scope_line(out)
    assert {f.split("/")[0] for f in scope} == {"l3", "link"}, scope
    oracle = oracle_line(out)
    assert set(oracle) <= unit_binaries(), oracle
    assert {b.split("_")[1] for b in oracle} == {"l3", "link"}, oracle
    # Against the whole tree it contrasts itself with: the majority of the binaries, not 7/17.
    assert len(oracle) > len(unit_binaries()) / 2, (len(oracle), len(unit_binaries()))


def test_mutate_attestation_gates_on_a_broken_run_never_on_its_findings(tmp_path):
    """What "never gates" means, rule by rule, against every `return 1` in mutate_report.py
    that an attestation can reach — the previous case generalised from the unlabelled-survivor
    rule alone and the PR body's "every gate rule is guarded on diff_mode" was false of the
    other three (red-team round 1 on #593).

    FINDINGS never gate: an attestation changes no source line, so every survivor and every
    `mutant-ok` label it can read is pre-existing debt outside its diff. Gating on one is
    Option B, which docs/OPEN-QUESTIONS.md 2026-09-14 rejected precisely because it fails PRs
    on debt they did not touch.

    A BROKEN RUN still fails closed: no reports and no mutants say the attestation did not
    happen, so there is nothing to report and exit 0 would be a false green — the same
    blind-spot rule the shell already applies for a missing oracle or an empty source scope."""
    # l3/ is where this fixture's mutants were executed; attesting a dir the runner never
    # entered is its own blind spot, pinned by
    # test_mutate_attestation_fails_closed_when_an_attested_dir_ran_no_mutant.
    ATTEST = ("--attest-ref", "origin/main", "--attest-tests", "tests/unit/test_l3_payload.cpp",
              "--attest-dirs", "l3")

    # (a) a malformed label already on main: gates without the attestation, listed with it.
    for bad, msg in ((" // mutant-ok(whatever): not a category", "unknown label category 'whatever'"),
                     (" // mutant-ok(equivalent):", "malformed label")):
        src = list(SRC)
        src[3] = "    if (a < 0)" + bad
        root, reports = _setup(tmp_path, src, MUTANTS)
        rc_gate, out_gate, _ = _report(tmp_path, root, reports)
        assert rc_gate == 1 and msg in out_gate, out_gate     # discriminating: it DOES gate otherwise
        rc, out, doc = _report(tmp_path, root, reports, *ATTEST)
        assert rc == 0, out
        assert msg in out, out                                # still reported, on stdout …
        assert any(msg in m for m in doc["malformed_labels"]), doc   # … and in report.json
        assert doc["attest"]["dirs"] == ["l3"], doc

    # (b) a stale label stays a reported warning: suppressing the gate must not suppress the
    # information the attestation exists to produce.
    root, reports = _setup(tmp_path, SRC, [_mutant(4, 11, "cxx_lt_to_le", "Killed")])
    rc, out, doc = _report(tmp_path, root, reports, *ATTEST)
    assert rc == 0, out
    assert doc["stale_labels"] and "warning: stale label: l3/x.cpp:4" in out, (doc, out)

    # (c) the run did not happen: both blind spots still fail, and say which.
    root, reports = _setup(tmp_path, SRC, [])
    rc, out, _ = _report(tmp_path, root, reports, *ATTEST)
    assert rc == 1 and "no mutants" in out and "blind spot" in out, out
    for f in reports.glob("*.json"):
        f.unlink()
    rc, out, _ = _report(tmp_path, root, reports, *ATTEST)
    assert rc == 1 and "no Mull reports" in out and "blind spot" in out, out

    # (d) and the shell's own disclosure says exactly that, not "no gate at all".
    clone = shared_clone(tmp_path / "shell", "tests/unit/test_link_master.cpp", TEST_EDIT)
    rc, out, _ = run(clone / "tools" / "mutate.sh", "--diff", "HEAD~1", "--dry-run")
    assert rc == 0, out
    assert "gate=none" not in attest_line(out), attest_line(out)
    assert "gate=blind-spot-only" in attest_line(out), attest_line(out)


def test_mutate_attestation_fails_closed_when_an_attested_dir_ran_no_mutant(tmp_path):
    """The blind spot the attest path opens, and the one rule it is NOT covered by
    (#593 red-team round 3, MEDIUM). mutate_report.py's per-changed-dir rule — written for an
    `includePaths` regex that reaches one scope dir but not another — is guarded on
    `diff_mode and ranges`, and an attestation is trend mode with no ranges, so that rule is
    unreachable on the new path. Yet the attest path is the ONLY one that names specific
    directories in report.json: the `test_<dir>_*` oracle binaries carry sibling dirs' mutants
    too, so one mutant anywhere in scope keeps `total > 0`, the global "no mutants" rule stays
    silent, and the run records a kill rate for a directory in which nothing was executed.

    Same report, same rules, only `--attest-dirs` differing — if the reporter's exit were
    independent of which dir is attested, the control below would be indistinguishable from
    the attack and neither would be evidence."""
    root, reports = _setup(tmp_path, SRC, MUTANTS)      # every mutant is in l3/x.cpp

    def attest(dirs):
        return ("--attest-ref", "origin/main",
                "--attest-tests", " ".join(f"tests/unit/test_{d}_x.cpp" for d in dirs.split()),
                "--attest-dirs", dirs)

    rc, out, _ = _report(tmp_path, root, reports, *attest("link"))
    assert rc == 1, out
    assert "link/" in out and "blind spot" in out, out
    assert "no mutant" in out, out
    # A union attestation fails on the dir that ran nothing and names only that one.
    rc, out, _ = _report(tmp_path, root, reports, *attest("l3 link"))
    assert rc == 1, out
    failed_on = [l for l in out.splitlines() if "blind spot" in l]
    assert len(failed_on) == 1, out
    assert "link/" in failed_on[0] and "l3/" not in failed_on[0], failed_on
    # The discriminating control: attest the dir the mutants are actually in — exit 0, with the
    # findings still listed and never gated (that is what the cases above pin).
    rc, out, doc = _report(tmp_path, root, reports, *attest("l3"))
    assert rc == 0, out
    assert doc["attest"]["dirs"] == ["l3"], doc
    assert doc["unlabelled"] == 1, doc


def _two_dir_report(tmp_path, l3_status):
    """One Elements report: both l3/x.cpp mutants at `l3_status`, one Killed link/y.cpp
    mutant. The link/ mutant is why the global "no mutants in a non-empty scope" rule stays
    silent — a dir's test_<dir>_* oracle binaries carry the sibling scope dirs' mutants too,
    so the only rule left to notice l3/ is the per-dir one."""
    root, reports = _setup(tmp_path, SRC, [])
    (root / "link").mkdir(exist_ok=True)
    (root / "link" / "y.cpp").write_text("int g(int a) {\n    if (a >= 4)\n        return 1;\n"
                                         "    return 0;\n}\n")
    (reports / "test_x.json").write_text(json.dumps({"files": {
        str(root / "l3" / "x.cpp"): {"mutants": [_mutant(2, 11, "cxx_ge_to_gt", l3_status),
                                                 _mutant(4, 11, "cxx_lt_to_le", l3_status)]},
        str(root / "link" / "y.cpp"): {"mutants": [_mutant(2, 11, "cxx_ge_to_gt", "Killed")]}}}))
    return root, reports


def test_report_blind_spot_rules_count_mutants_the_runner_RAN_not_ones_it_listed(tmp_path):
    """Both per-dir blind-spot rules ask whether the runner EXECUTED a mutant under a
    directory, and a mutant in the Elements report is not evidence that it did: Mull reports
    `NoCoverage` for a mutant no binary reached (the very shape RANK's own comment describes,
    and the same thing the rules mean by "the oracle does not reach this directory"), and
    `Ignored`/`Pending` for ones it never ran at all. Counting those made an attestation of a
    directory whose mutants ALL came back unexecuted exit 0 at 100 % — the exact failure the
    rule was written for (#593 red-team round 4).

    An unknown status counts as not executed too: a Mull release adding one must fail closed,
    not silently satisfy the rule."""
    def attest(dirs):
        return ("--attest-ref", "origin/main",
                "--attest-tests", " ".join(f"tests/unit/test_{d}_x.cpp" for d in dirs.split()),
                "--attest-dirs", dirs)

    for status in ("NoCoverage", "Ignored", "Pending", "SomeStatusMullAddsLater"):
        root, reports = _two_dir_report(tmp_path, status)
        rc, out, doc = _report(tmp_path, root, reports, *attest("l3"))
        assert rc == 1, (status, out)
        assert "l3/" in out and "blind spot" in out and "no mutant" in out, (status, out)
        # And the green it used to produce is exactly what must not be on offer.
        assert "mutation: PASS" not in out, (status, out)

    # The discriminating control, same fixture and same rule: a status that means a binary
    # really ran the mutant attests fine, so the exit is not independent of the status.
    for status in ("Survived", "Killed"):
        root, reports = _two_dir_report(tmp_path, status)
        rc, out, doc = _report(tmp_path, root, reports, *attest("l3"))
        assert rc == 0, (status, out)
        assert doc["attest"]["dirs"] == ["l3"], (status, doc)

    # The same question in diff mode, where the per-changed-dir rule was already reachable:
    # l3/x.cpp's changed lines produce only NoCoverage mutants, so `not_covered` keeps the
    # global rule quiet while nothing under l3/ was ever executed.
    root, reports = _two_dir_report(tmp_path, "NoCoverage")
    (tmp_path / "ranges.json").write_text(json.dumps({"l3/x.cpp": [[1, 8]]}))
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1, out
    assert "no mutants under l3/" in out and "blind spot" in out, out


def test_report_attest_flags_are_all_or_nothing(tmp_path):
    """`--attest-*` suppresses the malformed-label gate, so a caller must not be able to
    switch it off by accident: any ONE of the three flags used to do it (#593 red-team round 3),
    which meant an ordinary whole-tree run with a stray `--attest-dirs` lost the gate
    `test_report_malformed_label_fails_in_every_mode` exists to hold, and recorded
    `origin: changed-tests` with `tests: []` — a provenance false on its face. The three
    describe one attestation and are now required together; anything partial is a usage error
    (exit 2), never a quietly suppressed gate."""
    src = list(SRC)
    src[3] = "    if (a < 0) // mutant-ok(whatever): unknown category, malformed"
    root, reports = _setup(tmp_path, src, MUTANTS)
    ALL = ("--attest-ref", "origin/main", "--attest-tests", "tests/unit/test_l3_x.cpp",
           "--attest-dirs", "l3")
    # The gate this is about, with no attest flag at all: it fails, in trend mode.
    rc, out, _ = _report(tmp_path, root, reports)
    assert rc == 1 and "malformed mutant-ok label(s) — failing" in out, out
    # One or two of the three — all six non-empty proper subsets, the sixth being
    # {--attest-ref, --attest-dirs}, red-team round 3's stray flag with a ref attached: a
    # usage error, and the gate is NOT suppressed on the way.
    for partial in (ALL[:2], ALL[2:4], ALL[4:], ALL[:4], ALL[2:], ALL[:2] + ALL[4:]):
        rc, out, _ = _report(tmp_path, root, reports, *partial)
        assert rc == 2, (partial, out)
        assert "must be given together" in out, (partial, out)
        assert "NOT gated" not in out, (partial, out)
    # All three — the only caller shape mutate.sh produces — still suppresses it, as the
    # 2026-09-14 ruling requires.
    rc, out, doc = _report(tmp_path, root, reports, *ALL)
    assert rc == 0, out
    assert "malformed mutant-ok label(s) in the attested dir(s) — reported, NOT gated" in out, out
    assert doc["attest"]["tests"] == ["tests/unit/test_l3_x.cpp"], doc


def test_report_attest_flags_reject_a_blank_value(tmp_path):
    """The all-or-nothing rule is about CONTENT, not truthiness: a single space is a truthy
    Python string, so `all()` accepted it while `.split()` gave [] — the gate suppressed, the
    per-attested-dir blind-spot rule iterating nothing, and `report.json` carrying
    `origin: changed-tests` with `tests: []` and `dirs: []`, verbatim the provenance-false-on-
    its-face shape the all-or-nothing rule exists to prevent (#593 red-team round 4). The
    threat model is a caller's mis-expanded flag (`--attest-dirs "$DIRS "` with DIRS unset),
    the same one round 3's finding had, so a blank value must be the same exit-2 usage error
    an empty one already is. tools/mutate.sh cannot emit one (ATTEST_DIRS is non-empty
    whenever ATTEST=1), so this is the reporter's own guard, not a reachable CI path."""
    src = list(SRC)
    src[3] = "    if (a < 0) // mutant-ok(whatever): unknown category, malformed"
    root, reports = _setup(tmp_path, src, MUTANTS)
    GOOD = ["origin/main", "tests/unit/test_l3_x.cpp", "l3"]
    for blank in (" ", "\t", "  \n "):
        for i in range(len(GOOD)):
            vals = list(GOOD)
            vals[i] = blank
            rc, out, _ = _report(tmp_path, root, reports, "--attest-ref", vals[0],
                                 "--attest-tests", vals[1], "--attest-dirs", vals[2])
            assert rc == 2, (blank, i, out)
            assert "must be given together" in out, (blank, i, out)
            assert "NOT gated" not in out, (blank, i, out)
        # All three blank is the same usage error, not a quietly ungated whole-tree run.
        rc, out, _ = _report(tmp_path, root, reports, "--attest-ref", blank,
                             "--attest-tests", blank, "--attest-dirs", blank)
        assert rc == 2, (blank, out)
        assert "must be given together" in out and "NOT gated" not in out, (blank, out)
    # The discriminating control: the same three flags with content are accepted, and no
    # attest flag at all still leaves the gate where it was.
    rc, out, _ = _report(tmp_path, root, reports, "--attest-ref", GOOD[0],
                         "--attest-tests", GOOD[1], "--attest-dirs", GOOD[2])
    assert rc == 0 and "NOT gated" in out, out
    rc, out, _ = _report(tmp_path, root, reports)
    assert rc == 1 and "malformed mutant-ok label(s) — failing" in out, out


# --- tools/mutate.sh -> tools/mutate_report.py: the attestation is HANDED OVER, end to end ------
# Every other mutate.sh case above stops at --dry-run or at the missing-Mull skip, so the half
# of the script that hands the attestation to the reporter — ATTEST_ARGS, --ref "$REPORT_REF",
# the runner loop — was covered by a regex over the script's own source text alone. Red-team
# round 2 on #593 emptied ATTEST_ARGS and the whole file stayed green: with the --attest-* args
# gone the reporter sees a plain trend run, where a malformed `mutant-ok` label already on main
# exits 1 — the Option-B failure (redding a PR for debt it never touched) that
# docs/OPEN-QUESTIONS.md 2026-09-14 rejected and this path exists to prevent.
#
# Mull is absent here and in CI's `native` job, so the runner, cmake and clang++ are shimmed,
# the way tools/fuzz-smoke.sh's cases shim the five fuzzers (#141 review): what is REAL in this
# case is tools/mutate.sh, tools/mutate_report.py and everything that passes between them. What
# it does NOT cover, and what still needs a run with Mull installed: the instrumented build,
# the phase-2 includePaths filter, and the shape of a real Elements report.
_MULL_SHIM = r"""#!/usr/bin/env bash
# Stand-in for mull-runner: writes one canned Elements report for the target binary, says
# "No mutants found" for the rest of the oracle, and exits non-zero — the real runner does that
# whenever survivors exist, and mutate.sh must not read it as a failure.
name=""; dir="reports"
while [ $# -gt 0 ]; do
  case "$1" in
    --report-name) name="$2"; shift 2 ;;
    --report-dir) dir="$2"; shift 2 ;;
    *) shift ;;
  esac
done
mkdir -p "$dir"
root=$(cd ../.. && pwd -P)     # the runner's cwd is the build dir (mutate.sh cds there)
if [ "$name" = "$MULL_SHIM_TARGET" ]; then
  cat > "$dir/$name.json" <<JSON
{"files": {"$root/$MULL_SHIM_FILE": {"mutants": [
  {"id": "cxx_ge_to_gt:$MULL_SHIM_LINE:9", "mutatorName": "cxx_ge_to_gt", "status": "Survived",
   "location": {"start": {"line": $MULL_SHIM_LINE, "column": 9},
                "end": {"line": $MULL_SHIM_LINE, "column": 10}}}]}}}
JSON
  echo "Surviving mutants: 1"
else
  echo "No mutants found"
fi
exit 1
"""

_CMAKE_SHIM = r"""#!/usr/bin/env bash
# Stand-in for cmake: configuring is a no-op, and `--build <dir> ... --target a b c` drops an
# executable stub for every named target — all the runner loop needs to get past its
# "oracle binary was not built" blind-spot rule.
if [ "$1" = "--build" ]; then
  dir=$2; shift 2
  in_targets=0
  for a in "$@"; do
    [ "$a" = "--target" ] && { in_targets=1; continue; }
    case "$a" in --*) continue ;; esac
    [ "$in_targets" = 1 ] && { printf '#!/bin/sh\nexit 0\n' > "$dir/$a"; chmod +x "$dir/$a"; }
  done
fi
exit 0
"""

# An UNTRACKED source under the attested dir: `git diff` never lists untracked files, so the
# diff stays test-only (asserted below) while `find link -type f` — the attestation's scope —
# picks it up. Its label is malformed on purpose: that is the rule which gates in trend mode
# and must not gate on an attestation.
PROBE_REL = "link/zz_attest_probe.cpp"
PROBE_SRC = ("// an untracked probe source: in the attested dir, not in the diff\n"
             "int zz_attest_probe(int a) {\n"
             "    if (a >= 4) // mutant-ok(whatever): not a category — malformed, and already on main\n"
             "        return 1;\n"
             "    return 0;\n"
             "}\n")
PROBE_MUTANT_LINE = 3


def test_mutate_attestation_is_handed_to_the_reporter_end_to_end(tmp_path):
    """mutate.sh run to completion on a test-only diff, with the runner shimmed: the reporter
    must receive the attestation (`--attest-ref/--attest-tests/--attest-dirs`) and the empty ref,
    so a malformed label already in the attested dir is REPORTED and the run exits 0. Drop the
    ATTEST_ARGS block from mutate.sh and this case fails on both halves — exit 1 and no
    `attest` key in report.json — which is what no other case in this file does."""
    clone = shared_clone(tmp_path, "tests/unit/test_link_master.cpp", TEST_EDIT)
    (clone / PROBE_REL).write_text(PROBE_SRC)
    changed = subprocess.run(["git", "diff", "--name-only", "HEAD~1"], cwd=clone,
                             capture_output=True, text=True, check=True).stdout.split()
    # The diff is test-only: the probe is untracked, and nothing under a scope dir is in it.
    # (tools/mutate.sh itself can be: shared_clone copies the working-tree script in.)
    assert "tests/unit/test_link_master.cpp" in changed, changed
    assert not [f for f in changed if f.split("/")[0] in ("l3", "link", "core")], changed

    shim = tmp_path / "shim"
    shim.mkdir()
    (shim / "cmake").write_text(_CMAKE_SHIM)
    (shim / "clang++").write_text('#!/usr/bin/env bash\necho "clang version 18.1.8"\nexit 0\n')
    (shim / "mull-runner").write_text(_MULL_SHIM)
    for tool in ("cmake", "clang++", "mull-runner"):
        (shim / tool).chmod(0o755)
    (shim / "mull-ir-frontend").write_text("not a real plugin; mutate.sh only tests -f\n")

    env = {"PATH": f"{shim}:{os.environ['PATH']}", "OMGP_CLANG_MAJOR": "18",
           "MULL_RUNNER": str(shim / "mull-runner"), "MULL_PLUGIN": str(shim / "mull-ir-frontend"),
           "MULL_SHIM_TARGET": "test_link_master", "MULL_SHIM_FILE": PROBE_REL,
           "MULL_SHIM_LINE": str(PROBE_MUTANT_LINE)}
    rc, out, _ = run(clone / "tools" / "mutate.sh", "--diff", "HEAD~1", "--require",
                     env_overrides=env, timeout=300)
    assert rc == 0, out
    assert "mutation: PASS" in out, out
    assert "gate=blind-spot-only" in attest_line(out), out
    # The mull-present line: this run really did get past the tool check into the runner loop.
    assert "test-only attestation of link — trend only; no finding gates" in out, out
    assert "mutation: test_link_master: Surviving mutants: 1" in out, out
    doc = json.loads((clone / "build" / "mutate" / "report.json").read_text())
    # The provenance the shell derived, as the reporter recorded it — the whole point of the
    # call site this case exists to cover.
    assert doc["attest"] == {"origin": "changed-tests", "diff_ref": "HEAD~1",
                             "tests": ["tests/unit/test_link_master.cpp"], "dirs": ["link"]}, doc
    assert doc["mode"] == "trend", doc
    # … and the finding that would have gated a trend run is reported, not gated.
    assert any("unknown label category 'whatever'" in m for m in doc["malformed_labels"]), doc
    assert f"ERROR: {PROBE_REL}:{PROBE_MUTANT_LINE}: unknown label category 'whatever'" in out, out
    assert "malformed mutant-ok label(s) in the attested dir(s) — reported, NOT gated" in out, out


# --- tools/mutate_report.py: the triage gate on synthetic Elements reports --------------------

REPORT = ROOT / "tools" / "mutate_report.py"


def _mutant(line, col, mutator, status):
    return {"id": f"{mutator}:{line}:{col}", "mutatorName": mutator, "status": status,
            "location": {"start": {"line": line, "column": col}, "end": {"line": line, "column": col + 1}}}


def _setup(tmp_path, source_lines, mutants, ranges=None, removed=None):
    """A fake repo root with l3/x.cpp and one Elements report naming its mutants.

    `removed` is the other half of the diff (tools/mutate_ranges.py): the text of the lines
    each file's hunks deleted. Default {} = the diff deleted nothing anywhere."""
    root = tmp_path / "repo"
    (root / "l3").mkdir(parents=True, exist_ok=True)
    (root / "l3" / "x.cpp").write_text("\n".join(source_lines) + "\n")
    reports = tmp_path / "reports"
    reports.mkdir(exist_ok=True)
    (reports / "test_x.json").write_text(json.dumps(
        {"files": {str(root / "l3" / "x.cpp"): {"mutants": mutants}}}))
    (tmp_path / "ranges.json").write_text(json.dumps(ranges if ranges is not None else {}))
    (tmp_path / "removed.json").write_text(json.dumps(removed if removed is not None else {}))
    return root, reports


def _cfg_source_ext() -> str:
    cp = configparser.ConfigParser()
    cp.read(CFG)
    return cp["policy"]["source_ext"]


def _report(tmp_path, root, reports, *extra, source_ext=None, pass_removed=True):
    # --source-ext is what mutate.sh passes from the cfg; the reporter has no default of its own.
    # --removed likewise: pass_removed=False is a caller that never read the deleted half of the
    # diff, which the comment-only exemption must fail closed on.
    removed = ["--removed", str(tmp_path / "removed.json")] if pass_removed else []
    r = subprocess.run([sys.executable, str(REPORT), "--reports", str(reports), "--root", str(root),
                        "--scope-dirs", "l3 link core", "--ranges", str(tmp_path / "ranges.json"),
                        "--source-ext", source_ext if source_ext is not None else _cfg_source_ext(),
                        *removed, "--out", str(tmp_path / "report.json"), *extra],
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


def test_report_diff_mode_comment_only_changes_are_not_a_blind_spot(tmp_path):
    """A diff that touches only comment lines under a scope dir leaves no changed line a
    mutant can sit on, so the in-scope count is 0 and the whole-scope blind-spot rule fired —
    reddening CI for a PR that rewrote `mutant-ok` justifications (PR #558, link/master.cpp).
    Comments are removed in translation phase 3, before anything Mull mutates exists, so a
    line that is a comment after phase 2 carries no mutant: structurally the same case as
    `mutation-exempt(no-body)`, at line granularity, and no marker is needed to see it. The
    "after phase 2" qualifier is the subject of the two cases below it."""
    root, reports = _setup(tmp_path, SRC, MUTANTS, {"l3/x.cpp": [[6, 6]]})   # line 6: a comment
    rc, out, doc = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 0, out
    assert "failing (blind spot" not in out, out
    assert "l3/x.cpp" in out and "blank or a // comment" in out, out
    assert doc["survived"] == 0 and doc["unlabelled"] == 0


def test_report_diff_mode_still_fails_when_a_changed_code_line_yields_no_mutant(tmp_path):
    """The exemption above is per changed LINE and fails closed: one changed line that is not
    blank and not a `//` comment is code Mull should have reached, so zero mutants there is
    still the blind spot the rule exists to catch."""
    # Line 3 (`return 1;`) is code that carries no mutant; line 6 is a comment.
    root, reports = _setup(tmp_path, SRC, MUTANTS, {"l3/x.cpp": [[3, 3], [6, 6]]})
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1, out
    assert "blind spot" in out and "l3/x.cpp" in out, out


def test_report_deletion_only_change_is_not_exempted(tmp_path):
    """mutate.sh records a changed file with an EMPTY range list when every hunk in it is a
    pure deletion (`@@ -a,b +c,0 @@`): it does `ranges.setdefault(cur, [])` on the `+++` line
    and appends only when the hunk's `+count` is non-zero. No changed line exists there to
    read, so the comment-only exemption has nothing to decide and must not certify the file —
    it fails closed, exactly as the rule does without the exemption (red-team B2 on #558)."""
    root, reports = _setup(tmp_path, SRC, MUTANTS, {"l3/x.cpp": []})
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1, out
    assert "blind spot" in out and "l3/x.cpp" in out, out
    assert "blank or a // comment" not in out, out   # never a reason about lines nobody read


def test_report_backslash_continued_comment_line_is_not_exempted(tmp_path):
    r"""Translation phase 2 splices backslash-newline BEFORE phase 3 removes comments
    ([lex.comment]), so a `//` comment whose last character is `\` swallows the line below
    it: a changed line that reads as a comment but DELETES mutable code. `-Werror=comment`
    (implied by -Wall, CMakeLists.txt) stops such a file reaching this gate in this repo, but
    that is a control in another file, not a property of the predicate — so the predicate
    rejects the shape itself (red-team B3 on #558)."""
    src = list(SRC)
    src[5] = "    // mutant-ok(accepted, cxx_gt_to_ge): cap check; only detail bytes differ \\"
    root, reports = _setup(tmp_path, src, MUTANTS, {"l3/x.cpp": [[6, 6]]})
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1, out
    assert "blind spot" in out and "l3/x.cpp" in out, out
    assert "blank or a // comment" not in out, out


def test_report_malformed_label_on_a_changed_comment_line_fails(tmp_path):
    """The comment-only exemption must not become a hole in the label syntax check: a PR that
    only rewrites `mutant-ok` justifications changes exactly the lines the policy lives on. A
    malformed label on a changed line fails even when no mutant is in scope."""
    src = list(SRC)
    src[5] = "    // mutant-ok(whatever, cxx_gt_to_ge): not a category"
    root, reports = _setup(tmp_path, src, MUTANTS, {"l3/x.cpp": [[6, 6]]})
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1, out
    assert "unknown label category 'whatever'" in out, out


def test_report_mixed_hunk_that_deletes_code_is_not_exempted(tmp_path):
    """A hunk that deletes N code lines and adds one `//` comment leaves a range list holding
    only the comment. Deciding the exemption from that list alone certifies the file while
    reading strictly LESS of the diff than the deletion-only case above rejects, and prints a
    reason ("every line the diff ... is blank or a // comment") that is false of the deleted
    lines (red-team B1 on #558). So the removed half of the diff is read too: one deleted line
    that is not blank and not a `//` comment is mutable code the diff took away, and the file
    goes back under the blind-spot rule."""
    removed = {"l3/x.cpp": [{"text": "    if (a > 9)", "after": 3}, {"text": "        return 2;", "after": -1}]}
    root, reports = _setup(tmp_path, SRC, MUTANTS, {"l3/x.cpp": [[6, 6]]}, removed=removed)
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1, out
    assert "blind spot" in out and "l3/x.cpp" in out, out
    assert "blank or a // comment" not in out, out
    # The same hunk with only comment lines deleted IS the shape the exemption is for.
    root, reports = _setup(tmp_path, SRC, MUTANTS, {"l3/x.cpp": [[6, 6]]},
                           removed={"l3/x.cpp": [{"text": "    // cap check; only detail bytes differ", "after": 3},
                                                 {"text": "", "after": -1}]})
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 0 and "blank or a // comment" in out, out


def test_report_comment_only_exemption_needs_the_lines_the_diff_removed(tmp_path):
    """The removed half is an input, not an assumption: a caller that does not hand it over
    (an older tools/mutate.sh, or a hand-run of the reporter) has left the predicate unable to
    see a deletion at all, so the exemption is unavailable and the rule applies as on main."""
    root, reports = _setup(tmp_path, SRC, MUTANTS, {"l3/x.cpp": [[6, 6]]})
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main", pass_removed=False)
    assert rc == 1, out
    assert "blind spot" in out and "l3/x.cpp" in out, out
    assert "blank or a // comment" not in out, out


BLOCK_TOGGLE = [
    "int f(int a) {",                # 1
    "    if (a >= 4)",               # 2  the report's mutant sits here, off the changed line
    "        return 1;",             # 3
    "    /* disabled:",              # 4
    "    if (a > 9)",                # 5
    "// */   return 2;",             # 6  changed: starts with `//`, yet `return 2;` is LIVE
    "    return 0;",                 # 7
    "}",
]


def test_report_block_comment_terminator_on_a_changed_line_is_not_exempted(tmp_path):
    """`text.strip().startswith("//")` is a line-local test, and "is this line a comment after
    phase 2" is not decidable one line at a time: in the ordinary `/* ... // */ code` toggle
    idiom the `*/` ENDS the block comment and what follows it on the same line is executable
    (red-team B2 on #558). Nothing outside this predicate catches it — `-Wcomment` fires on
    `/*` inside a comment, which this shape does not contain. A changed line carrying `*/` is
    therefore never comment-only, whatever it starts with."""
    root, reports = _setup(tmp_path, BLOCK_TOGGLE, [_mutant(2, 11, "cxx_ge_to_gt", "Survived")],
                           {"l3/x.cpp": [[6, 6]]})
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1, out
    assert "blind spot" in out and "l3/x.cpp" in out, out
    assert "blank or a // comment" not in out, out


def test_report_raw_string_literal_file_is_not_exempted(tmp_path):
    """Inside a raw string literal a `//` line is string content, not a comment — and where
    the literal opens is not visible from the changed line. The delimiters are not decidable
    line by line either (`R"x(...)x"`), so a file containing `R"` anywhere is never exempt
    rather than parsed."""
    src = ["const char* k = R\"(", "// not a comment: this is string data", ")\";",
           "int f(int a) { return a >= 4 ? 1 : 0; }"]
    root, reports = _setup(tmp_path, src, [_mutant(4, 25, "cxx_ge_to_gt", "Survived")],
                           {"l3/x.cpp": [[2, 2]]})
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1, out
    assert "blind spot" in out and "l3/x.cpp" in out, out


def test_report_comment_added_after_a_spliced_line_is_not_exempted(tmp_path):
    """Phase 2 splices the line below a `\\`-ended line into it, so INSERTING a `//` comment
    after one truncates the logical line — everything the continuation would have carried is
    commented out, without any deleted line for the check above to see. The inserted line
    reads as a comment and is not one (it is the tail of a macro), so a changed line whose
    predecessor ends in `\\` fails closed too."""
    src = ["#define CLAMP(x) do { \\", "    // the logical line ends here, not at `while`",
           "    (x) = (x) > 9 ? 9 : (x); \\", "} while (0)",
           "int f(int a) { return a >= 4 ? 1 : 0; }"]
    root, reports = _setup(tmp_path, src, [_mutant(5, 25, "cxx_ge_to_gt", "Survived")],
                           {"l3/x.cpp": [[2, 2]]})
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1, out
    assert "blind spot" in out and "l3/x.cpp" in out, out


def test_report_comment_deleted_after_a_spliced_line_is_not_exempted(tmp_path):
    """The mirror of the case above (round-8 review on #558): DELETING a `//` comment that sat
    right after a `\\`-ended line re-splices the line below it into that logical line — the
    pre-image's comment ended the macro; without it the next line is macro body. The deleted
    line reads as a comment and its removal changed what compiles, so a deleted line whose
    surviving predecessor ends in `\\` fails closed exactly as an added one does. The deleted
    half arrives as text plus the post-image line of its predecessor (tools/mutate_ranges.py),
    which is all the check needs."""
    src = ["#define CLAMP(x) do { \\", "    (x) = (x) > 9 ? 9 : (x); \\", "} while (0)",
           "int f(int a) { return a >= 4 ? 1 : 0; }", "// a note the diff added"]
    removed = {"l3/x.cpp": [{"text": "    // the logical line ends here, not at `while`", "after": 1}]}
    root, reports = _setup(tmp_path, src, [_mutant(4, 25, "cxx_ge_to_gt", "Survived")],
                           {"l3/x.cpp": [[5, 5]]}, removed=removed)
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1, out
    assert "blind spot" in out and "l3/x.cpp" in out, out
    # A deleted comment whose predecessor was itself deleted is judged against THAT line.
    removed = {"l3/x.cpp": [{"text": "#define OLD(x) ((x) + 1) \\", "after": 3},
                            {"text": "    // continued", "after": -1}]}
    root, reports = _setup(tmp_path, src, [_mutant(4, 25, "cxx_ge_to_gt", "Survived")],
                           {"l3/x.cpp": [[5, 5]]}, removed=removed)
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1 and "blind spot" in out, out


def test_report_deleted_half_with_an_impossible_position_is_not_exempted(tmp_path):
    """A deleted line whose `after` names a post-image line the checked-out tree does not have
    (a stale or skewed removed map: the ranges and the tree disagree) cannot have its
    predecessor checked, and an unchecked predecessor is exactly the shape the splice check
    exists for — so the file is refused, not certified (round-8 red team on #558: this arm was
    pinned by no case, and a `continue` in its place left the suite green)."""
    src = ["// a note the diff added", "int f(int a) { return a >= 4 ? 1 : 0; }"]
    for after in (3, 99):
        removed = {"l3/x.cpp": [{"text": "    // gone", "after": after}]}
        root, reports = _setup(tmp_path, src, [_mutant(2, 25, "cxx_ge_to_gt", "Survived")],
                               {"l3/x.cpp": [[1, 1]]}, removed=removed)
        rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
        assert rc == 1, (after, out)
        assert "blind spot" in out and "l3/x.cpp" in out, (after, out)
        assert "blank or a // comment" not in out, (after, out)
    # -1 with no previous entry is the same refusal: a chained predecessor that does not exist.
    removed = {"l3/x.cpp": [{"text": "    // gone", "after": -1}]}
    root, reports = _setup(tmp_path, src, [_mutant(2, 25, "cxx_ge_to_gt", "Survived")],
                           {"l3/x.cpp": [[1, 1]]}, removed=removed)
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1 and "blind spot" in out and "blank or a // comment" not in out, out


def test_report_text_only_removed_map_is_treated_as_never_handed_over(tmp_path):
    """The removed half's format is {"text", "after"} since the deleted-half splice check; an
    older text-only list carries no positions, so the exemption must be UNAVAILABLE — the same
    outcome as omitting --removed — and the refusal must be the clean blind-spot one, not a
    TypeError on `e["text"]` (round-8 red team on #558: with the validator deleted the suite
    stayed green while the predicate died on a str)."""
    src = ["// a note the diff added", "int f(int a) { return a >= 4 ? 1 : 0; }"]
    for removed in ({"l3/x.cpp": ["    // gone"]}, {"l3/x.cpp": [{"text": "    // gone"}]},
                    {"l3/x.cpp": [["    // gone", 0]]}):
        root, reports = _setup(tmp_path, src, [_mutant(2, 25, "cxx_ge_to_gt", "Survived")],
                               {"l3/x.cpp": [[1, 1]]}, removed=removed)
        rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
        assert rc == 1, (removed, out)
        assert "blind spot" in out and "l3/x.cpp" in out, (removed, out)
        assert "Traceback" not in out and "TypeError" not in out, (removed, out)
        assert "blank or a // comment" not in out, (removed, out)


def test_mutate_ranges_names_the_predecessor_of_a_pure_deletion(tmp_path):
    """For `@@ -a,b +c,0 @@` unified diff gives c as the post-image line BEFORE the deletion, so
    the first deleted line's surviving predecessor is post line c (c-1 when the hunk also adds
    lines); a deletion at the top of the file has none (0)."""
    diff = ("diff --git a/l3/z.cpp b/l3/z.cpp\n--- a/l3/z.cpp\n+++ b/l3/z.cpp\n"
            "@@ -1,1 +0,0 @@\n-// header\n@@ -4,2 +2,0 @@\n-    // gone\n-    // gone too\n")
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "mutate_ranges.py"),
                        "--ranges-out", str(tmp_path / "rg.json"),
                        "--removed-out", str(tmp_path / "rm.json")],
                       input=diff, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert json.loads((tmp_path / "rg.json").read_text()) == {"l3/z.cpp": []}
    assert json.loads((tmp_path / "rm.json").read_text()) == {"l3/z.cpp": [
        {"text": "// header", "after": 0}, {"text": "    // gone", "after": 2},
        {"text": "    // gone too", "after": -1}]}


def test_mutate_ranges_feeds_the_gate_the_deleted_half_of_a_mixed_hunk(tmp_path):
    """End to end on a real `git diff -U0`, not a synthesised range list: tools/mutate_ranges.py
    is the one parser the gate reads, and a hunk that deletes two code lines and adds one
    comment must reach mutate_report.py as the comment (added range) AND both deleted lines
    (removed text), so that the file is NOT certified comment-only — red-team B1's reproducer
    on #558, which greened before this. The second file pins that a modification records its
    old lines too."""
    repo = tmp_path / "r"
    (repo / "l3").mkdir(parents=True)
    subprocess.run(["git", "init", "-q", "."], cwd=repo, check=True)
    (repo / "l3" / "x.cpp").write_text("int f(int a) {\n    if (a >= 4)\n        return 1;\n"
                                       "    if (a > 9)\n        return 2;\n    return 0;\n}\n")
    (repo / "l3" / "y.cpp").write_text("int g() {\n    int t = 1;\n    return t;\n}\n")
    subprocess.run(["git", "add", "-A"], cwd=repo, check=True)
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "b"],
                   cwd=repo, check=True)
    (repo / "l3" / "x.cpp").write_text("int f(int a) {\n    if (a >= 4)\n        return 1;\n"
                                       "    // the a > 9 guard is now the caller's job\n"
                                       "    return 0;\n}\n")
    (repo / "l3" / "y.cpp").write_text("int g() {\n    return 1;\n}\n")
    diff = subprocess.run(["git", "diff", "-U0", "HEAD", "--", "l3"], cwd=repo,
                          capture_output=True, text=True, check=True).stdout
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "mutate_ranges.py"),
                        "--ranges-out", str(tmp_path / "rg.json"),
                        "--removed-out", str(tmp_path / "rm.json")],
                       input=diff, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    ranges = json.loads((tmp_path / "rg.json").read_text())
    removed = json.loads((tmp_path / "rm.json").read_text())
    assert ranges["l3/x.cpp"] == [[4, 4]], ranges
    # Each deleted line names the POST-image line of its surviving predecessor (`after`), or -1
    # when that predecessor was deleted too — what the splice check on the deleted half reads.
    assert removed["l3/x.cpp"] == [{"text": "    if (a > 9)", "after": 3},
                                   {"text": "        return 2;", "after": -1}], removed
    assert removed["l3/y.cpp"] == [{"text": "    int t = 1;", "after": 1},
                                   {"text": "    return t;", "after": -1}], removed
    assert ranges["l3/y.cpp"] == [[2, 2]], ranges
    # ... and the gate, handed exactly those two files, stays closed: one Elements report with
    # a mutant nowhere near the changed lines, i.e. zero in scope, as in the CI run.
    reports = tmp_path / "reports"
    reports.mkdir()
    (reports / "test_x.json").write_text(json.dumps({"files": {
        str(repo / "l3" / "x.cpp"): {"mutants": [_mutant(2, 11, "cxx_ge_to_gt", "Survived")]}}}))
    r = subprocess.run([sys.executable, str(REPORT), "--reports", str(reports), "--root", str(repo),
                        "--scope-dirs", "l3 link core", "--ranges", str(tmp_path / "rg.json"),
                        "--removed", str(tmp_path / "rm.json"), "--source-ext", _cfg_source_ext(),
                        "--ref", "HEAD", "--out", str(tmp_path / "report.json")],
                       capture_output=True, text=True, cwd=ROOT)
    out = r.stdout + r.stderr
    assert r.returncode == 1, out
    assert "blind spot" in out and "l3/x.cpp" in out and "l3/y.cpp" in out, out
    assert "blank or a // comment" not in out, out


def test_mutate_ranges_does_not_read_an_added_line_as_a_file_header(tmp_path):
    """`git diff -U0` renders an ADDED line whose own text begins with `++ ` as `+++ new note`,
    which is also the shape of a post-image file header. Reading it as one re-keys every LATER
    hunk of that file under a fabricated path: the real file loses the changed lines that follow
    (so a survivor on them is out of scope, `survived=0`), and the fabricated dir's per-dir
    blind-spot check is vacuously satisfied — the gate exits 0 on an unlabelled survivor sitting
    on a line the diff changed (#558 red-team B-A). A header only ever precedes its file's first
    `@@`, so that branch is reachable only outside a hunk, exactly as the `-` branch already is.
    The carrier here is a block comment: clang-format normalises a leading `++ ` away in code,
    but not inside `/* … */`."""
    repo = tmp_path / "r"
    (repo / "l3").mkdir(parents=True)
    subprocess.run(["git", "init", "-q", "."], cwd=repo, check=True)
    before = ("/* changelog:\n   older note\n*/\nint f(int a) {\n    if (a > 4)\n"
              "        return 1;\n    return 0;\n}\n")
    (repo / "l3" / "x.cpp").write_text(before)
    subprocess.run(["git", "add", "-A"], cwd=repo, check=True)
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "b"],
                   cwd=repo, check=True)
    # Hunk 1 adds the poison line inside the block comment; hunk 2 — the one that must not be
    # lost — changes a real guard.
    (repo / "l3" / "x.cpp").write_text(before.replace("/* changelog:\n", "/* changelog:\n++ new note\n")
                                             .replace("if (a > 4)", "if (a >= 4)"))
    diff = subprocess.run(["git", "diff", "-U0", "HEAD", "--", "l3"], cwd=repo,
                          capture_output=True, text=True, check=True).stdout
    assert "\n+++ new note\n" in diff, diff        # the shape under attack really is produced
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "mutate_ranges.py"),
                        "--ranges-out", str(tmp_path / "rg.json"),
                        "--removed-out", str(tmp_path / "rm.json")],
                       input=diff, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    ranges = json.loads((tmp_path / "rg.json").read_text())
    removed = json.loads((tmp_path / "rm.json").read_text())
    assert list(ranges) == ["l3/x.cpp"], ranges    # no fabricated key
    assert ranges["l3/x.cpp"] == [[2, 2], [6, 6]], ranges
    assert removed["l3/x.cpp"] == [{"text": "    if (a > 4)", "after": 5}], removed
    # ... and end to end: the survivor on the guard the diff changed is in scope and unlabelled.
    reports = tmp_path / "reports"
    reports.mkdir()
    (reports / "test_x.json").write_text(json.dumps({"files": {str(repo / "l3" / "x.cpp"): {"mutants": [
        _mutant(6, 9, "cxx_ge_to_gt", "Survived"), _mutant(7, 9, "cxx_remove_void_call", "Killed")]}}}))
    r = subprocess.run([sys.executable, str(REPORT), "--reports", str(reports), "--root", str(repo),
                        "--scope-dirs", "l3 link core", "--ranges", str(tmp_path / "rg.json"),
                        "--removed", str(tmp_path / "rm.json"), "--source-ext", _cfg_source_ext(),
                        "--ref", "HEAD", "--out", str(tmp_path / "report.json")],
                       capture_output=True, text=True, cwd=ROOT)
    out = r.stdout + r.stderr
    assert r.returncode == 1, out
    assert "UNLABELLED survivor: l3/x.cpp:6" in out, out
    assert "survived=1" in out, out


def test_report_diff_mode_fails_when_a_changed_dir_has_no_executed_mutants(tmp_path):
    """A run-time path filter (mutate.sh phase 2) that matches one scope dir but not another
    leaves total > 0, so the whole-scope blind-spot rule above never fires while every mutant
    of the unmatched dir silently goes unexecuted (#141 review, LOW). Per changed dir: at
    least one mutant, on ANY line, must have been executed — else it is a blind spot."""
    root, reports = _setup(tmp_path, SRC, MUTANTS, {"l3/x.cpp": [[1, 8]], "link/y.cpp": [[1, 2]]})
    (root / "link").mkdir()
    (root / "link" / "y.cpp").write_text("int g() {\n    return 1;\n}\n")
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1, out
    assert "link/" in out and "blind spot" in out and "no mutants under" in out
    # An exempt(no-body) file is the one legitimate way for a changed dir to carry none. rc is
    # 1 either way here (l3/x.cpp:2 is an unlabelled survivor), so the distinguishing
    # assertion is the blind-spot message's absence (#141 review @a16d7a6: the earlier
    # `"link/" not in out.split("survivor")[0]` held with the exemption branch deleted).
    (root / "link" / "y.cpp").write_text("// mutation-exempt(no-body): declarations only\nint g();\n")
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1 and "UNLABELLED survivor: l3/x.cpp:2" in out, out
    assert "no mutants under" not in out and "blind spot" not in out, out


def test_report_diff_mode_per_dir_rule_ignores_non_source_changes(tmp_path):
    """mutate.sh filters SCOPE to source extensions but builds scope_ranges.json from a plain
    `git diff -U0 -- <scope dirs>`, so a docs/CMake touch under a scope dir reaches the
    report as a changed file. The per-dir rule must not demand mutants for a dir whose only
    change is such a file — else `link/frame.cpp` + `l3/README.md` reds deep-verify with
    "no mutants under l3/" although no l3/ source changed (#141 review @a16d7a6, MEDIUM)."""
    src = list(SRC)
    src[1] = "    if (a >= 4) // mutant-ok(equivalent): 4 is never reached, callers pass < 4"
    root, reports = _setup(tmp_path, src, MUTANTS,
                           {"l3/x.cpp": [[1, 8]], "link/README.md": [[1, 3]], "link/CMakeLists.txt": [[1, 1]]})
    (root / "link").mkdir()
    (root / "link" / "README.md").write_text("# link\n\ndocs only\n")
    (root / "link" / "CMakeLists.txt").write_text("add_library(omgp_link INTERFACE)\n")
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 0, out
    assert "no mutants under" not in out and "blind spot" not in out, out
    # A source change in that dir still trips the rule: the filter narrows, never disables it.
    (root / "link" / "y.cpp").write_text("int g() {\n    return 1;\n}\n")
    (tmp_path / "ranges.json").write_text(json.dumps(
        {"l3/x.cpp": [[1, 8]], "link/README.md": [[1, 3]], "link/y.cpp": [[1, 3]]}))
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main")
    assert rc == 1 and "no mutants under link/" in out, out


def test_source_extensions_have_one_source_of_truth(tmp_path):
    """mutate.sh's SCOPE filter (diff AND whole-tree) and mutate_report.py's ranges filter each
    carried their own extension list; an extension added to one and not the other (say .cxx)
    would drop that file from `ranges`, so its survivors AND the per-changed-dir rule go quiet
    at once, silently (#141 review @d1fc20b, MEDIUM). One line in tools/mutate.cfg now feeds
    both — mutate.sh derives its grep and find from it and hands it to the reporter, which has
    no default — and each consumer is pinned here against a cfg carrying an extra extension."""
    exts = _cfg_source_ext().split()
    assert {"cpp", "hpp"} <= set(exts) and "cxx" not in exts, exts
    # mutate.sh, diff mode and whole-tree mode: a changed l3/z.cxx is out of scope with the cfg
    # as committed, and in scope — listed, with the l3 oracle — once `cxx` joins the cfg line.
    clone = shared_clone(tmp_path, "l3/z.cxx", "int z() { return 1; }\n")
    for args in (("--diff", "HEAD~1", "--dry-run"), ("--dry-run",)):
        rc, out, _ = run(clone / "tools" / "mutate.sh", *args)
        assert rc == 0 and "l3/z.cxx" not in out, (args, out)
    cfg = clone / "tools" / "mutate.cfg"
    cfg.write_text(re.sub(r"^(source_ext *=.*)$", r"\1 cxx", cfg.read_text(), flags=re.M))
    for args in (("--diff", "HEAD~1", "--dry-run"), ("--dry-run",)):
        rc, out, _ = run(clone / "tools" / "mutate.sh", *args)
        assert rc == 0 and "l3/z.cxx" in out.split("mutation: scope:", 1)[1].splitlines()[0], (args, out)
        assert any(b.startswith("test_l3_") for b in oracle_line(out)), out
    # mutate.sh hands that same cfg value to the reporter; no other list exists in either file.
    sh = MUTATE.read_text()
    assert re.search(r"^SOURCE_EXT=\$\(cfg source_ext\)$", sh, re.M), "mutate.sh must read source_ext from the cfg"
    assert '--source-ext "$SOURCE_EXT"' in sh, "mutate.sh must pass the cfg list to mutate_report.py"
    assert "cpp|hpp" not in sh and "'*.cpp'" not in sh, "a second extension list in mutate.sh"
    assert "cpp|hpp" not in REPORT.read_text(), "a second extension list in mutate_report.py"
    # The reporter's ranges filter follows what it is handed: link/y.cxx is a source (and the
    # per-dir rule fires for link/) exactly when cxx is in the list it was given.
    src = list(SRC)
    src[1] = "    if (a >= 4) // mutant-ok(equivalent): 4 is never reached, callers pass < 4"
    root, reports = _setup(tmp_path, src, MUTANTS, {"l3/x.cpp": [[1, 8]], "link/y.cxx": [[1, 3]]})
    (root / "link").mkdir()
    (root / "link" / "y.cxx").write_text("int g() {\n    return 1;\n}\n")
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main", source_ext="cpp hpp")
    assert rc == 0 and "no mutants under" not in out, out
    rc, out, _ = _report(tmp_path, root, reports, "--ref", "origin/main", source_ext="cpp hpp cxx")
    assert rc == 1 and "no mutants under link/" in out, out
    # And it refuses to run without the list — a default here would be the second copy.
    r = subprocess.run([sys.executable, str(REPORT), "--reports", str(reports), "--root", str(root),
                        "--scope-dirs", "l3 link core", "--ranges", str(tmp_path / "ranges.json"),
                        "--out", str(tmp_path / "report.json"), "--ref", "origin/main"],
                       capture_output=True, text=True, cwd=ROOT)
    assert r.returncode == 2 and "--source-ext" in r.stderr, r.stderr


def test_mutate_phase2_config_is_one_anchored_regex_per_scope_dir(tmp_path):
    """The run-time includePaths filter is the one thing in mutate.sh that can silently narrow
    what the runner executes (#141 review @a16d7a6, LOW): pin the generated phase-2 mull.yml
    text — exactly one regex per configured scope dir, anchored on the physical root with
    every regex metacharacter in that root escaped — without needing Mull. The clone lives
    under a directory name built from metacharacters, so an unescaped ROOT would either fail
    to match its own path or match far too much."""
    weird = tmp_path / "r(o)[o]t+x.y$z"
    weird.mkdir()
    clone = weird / "clone"
    subprocess.run(["git", "clone", "-q", "--shared", "--no-checkout", str(ROOT), str(clone)], check=True)
    subprocess.run(["git", "checkout", "-q", "HEAD"], cwd=clone, check=True)
    for rel in ("tools/mutate.sh", "tools/mutate.cfg"):
        (clone / rel).write_text((ROOT / rel).read_text())
    rc, out, _ = run(clone / "tools" / "mutate.sh", "--print-phase2-config")
    assert rc == 0, out
    physical = str(clone.resolve())
    cp = configparser.ConfigParser()
    cp.read(CFG)
    scope_dirs = cp["policy"]["scope_dirs"].split()
    lines = out.splitlines()
    assert "includePaths:" in lines, out
    paths = [line[4:] for line in lines[lines.index("includePaths:") + 1:] if line.startswith("  - ")]
    assert len(paths) == len(scope_dirs) == len(set(paths)), out
    for d, pattern in zip(scope_dirs, paths):
        assert pattern.startswith("^") and pattern.endswith(f"/{d}/.*"), pattern
        assert re.fullmatch(pattern, f"{physical}/{d}/a/b.cpp"), (pattern, physical)
        assert not re.fullmatch(pattern, f"{physical}x/{d}/a.cpp"), pattern       # anchored on the root
        assert not re.fullmatch(pattern, f"{physical}/tests/{d}/a.cpp"), pattern  # not a substring match
        assert not re.fullmatch(pattern, f"{physical}/{d}x/a.cpp"), pattern       # the dir, not a prefix
    # The other keys the runner reads: every mutator group, the pinned timeout, quiet.
    assert f"timeout: {cp['policy']['timeout_ms']}" in lines and "quiet: true" in lines, out
    for g in cp["mutators"]["groups"].split():
        assert f"  - {g}" in lines[:lines.index("includePaths:")], out


# --- tools/mutate_diff_reports.py: the evidence tool for mutate.sh changes -----------------------

DIFF_REPORTS = ROOT / "tools" / "mutate_diff_reports.py"


def _elements(tmp_path, name, files):
    p = tmp_path / f"{name}.json"
    p.write_text(json.dumps({"files": files}))
    return p


def _diff_reports(root, before, after):
    r = subprocess.run([sys.executable, str(DIFF_REPORTS), str(before), str(after), "--root", str(root),
                        "--scope-dirs", "l3 link core"], capture_output=True, text=True, cwd=ROOT)
    return r.returncode, r.stdout + r.stderr


def test_diff_reports_fails_when_it_compared_nothing(tmp_path):
    """`differences: 0` from two reports with no in-scope mutant is vacuous, not evidence
    (#141 review, MEDIUM): the tool must fail closed, exactly as mutate_report.py does."""
    root = tmp_path / "repo"
    only_tests = {str(root / "tests" / "unit" / "t.cpp"): {"mutants": [_mutant(1, 1, "cxx_add_to_sub", "Killed")]}}
    rc, out = _diff_reports(root, _elements(tmp_path, "a", only_tests), _elements(tmp_path, "b", only_tests))
    assert rc == 1 and "compared nothing" in out, out


def test_diff_reports_keys_mutants_exactly_as_the_gate_does(tmp_path):
    """Relative (`link/frame.cpp`) and root-absolute keys are the SAME mutant to
    mutate_report.py (`rel_of` + `in_scope`); a substring match on `/link/` counted neither
    form the same way (#141 review, MEDIUM). Identical sets → 0; one flipped status → listed, 1."""
    root = tmp_path / "repo"
    m = [_mutant(3, 5, "cxx_ge_to_gt", "Killed"), _mutant(9, 2, "cxx_add_to_sub", "Survived")]
    rel = {"link/frame.cpp": {"mutants": m}}
    absolute = {str(root / "link" / "frame.cpp"): {"mutants": m}}
    rc, out = _diff_reports(root, _elements(tmp_path, "a", rel), _elements(tmp_path, "b", absolute))
    assert rc == 0 and "before=2" in out and "after =2" in out and "differences: 0" in out, out
    flipped = {"link/frame.cpp": {"mutants": [_mutant(3, 5, "cxx_ge_to_gt", "Killed"),
                                              _mutant(9, 2, "cxx_add_to_sub", "Killed")]}}
    rc, out = _diff_reports(root, _elements(tmp_path, "a", rel), _elements(tmp_path, "c", flipped))
    assert rc == 1 and "link/frame.cpp:9:2 cxx_add_to_sub: Survived -> Killed" in out and "differences: 1" in out, out
    # A mutant present on one side only is a difference too (a filter that DROPPED it).
    dropped = {"link/frame.cpp": {"mutants": m[:1]}}
    rc, out = _diff_reports(root, _elements(tmp_path, "a", rel), _elements(tmp_path, "d", dropped))
    assert rc == 1 and "Survived -> -" in out, out


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
