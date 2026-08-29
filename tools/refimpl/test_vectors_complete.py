"""SC-001 (spec 001 T068): every opcode has a golden vector for its request and, where the
protocol defines one, its response; the status block and the sample descriptor exist. A
new opcode without vectors fails here before it fails anywhere subtler."""
from __future__ import annotations

import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from _gen import P  # noqa: E402

G = P()
VECTORS = pathlib.Path(__file__).resolve().parents[2] / "tests" / "vectors"


def _messages():
    out = []
    for f in sorted(VECTORS.glob("*.json")):
        v = json.loads(f.read_text())
        if v["kind"] == "message":
            out.append((v["fields"]["opcode"], "response" if v["fields"]["flags"] & G.FLAG_response else "request"))
    return out


def test_every_opcode_direction_has_a_vector():
    have = set(_messages())
    missing = []
    for e in G.PAYLOAD_INFO:
        name = e["name"]
        target = next(o["target"] for o in G.OPCODE_INFO if o["name"] == name)
        if target != "response_only" and (name, "request") not in have:
            missing.append(f"{name} request")
        if (name, "response") not in have:
            missing.append(f"{name} response")
    assert not missing, f"opcodes without a golden vector: {missing}"


def test_status_block_and_descriptor_vectors_exist():
    kinds = {json.loads(f.read_text())["kind"] for f in VECTORS.glob("*.json")}
    assert {"message", "status", "descriptor"} <= kinds
    assert (VECTORS / "status_block.json").exists()
    assert (VECTORS / "descriptor_sample.json").exists() and (VECTORS / "descriptor_min.json").exists()


def test_vector_names_match_files_and_schema_fields():
    for f in sorted(VECTORS.glob("*.json")):
        v = json.loads(f.read_text())
        assert v["name"] == f.stem
        assert set(v) >= {"name", "kind", "spec_ref", "fields", "canonical", "bytes"}
        assert set(v) <= {"name", "kind", "spec_ref", "fields", "canonical", "bytes", "note"}
        assert v["canonical"].isascii() and v["bytes"] == v["bytes"].upper()
