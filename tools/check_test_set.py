#!/usr/bin/env python3
"""Prove that every tests/{unit,property}/test_*.cpp was compiled, registered and executed.

`pipeline.sh` `stage_unit` (ctest path) calls this after `ctest --preset native` and after
summing the floor. The floor (`UNIT_TEST_FLOOR`) catches a mass loss of assertions; it
cannot see ONE source dropping out while the rest keep the sum above it (#133). This
check reads the build artefacts and the run's own log, and names the first escape:

  compiled    compile_commands.json has an entry whose `file` is the source; its `-o`
              object (relative to the entry's `directory`) exists; the object path names
              the target: CMakeFiles/<target>.dir/...  (a CMakeLists line re-pointed at
              another file leaves the source with NO entry — "compiled by no target").
  registered  `ctest --show-only=json-v1` in the build dir lists a test whose command is
              that target's binary (a missing add_test leaves the binary unregistered).
  executed    this run's Testing/Temporary/LastTest.log has a per-test block whose
              `Command:` is that binary AND which carries an `EXECUTED:` line. A DISABLED
              test's block has `Command: ""`, no EXECUTED line, and still says
              "Test Passed." — so the block must be matched by command, not by name.

Matching is by BINARY (basename of the target / the ctest command), not by test name:
CMakeLists registers test_smoke as `smoke`.

Order matters: `ctest --show-only` REWRITES LastTest.log (measured on CMake 3.22: the log
holds zero EXECUTED lines afterwards), so the log is read first and written back
unchanged afterwards. Pure Python 3, standard library only; runs on every CI path.

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
from pathlib import Path

SOURCE_DIRS = ("tests/unit", "tests/property")
TARGET_RE = re.compile(r"CMakeFiles/([^/]+)\.dir/")
TEST_START_RE = re.compile(r"^\d+/\d+ Testing: (.*)$")
COMMAND_RE = re.compile(r'^Command: "(.*)"$')


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


def registered_binaries(build: Path):
    """basename of every registered test's command (ctest --show-only=json-v1)."""
    out = subprocess.run(["ctest", "--show-only=json-v1"], cwd=build,
                         capture_output=True, text=True, check=True).stdout
    return {os.path.basename(t["command"][0]) for t in json.loads(out)["tests"] if t.get("command")}


def executed_binaries(log_text: str):
    """basename of every binary whose log block ran and printed an EXECUTED: line."""
    out, command, executed = set(), "", False

    def close():
        if command and executed:
            out.add(os.path.basename(command))

    for line in log_text.splitlines():
        if TEST_START_RE.match(line):
            close()
            command, executed = "", False
        elif (m := COMMAND_RE.match(line)):
            command = m.group(1)
        elif line.startswith("EXECUTED: "):
            executed = True
    close()
    return out


def check_ctest(root: Path, build: Path, log: Path) -> int:
    log_text = log.read_text() if log.exists() else ""   # BEFORE show-only, which rewrites it
    compiled = compiled_targets(build)
    try:
        registered = registered_binaries(build)
    finally:
        if log.exists():
            log.write_text(log_text)   # hand the run's log back exactly as ctest left it
    executed = executed_binaries(log_text)
    failures = []
    for src in sources(root):
        rel = src.relative_to(root).as_posix()
        hit = compiled.get(os.path.realpath(src))
        if hit is None:
            failures.append(f"{rel}: compiled by no target (no compile_commands.json entry; CMakeLists re-pointed or missing)")
            continue
        target, obj = hit
        if not obj.exists():
            failures.append(f"{rel}: object {obj} missing (target {target} not built)")
        elif target not in registered:
            failures.append(f"{rel}: built as {target} but not registered with ctest (no add_test)")
        elif target not in executed:
            failures.append(f"{rel}: registered as {target} but did not run in this ctest run (DISABLED, filtered, or no EXECUTED line in {log})")
    if failures:
        for f in failures:
            print(f"unit: {f}", file=sys.stderr)
        return 1
    n = len(sources(root))
    print(f"unit: verified {n} test binar{'y' if n == 1 else 'ies'} (compiled, registered, executed; ctest path)")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ctest", action="store_true", required=True,
                    help="check the cmake/ctest build at --build against this run's log")
    ap.add_argument("--root", type=Path, default=Path.cwd(), help="repo root (tests/ lives here)")
    ap.add_argument("--build", type=Path, help="cmake build dir (default: ROOT/build/native)")
    ap.add_argument("--log", type=Path, help="ctest log (default: BUILD/Testing/Temporary/LastTest.log)")
    a = ap.parse_args(argv)
    root = a.root.resolve()
    build = (a.build or root / "build" / "native").resolve()
    log = (a.log or build / "Testing" / "Temporary" / "LastTest.log").resolve()
    return check_ctest(root, build, log)


if __name__ == "__main__":
    sys.exit(main())
