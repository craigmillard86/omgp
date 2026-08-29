"""Tests for tools/codegen.py — spec 001 US1 (FR-001..FR-005, SC-002). Written first.

Contract: specs/001-protocol-foundation/contracts/generated-constants.md
"""
from __future__ import annotations

import hashlib
import pathlib
import subprocess
import sys

import pytest
import yaml

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import _gen  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
CODEGEN = ROOT / "tools" / "codegen.py"
YAML = ROOT / "protocol" / "omgp-protocol.yaml"
DOCS = ROOT / "docs" / "protocol-l3.md"
OUTPUTS = ("omgp_protocol.h", "omgp_protocol.py", "omgp_vectors.h", "omgp_names.h")


def run(*args: str, yaml_path=YAML):
    r = subprocess.run([sys.executable, str(CODEGEN), "--yaml", str(yaml_path), *args],
                       capture_output=True, text=True, cwd=ROOT)
    return r.returncode, r.stdout, r.stderr


def digest(d: pathlib.Path) -> dict[str, str]:
    return {n: hashlib.sha256((d / n).read_bytes()).hexdigest() for n in OUTPUTS}


def reversed_keys(node):
    """Same document, every mapping's key order reversed (lists keep their order)."""
    if isinstance(node, dict):
        return {k: reversed_keys(node[k]) for k in reversed(list(node))}
    if isinstance(node, list):
        return [reversed_keys(x) for x in node]
    return node


def write_yaml(tmp_path: pathlib.Path, doc, name="p.yaml") -> pathlib.Path:
    p = tmp_path / name
    p.write_text(yaml.safe_dump(doc, sort_keys=False))
    return p


@pytest.fixture(scope="module")
def spec_doc():
    return yaml.safe_load(YAML.read_text())


# --- determinism (FR-002, SC-002) -------------------------------------------------------

