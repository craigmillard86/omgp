#!/usr/bin/env python3
"""Prove that every tests/{unit,property}/test_*.cpp was compiled, registered and executed.

`pipeline.sh` `stage_unit` (ctest path) calls this after `ctest --preset native
--output-junit Testing/junit.xml` and after summing the floor. The floor
(`UNIT_TEST_FLOOR`) catches a mass loss of assertions; it cannot see ONE source dropping
out while the rest keep the sum above it (#133). This check reads the build artefacts and
the run's own machine-readable record, and names every source that escaped:

  compiled    compile_commands.json has an entry whose `file` is the source; its `-o`
              object (relative to the entry's `directory`) exists; the object path names
              the target: CMakeFiles/<target>.dir/...  (a CMakeLists line re-pointed at
              another file leaves the source with NO entry — "compiled by no target").
  registered  `ctest --show-only=json-v1` in the build dir lists a test whose command is
              EXACTLY [<build>/<target>] — the target's own binary, by path (a same-named
              binary elsewhere is a decoy: red team @94f2462 finding 2), with no arguments
              (an argument is a Catch2 filter; a spec matching nothing plus
              --allow-running-no-tests exits 0 with `EXECUTED: 0` — @94f2462 finding 1).
              CMakeLists registers `COMMAND ${name}` bare, and the binary lands at
              <build>/<target> (no RUNTIME_OUTPUT_DIRECTORY): a layout change fails LOUDLY
              here, for every source, rather than silently.
  executed    this run's Testing/junit.xml has a <testcase> for that registered name with
              status="run" whose <system-out> carries an `EXECUTED: <n>` line with n > 0.
              A DISABLED test is status="disabled" with system-out "Disabled"; a
              SKIP_RETURN_CODE skip is status="notrun" (measured, CMake 3.22).

Why the JUnit record and not Testing/Temporary/LastTest.log (PR #172 red team): the log
interleaves ctest's framing (`N/M Testing:`, `Command:`) with the tests' own stdout, so a
test binary could print the framing and mint an execution for a test that never ran;
`Command:` quotes the whole argv, so an argument on add_test broke the match; and the log
is whatever bytes the tests wrote. In the JUnit record the framing is XML structure the
tests cannot produce — their stdout is escaped text inside <system-out> (by
construction of the writer) — names come from ctest, and non-UTF-8 bytes are replaced
by ctest before writing.

Freshness: a registered binary newer than the record means the record is from an earlier
run — refused. The record itself is deleted by stage_unit before ctest runs (a ctest that
fails before running — e.g. a CTestTestfile syntax error, exit 8 — writes none), so a
missing record means ctest did not write one this run — also refused. Both are controls on the
current pipeline, not guarantees: an out-of-band `ctest` after a rebuild is refused only
by the mtime comparison.

No sources at all is a failure, not a vacuous pass (review @7dbf9d8).

Matching is by BINARY (basename of the target / command[0]), not by test name:
CMakeLists registers test_smoke as `smoke`.

Order matters: `ctest --show-only` REWRITES LastTest.log (measured on CMake 3.22: the log
holds zero EXECUTED lines afterwards), so the log is read (as bytes) first and written
back unchanged afterwards. Pure Python 3, standard library only; runs on every CI path.

Exit 0 and one line `unit: verified N test binaries (compiled, registered, executed;
ctest path)` when every source passes all three; otherwise one
`unit: <source>: <reason>` line per failing source on stderr, exit 1.
"""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

SOURCE_DIRS = ("tests/unit", "tests/property")
TARGET_RE = re.compile(r"CMakeFiles/([^/]+)\.dir/")
EXECUTED_RE = re.compile(r"^EXECUTED: (\d+)$")


def sources(root: Path):
    return sorted(p for d in SOURCE_DIRS for p in (root / d).glob("test_*.cpp"))


def compiled_targets(build: Path):
    """realpath(source) -> (target, object path) from compile_commands.json."""
    out = {}
    for entry in json.loads((build / "compile_commands.json").read_text()):
        argv = shlex.split(entry["command"])
        if "-o" not in argv:
            continue
        obj = Path(entry["directory"]) / argv[argv.index("-o") + 1]
        m = TARGET_RE.search(obj.as_posix())
        if m:
            out[os.path.realpath(entry["file"])] = (m.group(1), obj)
    return out


