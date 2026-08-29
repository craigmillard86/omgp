#!/usr/bin/env python3
"""Generate the protocol constants from protocol/omgp-protocol.yaml (CLAUDE.md rule 1).

Outputs (default build/gen/): omgp_protocol.h (C++17), omgp_protocol.py, omgp_vectors.h
(from tests/vectors/*.json). Deterministic by construction: every mapping is iterated in
sorted key order, integers are rendered in one fixed style, and nothing volatile
(timestamps, paths, versions) is emitted — same YAML in, byte-identical files out.

    python3 tools/codegen.py [--yaml F] [--out D] [--vectors D] [--check] [--check-docs [--docs F]]

Exit 0 ok | 1 drift (--check / --check-docs) | 2 YAML validation error (names the conflict).
Contract: specs/001-protocol-foundation/contracts/generated-constants.md
"""
from __future__ import annotations

import argparse
import difflib
import json
import pathlib
import re
import sys

import yaml
from jinja2 import Environment, FileSystemLoader, StrictUndefined, select_autoescape

ROOT = pathlib.Path(__file__).resolve().parents[1]
TEMPLATES = ROOT / "protocol" / "templates"
OUTPUTS = ("omgp_protocol.h", "omgp_protocol.py", "omgp_vectors.h", "omgp_names.h")

FIELD_WIDTH = {"u8": 1, "u16": 2}
FIELD_TYPES = {"u8", "u16", "bytes"}
TARGETS = {"any": "Any", "module": "Module", "backplane": "Backplane",
           "response_only": "ResponseOnly"}
# section -> (symbol prefix, C++ type for ints). Order here is the output order.
SCALAR_SECTIONS = [
    ("limits", "LIMIT_", "uint32_t"),
    ("addressing", "ADDR_", "uint8_t"),
    ("l3_flags", "FLAG_", "uint8_t"),
    ("opcodes", "OP_", "uint8_t"),
    ("error_codes", "", "uint8_t"),
    ("node_states", "STATE_", "uint8_t"),
    ("events", "EVT_", "uint8_t"),
    ("param_kinds", "KIND_", "uint8_t"),
    ("tlv", "TLV_", "uint8_t"),
    ("module_types", "MT_", "uint8_t"),
    ("link_trunk", "TRUNK_", "uint32_t"),
    ("module_bus", "MBUS_", "uint32_t"),
]
BYTE_SECTIONS = {"addressing", "l3_flags", "opcodes", "error_codes", "node_states", "events",
                 "param_kinds", "tlv", "module_types"}
NAME_MAPS = [  # (python names map, C++ codes array, section, value-of).
    # Plural NAMES because TLV_NAME is already the NAME record's type constant.
    ("OPCODE_NAMES", "OPCODE_CODES", "opcodes", lambda v: v["code"]),
    ("TLV_NAMES", "TLV_CODES", "tlv", lambda v: v["type"]),
    ("ERROR_NAMES", "ERROR_CODES", "error_codes", lambda v: v),
    ("EVENT_NAMES", "EVENT_CODES", "events", lambda v: v),
    ("MODULE_TYPE_NAMES", "MODULE_TYPE_CODES", "module_types", lambda v: v),
    ("STATE_NAMES", "NODE_STATE_CODES", "node_states", lambda v: v),
    ("KIND_NAMES", "PARAM_KIND_CODES", "param_kinds", lambda v: v),
]


class ProtocolError(Exception):
    """The definition file is internally inconsistent (spec 001 FR-004)."""


# --- validation --------------------------------------------------------------------------

def _scalar_value(section: str, v):
    if section == "opcodes":
        return v["code"]
    if section == "tlv":
        return v["type"]
    return v


