#!/usr/bin/env python3
"""Prove that every test_*.cpp under tests/{unit,property} was compiled, registered and executed.

`pipeline.sh` `stage_unit` (ctest path) calls `--snapshot` BEFORE `ctest --preset native
--output-junit Testing/junit.xml` and `--ctest --pre -` after it and after summing the floor. The floor
(`UNIT_TEST_FLOOR`) catches a mass loss of assertions; it cannot see ONE source dropping
out while the rest keep the sum above it (#133). This check reads the build artefacts and
the run's own machine-readable record, and names every source that escaped:

  compiled    compile_commands.json has an entry whose `file` is the source; its `-o`
              object (relative to the entry's `directory`) exists; the object path names
              the target: CMakeFiles/<target>.dir/...  (a CMakeLists line re-pointed at
              another file leaves the source with NO entry — "compiled by no target").
              The object must be at least as new as the source: an edited-after-build
              source is named ("source is newer than its object; rebuild") rather than
              vouched for by a stale object (red team @ceab86f finding 3). An mtime
              comparison is a control, not a guarantee (clock skew, touch).
  registered  `ctest --show-only=json-v1` in the build dir lists a test whose command is
              EXACTLY [<build>/<target>] — the target's own binary, compared as an absolute
              path WITHOUT resolving symlinks: build/<other> -> build/<target> is not a
              registration of <other> (@ceab86f finding 5) — and the FILE the registration
              runs must be registered once: two registrations whose command[0] are one file
              (a link registered by its own path; @3dde163 finding 3) are both refused,
              since one file cannot be two targets' binaries — and so is the same path
              registered twice (@2f40596 lead a: only the first registration was read,
              so a second, filtered one escaped the no-argument rule). Likewise one NAME, one
              registration: execution evidence is keyed by ctest test name, so a name
              shared by two registrations would hand one binary's run to the other
              (@aed9693; CMake permits it across add_subdirectory) — both are refused.
              And the registration set must be the set that ran: `ctest --show-only` is
              read AFTER the tests, so its names must equal the record's <testcase> names
              (any status); a test that rewrote CTestTestfile.cmake while running is
              refused as "registration set differs from the run's record" (@aed9693,
              reproducer 2). A same-named binary elsewhere
              is a decoy (red team @94f2462 finding 2). No arguments
              (an argument is a Catch2 filter; a spec matching nothing plus
              --allow-running-no-tests exits 0 with `EXECUTED: 0` — @94f2462 finding 1).
              CMakeLists registers `COMMAND ${name}` bare, and the binary lands at
              <build>/<target> (no RUNTIME_OUTPUT_DIRECTORY): a layout change fails LOUDLY
              here, for every source, rather than silently.
  executed    this run's Testing/junit.xml has a <testcase> for that registered name with
              status="run" whose <system-out> carries an `EXECUTED: <n>` line with n > 0.
              That evidence is PER BINARY: it shows the binary containing the source's
              object ran and executed something, not that any particular source's cases
              did (a source whose TEST_CASEs are all hidden or deleted contributes zero to
              a binary that still reports n > 0). One source per target in CMakeLists today
              makes the two coincide — a control on the build files, not a guarantee.
              A DISABLED test is status="disabled" with system-out "Disabled"; a
              SKIP_RETURN_CODE skip is status="notrun" (measured, CMake 3.22). ctest keeps
              only --test-output-size-passed bytes (default 1024) of a passing test's stdout
              in the record, dropping the TAIL — where EXECUTED: is — and appends a marker
              line (red team @3880d35); stage_unit raises the size, and a record that
              carries the marker for a test with no EXECUTED line is named as truncated.

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

Unchanged during the run (red team @f8bce32): every artefact above is read AFTER the tests,
and the tests are programs that can write under build/ — a test that appended a
compile_commands.json entry and a stub object for a source no target built was verified on
the target that ran (escape 2, green). So stage_unit takes `--snapshot` first: sha256 of
compile_commands.json, the identity (dev, ino, size, mtime_ns, ctime_ns) of every source's
object and of every registered binary, and the registration list (name, argv). `--ctest --pre`
refuses what differs: "compile_commands.json changed during the ctest run", "object … did
not exist before the ctest run" / "changed during the ctest run", "registration set changed
during the ctest run" (this also closes the rewrite-that-keeps-the-name-set residual the
record comparison left), "<binary> changed during the ctest run"; and — red team @2f40596,
reproducer A — the SOURCE SET itself, with each source's identity: the check walks
sources() after ctest, so a built-but-never-registered source was verified away by a test
that deleted the file mid-run ("source set changed during the ctest run", up front;
"source changed during the ctest run" per source). The snapshot is held in
the stage's shell variable and handed over on stdin, so a running test cannot rewrite it
too. Identity by stat, not content: a rewrite in place changes ctime, which utime cannot
put back — a control (root, or a clock moved), not a guarantee. What the snapshot cannot
see: a test that reaches the shell's memory, or rewrites this tool or pipeline.sh while
running — stated, not covered; those files are reviewed code.

No sources at all is a failure, not a vacuous pass (review @7dbf9d8).

Matching is by BINARY (basename of the target / command[0]), not by test name:
CMakeLists registers test_smoke as `smoke`.

The source set is every test_*.cpp under tests/unit and tests/property at ANY depth
(rglob), the same set pipeline.sh's unit_sources() walks; a flat glob would leave
tests/unit/<sub>/test_x.cpp unchecked and unnamed (@ceab86f finding 4). A source that is
a symlink to another source is verified by the file it resolves to (@2f40596 lead b): its
own cases are whatever that file holds, which is the stated per-binary limit above (the
same as deleting a source's TEST_CASEs) — a visible tree change, not a rule here.

Order matters: `ctest --show-only` REWRITES LastTest.log (measured on CMake 3.22: the log
holds zero EXECUTED lines afterwards), so the log is read (as bytes) first and written
back unchanged afterwards. Pure Python 3, standard library only; runs on every CI path.

Exit 0 and one line `unit: verified N test binaries (compiled, registered, executed;
ctest path)` — N counts distinct binaries, so a multi-source target counts once (review
@ceab86f, LOW) — when every source passes all three; otherwise one
`unit: <source>: <reason>` line per failing source on stderr, exit 1.
"""
import argparse
import hashlib
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
# ctest's marker for a <system-out> it cut short (CMake 3.22 wording; a control, not a guarantee).
TRUNCATED_RE = re.compile(r"test output was removed since it exceeds the threshold")