def test_two_runs_are_byte_identical(tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    assert run("--out", str(a))[0] == 0
    assert run("--out", str(b))[0] == 0
    assert digest(a) == digest(b)


def test_key_order_does_not_change_output(tmp_path, spec_doc):
    a = tmp_path / "a"
    assert run("--out", str(a))[0] == 0
    shuffled = write_yaml(tmp_path, reversed_keys(spec_doc))
    b = tmp_path / "b"
    assert run("--out", str(b), yaml_path=shuffled)[0] == 0
    assert digest(a) == digest(b)


def test_outputs_have_no_volatile_content(tmp_path):
    out = tmp_path / "o"
    assert run("--out", str(out))[0] == 0
    for n in OUTPUTS:
        text = (out / n).read_text()
        assert str(tmp_path) not in text
        assert "20" + "26-" not in text and "Generated on" not in text
        assert text.endswith("\n") and not any(l.rstrip() != l for l in text.splitlines())


# --- completeness (FR-001, FR-003) ------------------------------------------------------

PREFIX = {"limits": "LIMIT_", "addressing": "ADDR_", "l3_flags": "FLAG_", "node_states": "STATE_",
          "events": "EVT_", "param_kinds": "KIND_", "module_types": "MT_", "link_trunk": "TRUNK_",
          "module_bus": "MBUS_", "error_codes": ""}


def test_every_scalar_section_key_present_in_both_outputs(tmp_path, spec_doc):
    out = tmp_path / "o"
    assert run("--out", str(out))[0] == 0
    mod = _gen.load(out)
    hdr = (out / "omgp_protocol.h").read_text()
    for section, prefix in PREFIX.items():
        for key, val in spec_doc[section].items():
            sym = f"{prefix}{key}"
            assert getattr(mod, sym) == val, sym
            assert f" {sym} = " in hdr, sym
    for key, v in spec_doc["opcodes"].items():
        assert getattr(mod, f"OP_{key}") == v["code"]
        assert f" OP_{key} = " in hdr
    for key, v in spec_doc["tlv"].items():
        assert getattr(mod, f"TLV_{key}") == v["type"]
    for key, v in spec_doc["reserved_opcode_ranges"].items():
        assert getattr(mod, f"RESERVED_{key.upper()}_MIN") == v["min"]
        assert getattr(mod, f"RESERVED_{key.upper()}_MAX") == v["max"]
    assert mod.PROTOCOL_MAJOR == spec_doc["protocol"]["major"]
    assert mod.PROTOCOL_MINOR == spec_doc["protocol"]["minor"]


def test_string_values_preserved_not_placeholders(tmp_path):
    out = tmp_path / "o"
    assert run("--out", str(out))[0] == 0
    mod = _gen.load(out)
    hdr = (out / "omgp_protocol.h").read_text()
    assert mod.TRUNK_crc == "crc16_ccitt_false"
    assert mod.MBUS_conventions == "smbus"
    assert mod.DESC_CRC == "crc16_ccitt_false"
    assert 'inline constexpr const char* TRUNK_crc = "crc16_ccitt_false";' in hdr
    assert "/* crc16_ccitt_false */" not in hdr  # the old generator's lossy form


def test_attribute_tables(tmp_path, spec_doc):
    out = tmp_path / "o"
    assert run("--out", str(out))[0] == 0
    mod = _gen.load(out)
    hdr = (out / "omgp_protocol.h").read_text()

    codes = [e["code"] for e in mod.OPCODE_INFO]
    assert codes == sorted(codes) and len(codes) == len(spec_doc["opcodes"])
    err = next(e for e in mod.OPCODE_INFO if e["name"] == "ERROR")
    assert err["target"] == "response_only" and err["idempotent"] is False
    assert next(e for e in mod.OPCODE_INFO if e["name"] == "SET_PARAM")["idempotent"] is True

    types_ = [e["type"] for e in mod.TLV_INFO]
    assert types_ == sorted(types_)
    name = next(e for e in mod.TLV_INFO if e["name"] == "NAME")
    assert name["required"] is True and name["repeated"] is False and name["max_len"] == 24
    chan = next(e for e in mod.TLV_INFO if e["name"] == "CHANNEL")
    assert chan["repeated"] is True and chan["max_len"] == 0

    pi = {e["name"]: e for e in mod.PAYLOAD_INFO}
    assert [e["code"] for e in mod.PAYLOAD_INFO] == sorted(e["code"] for e in mod.PAYLOAD_INFO)
    assert pi["SET_PARAM"] == {"name": "SET_PARAM", "code": mod.OP_SET_PARAM, "req_min": 4,
                               "req_max": 4, "resp_min": 0, "resp_max": 0, "opaque": False}
    assert pi["BP_POWER"]["opaque"] is True and pi["BP_POWER"]["req_max"] == mod.LIMIT_max_l3_payload
    assert pi["GET_EVENT"]["resp_min"] == 2 and pi["GET_EVENT"]["resp_max"] == mod.LIMIT_max_l3_payload
    assert pi["READ_DESC"]["resp_min"] == 3 and pi["IDENTIFY"]["resp_min"] == 7
    assert pi["ERROR"]["req_min"] == 0 and pi["ERROR"]["resp_min"] == 1

    fields = mod.PAYLOAD_FIELDS["SET_PARAM"]["request"]
    assert [f["name"] for f in fields] == ["param_id", "scope", "value"]
    assert fields[2]["type"] == "u16" and fields[2]["max"] == 4095
    assert mod.PAYLOAD_FIELDS["READ_DESC"]["response"][2]["len_from"] == "len"

    # reverse maps are plural: TLV_NAME is the NAME record's type constant (0x03)
    assert mod.TLV_NAME == 3 and mod.TLV_NAMES[mod.TLV_NAME] == "NAME"
    assert mod.OPCODE_NAMES[mod.OP_PING] == "PING" and mod.TLV_NAMES[mod.TLV_VENDOR] == "VENDOR"
    assert mod.ERROR_NAMES[mod.ERR_BUSY] == "ERR_BUSY" and mod.EVENT_NAMES[mod.EVT_NONE] == "NONE"
    assert mod.MODULE_TYPE_NAMES[mod.MT_TUBE_PREAMP] == "TUBE_PREAMP"
    assert mod.STATE_NAMES[mod.STATE_READY] == "READY" and mod.KIND_NAMES[mod.KIND_ENUM] == "ENUM"
    assert mod.DESC_MAX_BYTES == mod.LIMIT_max_descriptor_bytes

    for token in ("enum class Target", "struct OpcodeInfo", "OPCODE_INFO[]", "struct TlvInfo",
                  "TLV_INFO[]", "struct PayloadInfo", "PAYLOAD_INFO[]",
                  "DESC_MAX_BYTES = LIMIT_max_descriptor_bytes"):
        assert token in hdr, token


# --- validation (FR-004) ----------------------------------------------------------------

def _expect_conflict(tmp_path, doc, needle):
    rc, out, err = run("--out", str(tmp_path / "o"), yaml_path=write_yaml(tmp_path, doc))
    assert rc == 2, (rc, out, err)
    assert needle in err + out


def test_duplicate_opcode_rejected(tmp_path, spec_doc):
    doc = reversed_keys(reversed_keys(spec_doc))  # deep copy
    doc["opcodes"]["PING"]["code"] = doc["opcodes"]["IDENTIFY"]["code"]
    _expect_conflict(tmp_path, doc, "PING")


def test_duplicate_tlv_type_rejected(tmp_path, spec_doc):
    doc = reversed_keys(reversed_keys(spec_doc))
    doc["tlv"]["SERIAL"]["type"] = doc["tlv"]["NAME"]["type"]
    _expect_conflict(tmp_path, doc, "SERIAL")


def test_value_wider_than_a_byte_rejected(tmp_path, spec_doc):
    doc = reversed_keys(reversed_keys(spec_doc))
    doc["error_codes"]["ERR_INTERNAL"] = 0x100
    _expect_conflict(tmp_path, doc, "ERR_INTERNAL")


def test_bad_target_rejected(tmp_path, spec_doc):
    doc = reversed_keys(reversed_keys(spec_doc))
    doc["opcodes"]["PING"]["target"] = "everyone"
    _expect_conflict(tmp_path, doc, "everyone")


def test_reserved_range_overlap_rejected(tmp_path, spec_doc):
    doc = reversed_keys(reversed_keys(spec_doc))
    doc["opcodes"]["BP_ROUTE"]["code"] = 0x65  # inside firmware_update 0x60-0x6F
    _expect_conflict(tmp_path, doc, "BP_ROUTE")


def test_payload_for_unknown_opcode_rejected(tmp_path, spec_doc):
    doc = reversed_keys(reversed_keys(spec_doc))
    doc["l3_payloads"]["NOPE"] = {"request": [], "response": []}
    _expect_conflict(tmp_path, doc, "NOPE")


def test_unknown_field_width_rejected(tmp_path, spec_doc):
    doc = reversed_keys(reversed_keys(spec_doc))
    doc["l3_payloads"]["SET_PARAM"]["request"][0]["type"] = "u24"
    _expect_conflict(tmp_path, doc, "u24")


# --- --check and --check-docs -----------------------------------------------------------

def test_check_mode(tmp_path):
    out = tmp_path / "o"
    assert run("--out", str(out))[0] == 0
    assert run("--out", str(out), "--check")[0] == 0
    (out / "omgp_protocol.py").write_text("tampered\n")
    rc, stdout, _ = run("--out", str(out), "--check")
    assert rc == 1 and "omgp_protocol.py" in stdout


def test_check_docs_passes_on_repo():
    rc, out, err = run("--check-docs")
    assert rc == 0, out + err


def test_check_docs_fails_on_drifted_fixture(tmp_path):
    lines = DOCS.read_text().splitlines()
    kept = [l for l in lines if not l.startswith("| 0x11 | SET_BYPASS")]  # one row removed
    kept = [l.replace("| 0x12 | SET_PARAM", "| 0x99 | SET_PARAM") for l in kept]  # one wrong code
    fixture = tmp_path / "protocol-l3.md"
    fixture.write_text("\n".join(kept) + "\n")
    rc, out, err = run("--check-docs", "--docs", str(fixture))
    assert rc == 1
    assert "SET_BYPASS" in out + err and "SET_PARAM" in out + err


# --- vectors header ---------------------------------------------------------------------

def test_vectors_header_renders_empty_without_vectors(tmp_path):
    out = tmp_path / "o"
    empty = tmp_path / "novec"
    empty.mkdir()
    assert run("--out", str(out), "--vectors", str(empty))[0] == 0
    hdr = (out / "omgp_vectors.h").read_text()
    assert "COUNT = 0" in hdr and "namespace vectors" in hdr


def test_success_line(tmp_path):
    rc, out, _ = run("--out", str(tmp_path / "o"))
    assert rc == 0
    assert out.strip().startswith("codegen: wrote ") and "omgp_vectors.h" in out


def test_names_header_tables(tmp_path):
    out = tmp_path / "o"
    assert run("--out", str(out))[0] == 0
    hdr = (out / "omgp_names.h").read_text()
    for table in ("OPCODE_TABLE", "TLV_TABLE", "ERROR_TABLE", "EVENT_TABLE", "MODULE_TYPE_TABLE",
                  "NODE_STATE_TABLE", "PARAM_KIND_TABLE"):
        assert f"Entry {table}[]" in hdr, table
    assert '{ 0x7F, "ERROR" }' in hdr and '{ 0xF0, "USER_DEFINED_MIN" }' in hdr
    assert "namespace names" in hdr