def validate(p: dict) -> None:
    def unique(section, value_of):
        seen: dict = {}
        for k, v in p[section].items():
            val = value_of(v)
            if val in seen:
                raise ProtocolError(f"{section}: {k} and {seen[val]} share value {val:#04x}")
            seen[val] = k

    unique("opcodes", lambda v: v["code"])
    unique("tlv", lambda v: v["type"])
    for s in ("error_codes", "events", "module_types", "node_states", "param_kinds"):
        unique(s, lambda v: v)
    for section in BYTE_SECTIONS:
        for k, v in p[section].items():
            val = _scalar_value(section, v)
            if not isinstance(val, int) or isinstance(val, bool) or not 0 <= val <= 0xFF:
                raise ProtocolError(f"{section}: {k} = {val!r} does not fit a byte")
    for k, v in p["opcodes"].items():
        if v.get("target") not in TARGETS:
            raise ProtocolError(f"opcodes: {k} has unknown target {v.get('target')!r} "
                                f"(expected one of {sorted(TARGETS)})")
    for k, v in p["tlv"].items():
        if v.get("max_len", 0) > 0xFF:
            raise ProtocolError(f"tlv: {k} max_len {v['max_len']} exceeds 255")
    for rname, r in p.get("reserved_opcode_ranges", {}).items():
        for k, v in p["opcodes"].items():
            if r["min"] <= v["code"] <= r["max"]:
                raise ProtocolError(f"opcodes: {k} ({v['code']:#04x}) lies inside reserved "
                                    f"range {rname} [{r['min']:#04x}, {r['max']:#04x}]")
    payloads = p.get("l3_payloads", {})
    for k in payloads:
        if k not in p["opcodes"]:
            raise ProtocolError(f"l3_payloads: {k} is not an opcode")
    for k in p["opcodes"]:
        if k not in payloads:
            raise ProtocolError(f"l3_payloads: no entry for opcode {k}")
    for k, layout in payloads.items():
        if layout.get("opaque"):
            continue
        for direction in ("request", "response"):
            fields = layout.get(direction, [])
            names = []
            for i, f in enumerate(fields):
                if f.get("type") not in FIELD_TYPES:
                    raise ProtocolError(f"l3_payloads: {k}.{direction}.{f.get('name')} has "
                                        f"unknown type {f.get('type')!r} (expected u8|u16|bytes)")
                if f["type"] == "bytes" and i != len(fields) - 1:
                    raise ProtocolError(f"l3_payloads: {k}.{direction}: bytes field "
                                        f"{f['name']} must be last")
                if "len_from" in f and f["len_from"] not in names:
                    raise ProtocolError(f"l3_payloads: {k}.{direction}.{f['name']} len_from "
                                        f"{f['len_from']!r} does not name a preceding field")
                names.append(f["name"])
    if not isinstance(p.get("descriptor", {}).get("crc"), str):
        raise ProtocolError("descriptor.crc must name the CRC variant")


# --- model -------------------------------------------------------------------------------

def _hex2(v: int) -> str:
    return f"0x{v:02X}"


def _pyrepr(v):
    return f'"{v}"' if isinstance(v, str) else str(v)


def _cbool(v) -> str:
    return "true" if v else "false"


def _pybool(v) -> str:
    return "True" if v else "False"


def _bounds(p: dict, layout: dict, direction: str) -> tuple[int, int]:
    if layout.get("opaque"):
        return 0, p["limits"]["max_l3_payload"]
    fields = layout.get(direction)
    if fields is None:
        return 0, 0
    fixed = sum(FIELD_WIDTH.get(f["type"], 0) for f in fields)
    has_tail = any(f["type"] == "bytes" for f in fields)
    return fixed, (p["limits"]["max_l3_payload"] if has_tail else fixed)