def sources(root: Path):
    return sorted(p for d in SOURCE_DIRS for p in (root / d).rglob("test_*.cpp") if p.is_file())


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
    """abspath(command[0]) -> [(ctest test name, command argv)] from `ctest --show-only=json-v1`.

    Absolute, normalised, symlinks NOT resolved: the key is the path ctest will exec, and
    two paths that resolve to one file are two registrations of one binary, not of two."""
    out = subprocess.run(["ctest", "--show-only=json-v1"], cwd=build,
                         capture_output=True, text=True, check=True).stdout
    by_binary = {}
    for t in json.loads(out)["tests"]:
        if t.get("command"):
            by_binary.setdefault(os.path.abspath(t["command"][0]), []).append((t["name"], t["command"]))
    return by_binary


def executed_counts(junit: Path):
    """(ctest test name -> the EXECUTED: count it printed, for <testcase>s with status="run";
    set of names whose recorded stdout ctest truncated; list of every <testcase> name in the
    record, any status — the set ctest actually saw, duplicates included)."""
    out, truncated, recorded = {}, set(), []
    for case in ET.parse(junit).getroot().iter("testcase"):
        recorded.append(case.get("name"))
        if case.get("status") != "run":
            continue
        text = case.findtext("system-out") or ""
        if TRUNCATED_RE.search(text):
            truncated.add(case.get("name"))
        for line in text.splitlines():
            m = EXECUTED_RE.match(line)
            if m:
                out[case.get("name")] = int(m.group(1))
    return out, truncated, recorded


