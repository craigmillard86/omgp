"""Descriptor golden vectors (spec 001 T047), consumed by genvectors.py.

descriptor_sample exercises all 14 record types, one unknown type, VENDOR, 4 channels, 8
params (one enum with 3 PARAM_ENUM rows), POWER_TUBE, and is >= 600 bytes. descriptor_min
holds only the required records. msg_identify_resp is created here — not in the message
list — because its desc_len/desc_crc derive from the sample (FR-034), so no committed
vector is ever regenerated.
"""
from __future__ import annotations

import dataclasses

from _gen import P
import canonical as C
import omgp_descriptor as D
import omgp_l3 as l3

G = P()


def sample_records() -> list:
    recs = [
        D.ProtocolRec(1, 0),
        D.ModuleTypeRec(G.MT_TUBE_PREAMP),
        D.NameRec("British Preamp"),
        D.ManufacturerRec("OMGP"),
        D.ModelIdRec(0x0101, 0x0002, 0x0103),
        D.SerialRec("BP-0001"),
        D.ChannelRec(0, "Clean"),
        D.ChannelRec(1, "Crunch"),
        D.ChannelRec(2, "Lead"),
        D.ChannelRec(3, "Solo Boost / High Gain"),
        D.SwitchingRec(0x01, 120),
    ]
    params = [("Gain", G.KIND_CONTINUOUS, 2048), ("Bass", G.KIND_CONTINUOUS, 2048),
              ("Middle", G.KIND_CONTINUOUS, 2048), ("Treble", G.KIND_CONTINUOUS, 2048),
              ("Presence", G.KIND_CONTINUOUS, 1024), ("Bright", G.KIND_ENUM, 0),
              ("Master Volume", G.KIND_CONTINUOUS, 4095), ("Heater Current (mA)", G.KIND_READONLY, 0)]
    for i, (name, kind, default) in enumerate(params, start=1):
        recs.append(D.ParamRec(i, 0xFF if i != 6 else 3, kind, default, name))
    recs += [D.ParamEnumRec(6, 0, "Off"), D.ParamEnumRec(6, 1, "Bright"), D.ParamEnumRec(6, 2, "Extra Bright")]
    recs += [
        D.AudioRec(0x03, 1, 500, 1200),
        D.PowerLvRec(40, 40, 0, 20),
        D.PowerTubeRec(2, 2, 4, 600, 700, 250, 12, 20),
        D.VendorRec(0x1234, bytes(range(200))),
        D.UnknownRec(0x55, bytes.fromhex("01020304050607")),
        D.UnknownRec(0x56, bytes(120)),
    ]
    return recs


def min_records() -> list:
    return [D.ProtocolRec(1, 0), D.ModuleTypeRec(G.MT_ANALOGUE_EFFECT), D.NameRec("N"), D.ManufacturerRec("M"),
            D.ModelIdRec(1, 1, 1), D.ChannelRec(0, "Clean"), D.SwitchingRec(0x00, 0),
            D.ParamRec(1, 0xFF, G.KIND_CONTINUOUS, 0, "Gain"), D.AudioRec(0x03, 0, 500, 1200),
            D.PowerLvRec(40, 40, 0, 20)]


def _record_fields(rec) -> dict:
    d = {k: (v.hex() if isinstance(v, bytes) else v) for k, v in dataclasses.asdict(rec).items()}
    return {"type": G.TLV_NAMES.get(D.TYPE_OF.get(type(rec), getattr(rec, "type", -1)), "UNKNOWN"), **d}


def _descriptor_entry(name: str, spec_ref: str, recs: list, note: str | None) -> dict:
    blob = D.build_descriptor(recs)
    entry = {"name": name, "kind": "descriptor", "spec_ref": spec_ref,
             "fields": {"records": [_record_fields(r) for r in recs]},
             "canonical": C.descriptor_to_canonical(recs), "bytes": " ".join(f"{b:02X}" for b in blob)}
    if note:
        entry["note"] = note
    return entry


def build() -> list[dict]:
    sample = sample_records()
    sample_blob = D.build_descriptor(sample)
    assert len(sample_blob) >= 600, len(sample_blob)
    entries = [
        _descriptor_entry("descriptor_sample", "protocol-l3 §4, §4.1 (every record type, unknown skipped)", sample,
                          f"{len(sample_blob)} bytes; crc=0x{D.descriptor_crc(sample_blob):04X}"),
        _descriptor_entry("descriptor_min", "protocol-l3 §4.1 required records only", min_records(), None),
    ]
    # IDENTIFY response (FR-008 ruling), desc_len/desc_crc of the sample (FR-034)
    obj = l3.IdentifyResp(1, 0, G.MT_TUBE_PREAMP, len(sample_blob), D.descriptor_crc(sample_blob))
    h0 = l3.Header(G.OP_IDENTIFY, 0x10, 5, G.FLAG_response, 0)
    raw = C.encode_message(h0, obj)
    h = l3.decode_header(raw)
    entries.append({"name": "msg_identify_resp", "kind": "message",
                    "spec_ref": "protocol-l3 §3.1 IDENTIFY response (ruling: docs/OPEN-QUESTIONS.md); desc_crc per §4.1",
                    "fields": {"opcode": "IDENTIFY", "node_id": 0x10, "seq": 5, "flags": G.FLAG_response,
                               **dataclasses.asdict(obj)},
                    "canonical": C.message_to_canonical(h, obj), "bytes": " ".join(f"{b:02X}" for b in raw),
                    "note": "desc_len/desc_crc are those of descriptor_sample"})
    return entries
