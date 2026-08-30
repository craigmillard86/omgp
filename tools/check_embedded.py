#!/usr/bin/env python3
"""Static enforcement of CLAUDE.md rules 1, 4 and 5 for embedded-path code (l3/ link/ core/).

Findings (exit 1, one `file:line: message` per finding):
  * forbidden constructs: heap allocation, exceptions, RTTI, allocating STL containers,
    OS/time headers;
  * protocol literals: an integer literal whose value the protocol YAML also defines
    (opcode, error code, TLV type, event, module type, flag, limit, addressing or timing
    value) — such values must come from the generated header. Values below 0x10 are
    ignored (bit masks and small counts are too common to police by grep);
  * missing citation: every file under l3/ must cite a `protocol-l3 §` section
    (constitution Principle IX).
Escape hatch: `// literal-ok: <reason>` on the same line suppresses the literal check
for that line. Comments and string literals are stripped before matching.

Runs without a build (pure Python, PyYAML only) so the pipeline bootstrap path gets it.
Spec: specs/001-protocol-foundation research R-10, FR-005, SC-007.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[1]

FORBIDDEN = [
    (re.compile(r"\bnew\b(?!\s*\()"), "dynamic allocation (new)"),
    (re.compile(r"\bnew\s*\("), "dynamic allocation (placement/new-expression)"),
    (re.compile(r"\bdelete\b"), "dynamic allocation (delete)"),
    (re.compile(r"\bmalloc\s*\("), "dynamic allocation (malloc)"),
    (re.compile(r"\bcalloc\s*\("), "dynamic allocation (calloc)"),
    (re.compile(r"\brealloc\s*\("), "dynamic allocation (realloc)"),
    (re.compile(r"\bfree\s*\("), "dynamic allocation (free)"),
    (re.compile(r"\bstd::(vector|string|map|unordered_map|set|unordered_set|list|deque|"
                r"unique_ptr|shared_ptr|function)\b"), "allocating STL type"),
    (re.compile(r"\bthrow\b"), "exceptions (throw)"),
    (re.compile(r"\btry\s*\{"), "exceptions (try)"),
    (re.compile(r"\bcatch\s*\("), "exceptions (catch)"),
    (re.compile(r"\bdynamic_cast\b"), "RTTI (dynamic_cast)"),
    (re.compile(r"\btypeid\b"), "RTTI (typeid)"),
    (re.compile(r"#\s*include\s*<(string|vector|map|unordered_map|set|list|deque|memory|"
                r"functional|chrono|thread|mutex|iostream|fstream|sstream|exception|"
                r"stdexcept|typeinfo)>"), "forbidden header"),
    (re.compile(r"\bstd::chrono\b|\bsystem_clock\b|\bsteady_clock\b"), "wall-clock access"),
    (re.compile(r"\bsleep\s*\(|\busleep\s*\(|\bstd::this_thread\b"), "sleep / thread"),
]

INT_LITERAL = re.compile(r"(?<![\w.])(0[xX][0-9a-fA-F]+|\d+)(?:[uUlL]*)\b")
LITERAL_OK = re.compile(r"//\s*literal-ok\s*:")
CITATION = re.compile(r"(protocol-l3|trunk(-link-layer)?( spec)?|Spec)\s*§")


def yaml_values(yaml_path: pathlib.Path) -> dict[int, str]:
    """Every protocol value >= 0x10 the definition file fixes, mapped to its symbol(s).

    A value shared by several symbols (0x7F is OP_ERROR and ADDR_module_max) lists all
    of them, so the finding names whichever one the author meant."""
    p = yaml.safe_load(yaml_path.read_text())
    names: dict[int, list[str]] = {}

    def add(val, name):
        if isinstance(val, bool) or not isinstance(val, int):
            return
        if val >= 0x10:
            names.setdefault(val, []).append(name)

    for k, v in p.get("limits", {}).items():
        add(v, f"LIMIT_{k}")
    for k, v in p.get("addressing", {}).items():
        add(v, f"ADDR_{k}")
    for k, v in p.get("l3_flags", {}).items():
        add(v, f"FLAG_{k}")
    for k, v in p.get("opcodes", {}).items():
        add(v["code"], f"OP_{k}")
    for k, v in p.get("reserved_opcode_ranges", {}).items():
        add(v["min"], f"RESERVED_{k}_MIN")
        add(v["max"], f"RESERVED_{k}_MAX")
    for k, v in p.get("error_codes", {}).items():
        add(v, k)
    for k, v in p.get("events", {}).items():
        add(v, f"EVT_{k}")
    for k, v in p.get("tlv", {}).items():
        add(v["type"], f"TLV_{k}")
        add(v.get("max_len"), f"TLV_INFO[{k}].max_len")
    for k, v in p.get("module_types", {}).items():
        add(v, f"MT_{k}")
    for k, v in p.get("link_trunk", {}).items():
        add(v, f"TRUNK_{k}")
    for k, v in p.get("module_bus", {}).items():
        add(v, f"MBUS_{k}")
    return {val: " / ".join(syms) for val, syms in names.items()}


def strip_comments_and_strings(lines: list[str]) -> list[str]:
    """Return lines with // and /* */ comments and "..." / '...' literals blanked."""
    out = []
    in_block = False
    for line in lines:
        res = []
        i = 0
        in_str = None
        while i < len(line):
            c = line[i]
            nxt = line[i + 1] if i + 1 < len(line) else ""
            if in_block:
                if c == "*" and nxt == "/":
                    in_block = False
                    i += 2
                    continue
                i += 1
                continue
            if in_str:
                if c == "\\":
                    i += 2
                    continue
                if c == in_str:
                    in_str = None
                i += 1
                continue
            if c == "/" and nxt == "/":
                break
            if c == "/" and nxt == "*":
                in_block = True
                i += 2
                continue
            if c in ('"', "'"):
                in_str = c
                res.append(" ")
                i += 1
                continue
            res.append(c)
            i += 1
        out.append("".join(res))
    return out


