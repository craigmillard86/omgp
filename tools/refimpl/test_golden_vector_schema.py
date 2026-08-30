"""T006 (spec 002 Phase 1): the golden-vector schema must be able to describe `frame`
vectors ahead of their creation (contracts/frame-vectors.md "Schema amendment"). This does
not exercise any frame codec — it only pins the schema contract itself.
"""
from __future__ import annotations

import json
import pathlib
import re

SCHEMA_PATH = (
    pathlib.Path(__file__).resolve().parents[2]
    / "specs" / "001-protocol-foundation" / "contracts" / "golden-vector.schema.json"
)


def _schema() -> dict:
    return json.loads(SCHEMA_PATH.read_text())


def test_schema_is_valid_json():
    _schema()


def test_kind_enum_gains_frame():
    schema = _schema()
    assert schema["properties"]["kind"]["enum"] == ["message", "status", "descriptor", "frame"]


def test_name_pattern_accepts_frame_prefix():
    schema = _schema()
    pattern = schema["properties"]["name"]["pattern"]
    assert re.fullmatch(pattern, "frame_ping_req")
    assert re.fullmatch(pattern, "msg_ping_req")
    assert not re.fullmatch(pattern, "bogus_ping_req")


def test_fields_description_lists_frame_fields():
    schema = _schema()
    description = schema["properties"]["fields"]["description"]
    for field in ("dst", "src", "response", "retry", "seq", "payload"):
        assert field in description


def test_required_and_additional_properties_unchanged():
    schema = _schema()
    assert schema["required"] == ["name", "kind", "spec_ref", "fields", "canonical", "bytes"]
    assert schema["additionalProperties"] is False