def identity(path) -> list:
    """What a file is, for the unchanged-during-the-run comparison: (dev, ino, size, mtime_ns,
    ctime_ns), or None if absent. A rewrite in place changes size/mtime/ctime; a replacement
    changes ino; `touch`/utime cannot put ctime back — a control (root, or a clock moved), not
    a guarantee. Contents are not hashed: 17 sanitizer objects per run is a cost for nothing
    the stat does not already show."""
    try:
        st = os.stat(path)
    except OSError:
        return None
    return [st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns]


def read_artefacts(build: Path, log: Path):
    """compiled_targets + registered_tests, with LastTest.log handed back as ctest left it."""
    log_bytes = log.read_bytes() if log.exists() else None   # BEFORE show-only, which rewrites it
    compiled = compiled_targets(build)
    try:
        registered = registered_tests(build)
    finally:
        if log_bytes is not None:
            log.write_bytes(log_bytes)
    return compiled, registered


def registration_list(registered) -> list:
    return sorted([name, argv] for regs in registered.values() for name, argv in regs)


def snapshot(root: Path, build: Path, log: Path) -> dict:
    """The pre-run fingerprint stage_unit takes BEFORE ctest and hands to --ctest --pre: the
    source set (each source's identity), the compilation database (hashed), every source's
    object, the registration set (name, argv) and every registered binary. Anything that
    differs after the run was written while the tests ran (or after them) and is not
    evidence about the run (red team @f8bce32; the sources: @2f40596)."""
    compiled, registered = read_artefacts(build, log)
    srcs, objects = {}, {}
    for src in sources(root):
        srcs[src.relative_to(root).as_posix()] = identity(src)
        hit = compiled.get(os.path.realpath(src))
        if hit is not None:
            objects[str(hit[1])] = identity(hit[1])
    return {
        "sources": srcs,
        "compile_commands": hashlib.sha256((build / "compile_commands.json").read_bytes()).hexdigest(),
        "objects": objects,
        "registered": registration_list(registered),
        "binaries": {path: identity(path) for path in registered},
    }