def scan_file(path: pathlib.Path, values: dict[int, str], require_citation: bool) -> list[str]:
    raw = path.read_text(encoding="utf-8", errors="replace").splitlines()
    code = strip_comments_and_strings(raw)
    findings: list[str] = []
    try:
        rel = path.relative_to(ROOT)
    except ValueError: # scanning a directory outside the repo (tests use tmp dirs)
        rel = path
    for n, (line, orig) in enumerate(zip(code, raw), start=1):
        for rx, why in FORBIDDEN:
            if rx.search(line):
                findings.append(f"{rel}:{n}: {why}")
        if LITERAL_OK.search(orig):
            continue
        for m in INT_LITERAL.finditer(line):
            tok = m.group(1)
            val = int(tok, 16) if tok.lower().startswith("0x") else int(tok)
            if val in values:
                findings.append(
                    f"{rel}:{n}: protocol literal {tok} duplicates {values[val]} — use the "
                    f"generated constant (or annotate `// literal-ok: <reason>`)")
    if require_citation and not any(CITATION.search(l) for l in raw):
        findings.append(f"{rel}:1: no spec citation (expected a `protocol-l3 §…` comment)")
    return findings


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dirs", nargs="+", default=["l3", "link", "core"],
                    help="directories to scan (relative to repo root or absolute)")
    ap.add_argument("--yaml", default=str(ROOT / "protocol" / "omgp-protocol.yaml"))
    ap.add_argument("--cite-dirs", nargs="*", default=["l3", "link"],
                    help="directory basenames whose files must cite a spec section")
    args = ap.parse_args(argv)

    values = yaml_values(pathlib.Path(args.yaml))
    findings: list[str] = []
    scanned = 0
    for d in args.dirs:
        base = pathlib.Path(d) if pathlib.Path(d).is_absolute() else ROOT / d
        if not base.is_dir():
            continue
        cite = base.name in args.cite_dirs
        for f in sorted(base.rglob("*")):
            if f.suffix in (".cpp", ".hpp", ".h", ".cc"):
                scanned += 1
                findings.extend(scan_file(f, values, cite))
    for line in findings:
        print(line)
    if findings:
        print(f"check_embedded: {len(findings)} finding(s) in {scanned} file(s)")
        return 1
    print(f"check_embedded: {scanned} file(s) clean ({len(values)} protocol values policed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
