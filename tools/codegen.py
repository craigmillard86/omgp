#!/usr/bin/env python3
"""Generate C++ header + Python module from protocol/omgp-protocol.yaml.
Deterministic: same YAML in -> byte-identical output."""
import yaml, pathlib, sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "gen"
OUT.mkdir(parents=True, exist_ok=True)
p = yaml.safe_load((ROOT / "protocol" / "omgp-protocol.yaml").read_text())

def cxx_const(name, val):
    return f"inline constexpr uint32_t {name} = {val};"

hdr = ["// GENERATED from protocol/omgp-protocol.yaml -- DO NOT EDIT",
       "#pragma once", "#include <cstdint>", "namespace omgp {",
       f"inline constexpr uint8_t PROTOCOL_MAJOR = {p['protocol']['major']};",
       f"inline constexpr uint8_t PROTOCOL_MINOR = {p['protocol']['minor']};"]
py  = ["# GENERATED from protocol/omgp-protocol.yaml -- DO NOT EDIT",
       f"PROTOCOL_MAJOR = {p['protocol']['major']}",
       f"PROTOCOL_MINOR = {p['protocol']['minor']}"]

def emit_map(section, prefix, value_of):
    hdr.append(f"// --- {section} ---")
    for k in sorted(p[section]):
        v = value_of(p[section][k])
        hdr.append(cxx_const(f"{prefix}{k}", v))
        py.append(f"{prefix}{k} = {v}")

emit_map("limits", "LIMIT_", lambda v: v)
emit_map("addressing", "ADDR_", lambda v: v)
emit_map("l3_flags", "FLAG_", lambda v: v)
emit_map("opcodes", "OP_", lambda v: v["code"])
emit_map("error_codes", "", lambda v: v)
emit_map("node_states", "STATE_", lambda v: v)
emit_map("events", "EVT_", lambda v: v)
emit_map("param_kinds", "KIND_", lambda v: v)
emit_map("tlv", "TLV_", lambda v: v["type"])
emit_map("module_types", "MT_", lambda v: v)
emit_map("link_trunk", "TRUNK_", lambda v: v if isinstance(v, int) else f'0 /* {v} */')
emit_map("module_bus", "MBUS_", lambda v: v if isinstance(v, int) else f'0 /* {v} */')
hdr.append("} // namespace omgp")

(OUT / "omgp_protocol.h").write_text("\n".join(hdr) + "\n")
(OUT / "omgp_protocol.py").write_text("\n".join(py) + "\n")
print(f"codegen: wrote {OUT}/omgp_protocol.h and .py")