def check_ctest(root: Path, build: Path, log: Path, junit: Path, pre: dict) -> int:
    srcs = sources(root)
    if not srcs:
        print(f"unit: no test sources under {', '.join(SOURCE_DIRS)} in {root} (nothing to verify is a failure, not a pass)",
              file=sys.stderr)
        return 1
    if not junit.exists():
        print(f"unit: no execution record at {junit} (ctest --output-junit did not write one this run)", file=sys.stderr)
        return 1
    compiled, registered = read_artefacts(build, log)
    executed, truncated, recorded = executed_counts(junit)
    record_mtime = junit.stat().st_mtime
    failures = []
    # Unchanged since the pre-run snapshot (red team @f8bce32; the source set @2f40596): the
    # source and registration sets are compared here, the sources, objects and binaries per
    # source below. The set first: a source the tree lost during the run is not in srcs, so
    # no per-source line could ever name it.
    now_srcs = sorted(src.relative_to(root).as_posix() for src in srcs)
    if now_srcs != sorted(pre["sources"]):
        lost = sorted(set(pre["sources"]) - set(now_srcs))
        gained = sorted(set(now_srcs) - set(pre["sources"]))
        failures.append(f"source set changed during the ctest run (in the tree before the run and not after: {lost}; "
                        f"after and not before: {gained}) — a test deleted or added a test source? rerun via stage_unit")
    if hashlib.sha256((build / "compile_commands.json").read_bytes()).hexdigest() != pre["compile_commands"]:
        failures.append("compile_commands.json changed during the ctest run (differs from the pre-run snapshot) — "
                        "a test wrote it? rebuild and rerun via stage_unit")
    now = registration_list(registered)
    if now != pre["registered"]:
        before = [f"{n} -> {' '.join(a)}" for n, a in pre["registered"] if [n, a] not in now]
        after = [f"{n} -> {' '.join(a)}" for n, a in now if [n, a] not in pre["registered"]]
        failures.append(f"registration set changed during the ctest run (before the run and not after: {before}; "
                        f"after and not before: {after}) — a test rewrote CTestTestfile.cmake? rerun via stage_unit")
    by_name = {}   # ctest test name -> registered paths carrying it (one name, one registration)
    for path, regs in registered.items():
        for name, _ in regs:
            by_name.setdefault(name, []).append(path)
    if sorted(by_name) != sorted(set(recorded)) or len(recorded) != len(set(recorded)):
        missing = sorted(set(by_name) - set(recorded))
        extra = sorted(set(recorded) - set(by_name))
        dups = sorted({n for n in recorded if recorded.count(n) > 1})
        failures.append("registration set differs from the run's record "
                        f"(registered but not in {junit}: {missing}; in the record but not registered: {extra}; "
                        f"recorded more than once: {dups}) — CTestTestfile.cmake changed after ctest ran?")
    inode = {}   # registered path -> (st_dev, st_ino) of the file it runs, for the one-file-one-registration rule
    for path in registered:
        try:
            st = os.stat(path)
        except OSError:
            continue
        inode[path] = (st.st_dev, st.st_ino)
    by_basename = {}   # for the "same name, wrong path" message only
    for path in registered:
        by_basename.setdefault(os.path.basename(path), []).append(path)
    for src in srcs:
        rel = src.relative_to(root).as_posix()
        if rel in pre["sources"] and pre["sources"][rel] != identity(src):
            failures.append(f"{rel}: source changed during the ctest run (not the file its object was built from); "
                            f"rebuild and rerun via stage_unit")
            continue
        hit = compiled.get(os.path.realpath(src))
        if hit is None:
            failures.append(f"{rel}: compiled by no target (no compile_commands.json entry; CMakeLists re-pointed or missing)")
            continue
        target, obj = hit
        if not obj.exists():
            failures.append(f"{rel}: object {obj} missing (target {target} not built)")
            continue
        if src.stat().st_mtime > obj.stat().st_mtime:
            failures.append(f"{rel}: source is newer than its object {obj} (target {target}); rebuild before verifying")
            continue
        if pre["objects"].get(str(obj)) is None:
            failures.append(f"{rel}: object {obj} did not exist before the ctest run (written during or after the tests — "
                            f"not evidence that the binary that ran contains it); rebuild and rerun via stage_unit")
            continue
        if pre["objects"][str(obj)] != identity(obj):
            failures.append(f"{rel}: object {obj} changed during the ctest run; rebuild and rerun via stage_unit")
            continue
        binary = os.path.abspath(build / target)
        if binary not in registered:
            real = os.path.realpath(binary)
            decoys = [d for d in by_basename.get(target, []) if d != binary]
            aliases = [d for d in registered if os.path.realpath(d) == real]
            if aliases:
                failures.append(f"{rel}: built as {target} but not registered with ctest: the only registration reaching "
                                f"this file runs {aliases[0]}, not {binary} (a link is not an add_test for {target})")
            elif decoys:
                failures.append(f"{rel}: built as {target} but the only registration named {target} runs {decoys[0]}, "
                                f"which is not this target's binary {binary} (no add_test for it)")
            else:
                failures.append(f"{rel}: built as {target} but not registered with ctest (no add_test)")
            continue
        name, argv = registered[binary][0]
        twins = [d for d, i in inode.items() if d != binary and i == inode.get(binary)]
        namesakes = [d for d in by_name.get(name, []) if d != binary]
        if namesakes:
            failures.append(f"{rel}: registered as '{name}', but one name cannot identify two binaries' runs: "
                            f"'{name}' also registers {namesakes[0]} (execution evidence is per name)")
        elif len(registered[binary]) > 1:
            # The same path spelled twice is the twins rule's case with no link to resolve
            # (red team @2f40596, lead a): only the first registration was examined, so a
            # second, filtered one escaped the no-argument rule below.
            others = [n for n, _ in registered[binary][1:]]
            failures.append(f"{rel}: {binary} is registered more than once ('{name}' and {others}): one file, one "
                            f"registration — a second run of the same binary is not evidence about a second target")
        elif twins:
            failures.append(f"{rel}: registered as '{name}', but the file {binary} runs is also registered as "
                            f"'{registered[twins[0]][0][0]}' ({twins[0]}): one file cannot be two targets' binaries, "
                            f"so neither registration is a run of its own target")
        elif pre["binaries"].get(binary) != identity(binary):
            failures.append(f"{rel}: {binary} changed during the ctest run (the file that ran is not the file the build "
                            f"produced — overwritten by a test?); rebuild and rerun via stage_unit")
        elif len(argv) > 1:
            failures.append(f"{rel}: registered as '{name}' with argument(s) {argv[1:]} — a filtered run is not "
                            f"execution of the source; register the whole binary (add_test(NAME {target} COMMAND {target}))")
        elif os.path.getmtime(binary) > record_mtime:
            failures.append(f"{rel}: {binary} is newer than {junit} (the execution record predates this binary; rerun ctest)")
        elif name not in executed and name in truncated:
            failures.append(f"{rel}: registered as {target} (ctest test '{name}') ran, but ctest truncated its recorded output "
                            f"before the EXECUTED line (--test-output-size-passed too small for this run of {junit}; rerun via stage_unit)")
        elif name not in executed:
            failures.append(f"{rel}: registered as {target} (ctest test '{name}') but did not run in this ctest run (DISABLED, skipped, or no EXECUTED line in {junit})")
        elif executed[name] == 0:
            failures.append(f"{rel}: registered as {target} (ctest test '{name}') ran but reported EXECUTED: 0 — no test case executed")
    if failures:
        for f in failures:
            print(f"unit: {f}", file=sys.stderr)
        return 1
    n = len({compiled[os.path.realpath(src)][0] for src in srcs})   # distinct targets, not sources
    print(f"unit: verified {n} test binar{'y' if n == 1 else 'ies'} (compiled, registered, executed; ctest path)")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--ctest", action="store_true",
                      help="check the cmake/ctest build at --build against this run's record and the --pre snapshot")
    mode.add_argument("--snapshot", action="store_true",
                      help="print the pre-run snapshot (JSON) of the build artefacts for a later --ctest --pre")
    ap.add_argument("--pre", type=Path,
                    help="the --snapshot taken before ctest ran ('-' for stdin); required with --ctest")
    ap.add_argument("--root", type=Path, default=Path.cwd(), help="repo root (tests/ lives here)")
    ap.add_argument("--build", type=Path, help="cmake build dir (default: ROOT/build/native)")
    ap.add_argument("--log", type=Path, help="ctest log to preserve (default: BUILD/Testing/Temporary/LastTest.log)")
    ap.add_argument("--junit", type=Path, help="this run's `ctest --output-junit` record (default: BUILD/Testing/junit.xml)")
    a = ap.parse_args(argv)
    root = a.root.resolve()
    build = (a.build or root / "build" / "native").resolve()
    log = (a.log or build / "Testing" / "Temporary" / "LastTest.log").resolve()
    junit = (a.junit or build / "Testing" / "junit.xml").resolve()
    if a.snapshot:
        json.dump(snapshot(root, build, log), sys.stdout)
        return 0
    if a.pre is None:
        ap.error("--ctest needs --pre <snapshot taken before ctest ran> (stage_unit takes it with --snapshot)")
    try:
        pre = json.load(sys.stdin if str(a.pre) == "-" else a.pre.open())
        for key in ("sources", "compile_commands", "objects", "registered", "binaries"):
            pre[key]
    except (OSError, ValueError, KeyError, TypeError) as e:
        print(f"unit: no usable pre-run snapshot at {a.pre} ({e}); rerun via stage_unit", file=sys.stderr)
        return 1
    return check_ctest(root, build, log, junit, pre)


if __name__ == "__main__":
    sys.exit(main())
