"""T020 agent-dispatchable slice (issue #38): `genvectors.py` must be able to build the
five `frame_*` records of contracts/frame-vectors.md's table, validated against the
schema amendment (#24/T006) and cross-checked against the T018 codec and T019 canonical
renderer — all without ever writing under `tests/vectors/` (OPERATING-POLICY §2; the
creating commit is human-triggered, CLAUDE.md rule 9, and out of this issue's scope).
"""
from __future__ import annotations

import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import canonical as C  # noqa: E402
import genvectors as GV  # noqa: E402
import omgp_link as link  # noqa: E402

SCHEMA_PATH = (
    pathlib.Path(__file__).resolve().parents[2]
    / "specs" / "001-protocol-foundation" / "contracts" / "golden-vector.schema.json"
)

EXPECTED_NAMES = {
    "frame_ping_req", "frame_response", "frame_retry", "frame_max_payload", "frame_worst_stuffing",
}


def _schema() -> dict:
    return json.loads(SCHEMA_PATH.read_text())


def _frame_entries() -> list[dict]:
    return [e for e in GV.build() if e["kind"] == "frame"]


def test_five_frame_vectors_are_generated():
    names = {e["name"] for e in _frame_entries()}
    assert names == EXPECTED_NAMES


def test_no_file_written_under_tests_vectors(tmp_path):
    # build() must be pure: calling it does not touch the real tests/vectors/ directory.
    before = sorted((GV.VECTORS).glob("frame_*.json")) if GV.VECTORS.exists() else []
    GV.build()
    after = sorted((GV.VECTORS).glob("frame_*.json")) if GV.VECTORS.exists() else []
    assert before == after == []


def test_entries_match_schema_contract():
    schema = _schema()
    name_pattern = schema["properties"]["name"]["pattern"]
    bytes_pattern = schema["properties"]["bytes"]["pattern"]
    canonical_pattern = schema["properties"]["canonical"]["pattern"]
    required = set(schema["required"])
    allowed = set(schema["properties"]) | {"note"}
    assert schema["properties"]["kind"]["enum"] == ["message", "status", "descriptor", "frame"]
    for e in _frame_entries():
        assert re.fullmatch(name_pattern, e["name"]), e["name"]
        assert re.fullmatch(bytes_pattern, e["bytes"]), e["bytes"]
        assert re.fullmatch(canonical_pattern, e["canonical"]), e["canonical"]
        assert required <= set(e)
        assert set(e) <= allowed
        assert e["spec_ref"].startswith("trunk §4"), e["spec_ref"]
        assert set(e["fields"]) == {"dst", "src", "response", "retry", "seq", "payload"}
        assert isinstance(e["fields"]["response"], bool) and isinstance(e["fields"]["retry"], bool)


def test_canonical_round_trips_through_t019_renderer():
    for e in _frame_entries():
        frame = C.canonical_to_frame(e["canonical"])
        assert C.frame_to_canonical(frame) == e["canonical"]
        assert frame.dst == e["fields"]["dst"]
        assert frame.src == e["fields"]["src"]
        assert frame.response == e["fields"]["response"]
        assert frame.retry == e["fields"]["retry"]
        assert frame.seq == e["fields"]["seq"]
        assert frame.payload.hex() == e["fields"]["payload"]


def test_bytes_match_t018_codec():
    for e in _frame_entries():
        frame = C.canonical_to_frame(e["canonical"])
        raw = link.encode_frame(frame)
        assert " ".join(f"{b:02X}" for b in raw) == e["bytes"]
        d = link.Deframer()
        decoded = d.feed_bytes(raw)
        assert decoded == [frame]
        assert d.stats["delivered"] == 1


def test_frame_retry_seq_is_the_four_bit_maximum():
    entry = next(e for e in _frame_entries() if e["name"] == "frame_retry")
    assert entry["fields"]["seq"] == 15
    assert entry["fields"]["retry"] is True


def test_frame_max_payload_is_exactly_72_wire_bytes():
    entry = next(e for e in _frame_entries() if e["name"] == "frame_max_payload")
    raw_len = len(entry["bytes"].split(" "))
    assert raw_len == 72


def test_frame_worst_stuffing_is_exactly_140_wire_bytes():
    entry = next(e for e in _frame_entries() if e["name"] == "frame_worst_stuffing")
    raw_len = len(entry["bytes"].split(" "))
    assert raw_len == 140
    assert entry["fields"]["dst"] == entry["fields"]["src"] == 0x7D
    assert entry["fields"]["response"] is True and entry["fields"]["retry"] is True
    assert entry["fields"]["seq"] == 11