def registered_tests(build: Path):
    """realpath(command[0]) -> [(ctest test name, command argv)] from `ctest --show-only=json-v1`."""
    out = subprocess.run(["ctest", "--show-only=json-v1"], cwd=build,
                         capture_output=True, text=True, check=True).stdout
    by_binary = {}
    for t in json.loads(out)["tests"]:
        if t.get("command"):
            by_binary.setdefault(os.path.realpath(t["command"][0]), []).append((t["name"], t["command"]))
    return by_binary


def executed_counts(junit: Path):
    """ctest test name -> the EXECUTED: count it printed, for <testcase>s with status="run"."""
    out = {}
    for case in ET.parse(junit).getroot().iter("testcase"):
        if case.get("status") != "run":
            continue
        for line in (case.findtext("system-out") or "").splitlines():
            m = EXECUTED_RE.match(line)
            if m:
                out[case.get("name")] = int(m.group(1))
    return out


def check_ctest(root: Path, build: Path, log: Path, junit: Path) -> int:
    srcs = sources(root)
    if not srcs:
        print(f"unit: no test sources under {', '.join(SOURCE_DIRS)} in {root} (nothing to verify is a failure, not a pass)",
              file=sys.stderr)
        return 1
    if not junit.exists():
        print(f"unit: no execution record at {junit} (ctest --output-junit did not write one this run)", file=sys.stderr)
        return 1
    log_bytes = log.read_bytes() if log.exists() else None   # BEFORE show-only, which rewrites it
    compiled = compiled_targets(build)
    try:
        registered = registered_tests(build)
    finally:
        if log_bytes is not None:
            log.write_bytes(log_bytes)   # hand the run's log back exactly as ctest left it
    executed = executed_counts(junit)
    record_mtime = junit.stat().st_mtime
    by_basename = {}   # for the "same name, wrong path" message only
    for path in registered:
        by_basename.setdefault(os.path.basename(path), []).append(path)
    failures = []
    for src in srcs:
        rel = src.relative_to(root).as_posix()
        hit = compiled.get(os.path.realpath(src))
        if hit is None:
            failures.append(f"{rel}: compiled by no target (no compile_commands.json entry; CMakeLists re-pointed or missing)")
            continue
        target, obj = hit
        if not obj.exists():
            failures.append(f"{rel}: object {obj} missing (target {target} not built)")
            continue
        binary = os.path.realpath(build / target)
        if binary not in registered:
            decoys = [d for d in by_basename.get(target, []) if d != binary]
            if decoys:
                failures.append(f"{rel}: built as {target} but the only registration named {target} runs {decoys[0]}, "
                                f"which is not this target's binary {binary} (no add_test for it)")
            else:
                failures.append(f"{rel}: built as {target} but not registered with ctest (no add_test)")
            continue
        name, argv = registered[binary][0]
        if len(argv) > 1:
            failures.append(f"{rel}: registered as '{name}' with argument(s) {argv[1:]} — a filtered run is not "
                            f"execution of the source; register the whole binary (add_test(NAME {target} COMMAND {target}))")
        elif os.path.getmtime(binary) > record_mtime:
            failures.append(f"{rel}: {binary} is newer than {junit} (the execution record predates this binary; rerun ctest)")
        elif name not in executed:
            failures.append(f"{rel}: registered as {target} (ctest test '{name}') but did not run in this ctest run (DISABLED, skipped, or no EXECUTED line in {junit})")
        elif executed[name] == 0:
            failures.append(f"{rel}: registered as {target} (ctest test '{name}') ran but reported EXECUTED: 0 — no test case executed")
    if failures:
        for f in failures:
            print(f"unit: {f}", file=sys.stderr)
        return 1
    n = len(srcs)
    print(f"unit: verified {n} test binar{'y' if n == 1 else 'ies'} (compiled, registered, executed; ctest path)")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ctest", action="store_true", required=True,
                    help="check the cmake/ctest build at --build against this run's record")
    ap.add_argument("--root", type=Path, default=Path.cwd(), help="repo root (tests/ lives here)")
    ap.add_argument("--build", type=Path, help="cmake build dir (default: ROOT/build/native)")
    ap.add_argument("--log", type=Path, help="ctest log to preserve (default: BUILD/Testing/Temporary/LastTest.log)")
    ap.add_argument("--junit", type=Path, help="this run's `ctest --output-junit` record (default: BUILD/Testing/junit.xml)")
    a = ap.parse_args(argv)
    root = a.root.resolve()
    build = (a.build or root / "build" / "native").resolve()
    log = (a.log or build / "Testing" / "Temporary" / "LastTest.log").resolve()
    junit = (a.junit or build / "Testing" / "junit.xml").resolve()
    return check_ctest(root, build, log, junit)


if __name__ == "__main__":
    sys.exit(main())