def build_model(p: dict, vectors_dir: pathlib.Path | None) -> dict:
    scalar_sections = []
    for section, prefix, ctype in SCALAR_SECTIONS:
        items = []
        for k in sorted(p[section]):
            v = _scalar_value(section, p[section][k])
            if isinstance(v, str):
                items.append({"symbol": f"{prefix}{k}", "ctype": "const char*",
                              "cvalue": f'"{v}"', "pyvalue": f'"{v}"'})
            else:
                cv = _hex2(v) if section in BYTE_SECTIONS else str(v)
                items.append({"symbol": f"{prefix}{k}", "ctype": ctype, "cvalue": cv,
                              "pyvalue": cv})
        scalar_sections.append({"section": section, "entries": items}) # not "items": Jinja would resolve dict.items()

    reserved = [{"name": k.upper(), "min": _hex2(v["min"]), "max": _hex2(v["max"])}
                for k, v in sorted(p.get("reserved_opcode_ranges", {}).items())]

    opcode_info = []
    for name, v in sorted(p["opcodes"].items(), key=lambda kv: kv[1]["code"]):
        idem = bool(v.get("idempotent", False))
        opcode_info.append({"name": name, "chex": _hex2(v["code"]), "target": v["target"],
                            "target_enum": TARGETS[v["target"]], "idempotent_c": _cbool(idem),
                            "idempotent_py": _pybool(idem)})

    tlv_info = []
    for name, v in sorted(p["tlv"].items(), key=lambda kv: kv[1]["type"]):
        tlv_info.append({"name": name, "thex": _hex2(v["type"]),
                         "required_c": _cbool(v.get("required")), "required_py": _pybool(v.get("required")),
                         "repeated_c": _cbool(v.get("repeated")), "repeated_py": _pybool(v.get("repeated")),
                         "max_len": int(v.get("max_len", 0))})

    payload_info, payload_fields = [], []
    for name, v in sorted(p["opcodes"].items(), key=lambda kv: kv[1]["code"]):
        layout = p["l3_payloads"][name]
        rmin, rmax = _bounds(p, layout, "request")
        smin, smax = _bounds(p, layout, "response")
        opaque = bool(layout.get("opaque"))
        payload_info.append({"name": name, "chex": _hex2(v["code"]), "req_min": rmin, "req_max": rmax,
                             "resp_min": smin, "resp_max": smax, "opaque_c": _cbool(opaque),
                             "opaque_py": _pybool(opaque)})
        entry = {"name": name, "opaque_py": _pybool(opaque)}
        for direction in ("request", "response"):
            fields = ([{"name": "bytes", "type": "bytes"}] if opaque else layout.get(direction, []))
            entry[direction] = [{"name": f["name"], "type": f["type"],
                                 "max_py": str(f["max"]) if "max" in f else "None",
                                 "len_from_py": f'"{f["len_from"]}"' if "len_from" in f else "None"}
                                for f in fields]
        payload_fields.append(entry)

    name_maps = []
    for symbol, csymbol, section, value_of in NAME_MAPS:
        items = sorted(({"value": _hex2(value_of(v)), "name": k} for k, v in p[section].items()),
                       key=lambda e: int(e["value"], 16))
        name_maps.append({"symbol": symbol, "csymbol": csymbol, "entries": items})

    return {
        "protocol": p["protocol"], "scalar_sections": scalar_sections, "reserved": reserved,
        "descriptor": p["descriptor"], "opcode_info": opcode_info, "tlv_info": tlv_info,
        "payload_info": payload_info, "payload_fields": payload_fields, "name_maps": name_maps,
        "vectors": load_vectors(vectors_dir),
    }


