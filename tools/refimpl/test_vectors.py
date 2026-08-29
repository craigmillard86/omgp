"""Bind the Python reference implementation to the committed golden vectors (spec 001
FR-025, T035): for every tests/vectors/*.json, encode(fields) == bytes and decode(bytes)
renders to canonical. The vectors are immutable; if this fails, the implementation is wrong.
"""
from __future__ import annotations

import json
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import canonical as C  # noqa: E402
import omgp_l3 as l3  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
VECTORS = sorted((ROOT / "tests" / "vectors").glob("*.json"))


def _bytes(v: dict) -> bytes:
    return bytes.fromhex(v["bytes"].replace(" ", ""))


@pytest.mark.parametrize("path", VECTORS, ids=[p.stem for p in VECTORS])
def test_vector(path: pathlib.Path):
    v = json.loads(path.read_text())
    assert v["name"] == path.stem
    raw = _bytes(v)
    if v["kind"] == "message":
        h, obj = C.canonical_to_message(v["canonical"])
        assert C.encode_message(h, obj) == raw, "encode(canonical) != bytes"
        assert C.render_bytes(raw) == v["canonical"], "render(bytes) != canonical"
        assert v["fields"]["opcode"] == v["canonical"].split()[0].split("=")[1]
        assert h.node_id == v["fields"]["node_id"] and h.seq == v["fields"]["seq"]
    elif v["kind"] == "status":
        sb = C.canonical_to_status(v["canonical"])
        assert l3.encode_status_block(sb) == raw
        assert C.status_to_canonical(l3.decode_status_block(raw)) == v["canonical"]
    elif v["kind"] == "descriptor":
        desc = pytest.importorskip("omgp_descriptor")  # arrives with US3
        recs = desc.parse_descriptor(raw)
        assert C.descriptor_to_canonical(recs) == v["canonical"]
        assert desc.build_descriptor(C.canonical_to_descriptor(v["canonical"])) == raw
    else:
        pytest.fail(f"unknown vector kind {v['kind']!r}")


def test_vectors_exist():
    assert VECTORS, "no golden vectors committed under tests/vectors/"