def _cstr(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def load_vectors(vectors_dir: pathlib.Path | None) -> list[dict]:
    out = []
    if vectors_dir is None or not vectors_dir.is_dir():
        return out
    for f in sorted(vectors_dir.glob("*.json")):
        v = json.loads(f.read_text())
        raw = bytes.fromhex(v["bytes"].replace(" ", ""))
        if not raw:
            raise ProtocolError(f"vector {f.name}: bytes must not be empty")
        if v["name"] != f.stem:
            raise ProtocolError(f"vector {f.name}: name {v['name']!r} must equal the file stem")
        out.append({"name": v["name"], "kind": v["kind"], "spec_ref_c": _cstr(v["spec_ref"]),
                    "canonical_c": _cstr(v["canonical"]),
                    "bytes_c": ", ".join(_hex2(b) for b in raw), "len": len(raw)})
    return sorted(out, key=lambda e: e["name"])


# --- rendering ---------------------------------------------------------------------------

def render(model: dict) -> dict[str, str]:
    # The templates render C++ and Python *source* (never HTML), so HTML autoescaping must
    # stay off — it would mangle quotes and angle brackets in the generated code.
    # select_autoescape() with no HTML extensions expresses that explicitly (and is the
    # form CodeQL's py/jinja2/autoescape-false query recognises as deliberate).
    env = Environment(loader=FileSystemLoader(str(TEMPLATES)), trim_blocks=True,
                      lstrip_blocks=True, keep_trailing_newline=True, undefined=StrictUndefined,
                      autoescape=select_autoescape(enabled_extensions=(), default_for_string=False,
                                                   default=False))
    out = {}
    for name in OUTPUTS:
        text = env.get_template(name + ".j2").render(**model)
        lines = [l.rstrip() for l in text.splitlines()]
        out[name] = "\n".join(lines).rstrip("\n") + "\n"
    return out


# --- docs lint (--check-docs) --------------------------------------------------------------

def _sections(md: str) -> dict[str, str]:
    """Map heading number ('3.1', '4.1', ...) to the text of that section."""
    parts = re.split(r"^(#{2,3}) (\d+(?:\.\d+)?)\.? [^\n]*$", md, flags=re.M)
    out: dict[str, str] = {}
    # parts: [pre, hashes, num, body, hashes, num, body, ...]
    for i in range(1, len(parts) - 2, 3):
        out[parts[i + 1]] = parts[i + 2]
    return out


def check_docs(p: dict, md: str) -> list[str]:
    findings: list[str] = []
    sec = _sections(md)
    row = re.compile(r"^\| (0x[0-9A-Fa-f]{2}) \| ([A-Z_]+) \|", re.M)

    def table(section_id: str, what: str, yaml_map: dict[str, int]):
        text = sec.get(section_id)
        if text is None:
            findings.append(f"docs: section {section_id} ({what}) not found")
            return
        doc_map = {name: int(code, 16) for code, name in row.findall(text)}
        for name, code in sorted(yaml_map.items()):
            if name not in doc_map:
                findings.append(f"docs §{section_id}: {what} {name} ({code:#04x}) missing from table")
            elif doc_map[name] != code:
                findings.append(f"docs §{section_id}: {what} {name} is {doc_map[name]:#04x} in the "
                                f"table but {code:#04x} in the YAML")
        for name, code in sorted(doc_map.items()):
            if name not in yaml_map:
                findings.append(f"docs §{section_id}: table row {name} ({code:#04x}) has no YAML entry")

    table("3.1", "opcode", {k: v["code"] for k, v in p["opcodes"].items()})
    table("4.1", "record type", {k: v["type"] for k, v in p["tlv"].items()})

    err_line = next((l for l in sec.get("3.1", "").splitlines() if l.startswith("Error codes")), "")
    doc_errs = {int(x, 16) for x in re.findall(r"`(0x[0-9A-Fa-f]{2})`", err_line)}
    yaml_errs = {k: v for k, v in p["error_codes"].items()}
    for k, v in sorted(yaml_errs.items()):
        if v not in doc_errs:
            findings.append(f"docs §3.1: error code {k} ({v:#04x}) missing from the error-codes line")
    for v in sorted(doc_errs - set(yaml_errs.values())):
        findings.append(f"docs §3.1: error-codes line lists {v:#04x}, which the YAML does not define")

    ev_text = sec.get("3.4", "")
    doc_events = {n: int(c, 16) for n, c in re.findall(r"([A-Z_]+) \((0x[0-9A-Fa-f]{2})", ev_text)}
    for k, v in sorted(p["events"].items()):
        base = k[:-4] if k.endswith("_MIN") else k
        if k.endswith("_MAX"):
            if f"0x{v:02X}" not in ev_text:
                findings.append(f"docs §3.4: event range end {k} ({v:#04x}) not mentioned")
            continue
        if doc_events.get(base) != v:
            findings.append(f"docs §3.4: event {base} ({v:#04x}) missing or with a different code")
    for n, c in sorted(doc_events.items()):
        if n not in p["events"] and f"{n}_MIN" not in p["events"]:
            findings.append(f"docs §3.4: event {n} ({c:#04x}) has no YAML entry")

    def enum_line(section_id: str, marker: str, what: str, yaml_map: dict[str, int]):
        line = next((l for l in sec.get(section_id, "").splitlines() if marker in l), "")
        doc_map = {name.upper(): int(v) for v, name in re.findall(r"(?<![\w.x])(\d)=([A-Za-z_]+)", line)}
        for k, v in sorted(yaml_map.items()):
            if doc_map.get(k) != v:
                findings.append(f"docs §{section_id}: {what} {k} ({v}) missing or with a different value")
        for k, v in sorted(doc_map.items()):
            if k not in yaml_map:
                findings.append(f"docs §{section_id}: {what} {k} ({v}) has no YAML entry")

    enum_line("3.3", "state", "node state", p["node_states"])
    enum_line("4.1", "| 0x20 | PARAM |", "param kind", p["param_kinds"])
    return findings


# --- CLI -----------------------------------------------------------------------------------

def _rel(path: pathlib.Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--yaml", default=str(ROOT / "protocol" / "omgp-protocol.yaml"))
    ap.add_argument("--out", default=str(ROOT / "build" / "gen"))
    ap.add_argument("--vectors", default=str(ROOT / "tests" / "vectors"),
                    help="directory of golden-vector JSON files (missing dir = no vectors)")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 with a diff if --out differs from a fresh render; write nothing")
    ap.add_argument("--check-docs", action="store_true",
                    help="verify docs/protocol-l3.md tables against the YAML; write nothing")
    ap.add_argument("--docs", default=str(ROOT / "docs" / "protocol-l3.md"))
    args = ap.parse_args(argv)

    try:
        p = yaml.safe_load(pathlib.Path(args.yaml).read_text())
        validate(p)
        if args.check_docs:
            findings = check_docs(p, pathlib.Path(args.docs).read_text())
            for f in findings:
                print(f)
            if findings:
                print(f"codegen --check-docs: {len(findings)} drift finding(s) between "
                      f"{_rel(pathlib.Path(args.docs))} and {_rel(pathlib.Path(args.yaml))}")
                return 1
            print("codegen --check-docs: docs tables match the YAML")
            return 0
        rendered = render(build_model(p, pathlib.Path(args.vectors)))
    except ProtocolError as e:
        print(f"codegen: protocol definition error: {e}", file=sys.stderr)
        return 2

    out_dir = pathlib.Path(args.out)
    if args.check:
        drift = 0
        for name, text in rendered.items():
            current = (out_dir / name).read_text() if (out_dir / name).exists() else ""
            if current != text:
                drift += 1
                print(f"codegen --check: {name} differs from a fresh render")
                sys.stdout.writelines(difflib.unified_diff(
                    current.splitlines(True), text.splitlines(True),
                    fromfile=f"{_rel(out_dir / name)} (on disk)", tofile=f"{name} (fresh)", n=1))
        return 1 if drift else 0

    out_dir.mkdir(parents=True, exist_ok=True)
    for name, text in rendered.items():
        (out_dir / name).write_text(text)
    print(f"codegen: wrote {_rel(out_dir)}/{OUTPUTS[0]}, " + ", ".join(OUTPUTS[1:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
