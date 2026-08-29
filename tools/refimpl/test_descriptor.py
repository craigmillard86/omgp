"""Reference-implementation tests for the descriptor codec (spec 001 US3, T042).

Blobs are hand-assembled from data-model.md §3.1 / protocol-l3 §4 — never from code.
"""
from __future__ import annotations

import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from _gen import P  # noqa: E402
import omgp_descriptor as D  # noqa: E402
from omgp_l3 import L3Error  # noqa: E402

G = P()


def tlv(t: int, value: bytes) -> bytes:
    return bytes([t, len(value)]) + value


def s(text: str) -> bytes:
    return text.encode()


PROTOCOL = tlv(G.TLV_PROTOCOL, bytes([1, 0]))
MODULE_TYPE = tlv(G.TLV_MODULE_TYPE, bytes([G.MT_TUBE_PREAMP]))
NAME = tlv(G.TLV_NAME, s("N"))
MANUFACTURER = tlv(G.TLV_MANUFACTURER, s("M"))
MODEL_ID = tlv(G.TLV_MODEL_ID, bytes.fromhex("0101 0200 0301"))
CHANNEL0 = tlv(G.TLV_CHANNEL, bytes([0]) + s("Clean"))
SWITCHING = tlv(G.TLV_SWITCHING, bytes.fromhex("01 7800"))
PARAM1 = tlv(G.TLV_PARAM, bytes([1, 0xFF, G.KIND_CONTINUOUS]) + bytes.fromhex("0008") + s("Gain"))
AUDIO = tlv(G.TLV_AUDIO, bytes.fromhex("03 01 F401 B004"))
POWER_LV = tlv(G.TLV_POWER_LV, bytes.fromhex("2800 2800 0000 1400"))
MIN = PROTOCOL + MODULE_TYPE + NAME + MANUFACTURER + MODEL_ID + CHANNEL0 + SWITCHING + PARAM1 + AUDIO + POWER_LV

SERIAL = tlv(G.TLV_SERIAL, s("BP-0001"))
CHANNEL1 = tlv(G.TLV_CHANNEL, bytes([1]) + s("Crunch"))
PARAM_ENUM = tlv(G.TLV_PARAM_ENUM, bytes([3, 0]) + s("Bright"))
POWER_TUBE = tlv(G.TLV_POWER_TUBE, bytes.fromhex("02 02 04 5802 BC02 FA00 0C 14"))
VENDOR = tlv(G.TLV_VENDOR, bytes.fromhex("3412 deadbeef"))
UNKNOWN = tlv(0x55, bytes.fromhex("01020304050607"))
FULL = (PROTOCOL + MODULE_TYPE + NAME + MANUFACTURER + MODEL_ID + SERIAL + CHANNEL0 + CHANNEL1 + SWITCHING +
        PARAM1 + PARAM_ENUM + AUDIO + POWER_LV + POWER_TUBE + VENDOR + UNKNOWN)


def raises(status: str):
    return pytest.raises(L3Error, match=rf"^{status}\b")


def test_minimal_descriptor_parses_to_typed_records():
    recs = D.parse_descriptor(MIN)
    assert recs[0] == D.ProtocolRec(1, 0)
    assert recs[1] == D.ModuleTypeRec(G.MT_TUBE_PREAMP)
    assert recs[2] == D.NameRec("N") and recs[3] == D.ManufacturerRec("M")
    assert recs[4] == D.ModelIdRec(0x0101, 0x0002, 0x0103)
    assert recs[5] == D.ChannelRec(0, "Clean")
    assert recs[6] == D.SwitchingRec(0x01, 120)
    assert recs[7] == D.ParamRec(1, 0xFF, G.KIND_CONTINUOUS, 2048, "Gain")
    assert recs[8] == D.AudioRec(0x03, 1, 500, 1200)
    assert recs[9] == D.PowerLvRec(40, 40, 0, 20)
    assert len(recs) == 10


def test_full_descriptor_round_trips_including_unknown_and_vendor():
    recs = D.parse_descriptor(FULL)
    assert D.SerialRec("BP-0001") in recs
    assert D.ParamEnumRec(3, 0, "Bright") in recs
    assert D.PowerTubeRec(2, 2, 4, 600, 700, 250, 12, 20) in recs
    assert D.VendorRec(0x1234, bytes.fromhex("deadbeef")) in recs
    assert recs[-1] == D.UnknownRec(0x55, bytes.fromhex("01020304050607"))
    assert D.build_descriptor(recs) == FULL
    assert D.build_descriptor(D.parse_descriptor(MIN)) == MIN


def test_validate_report_counts():
    r = D.validate_descriptor(FULL)
    assert r.status == "Ok" and r.skipped_unknown == 1 and r.channel_count == 2 and r.param_count == 1
    r = D.validate_descriptor(MIN)
    assert (r.status, r.skipped_unknown, r.channel_count, r.param_count) == ("Ok", 0, 1, 1)


def test_unknown_type_is_skipped_by_length_and_later_records_survive():
    blob = PROTOCOL + UNKNOWN + MODULE_TYPE + NAME + MANUFACTURER + MODEL_ID + CHANNEL0 + SWITCHING + PARAM1 + AUDIO + POWER_LV
    recs = D.parse_descriptor(blob)
    assert recs[1] == D.UnknownRec(0x55, bytes.fromhex("01020304050607"))
    assert recs[-1] == D.PowerLvRec(40, 40, 0, 20)
    assert D.validate_descriptor(blob).skipped_unknown == 1


@pytest.mark.parametrize("name,chunk", [("PROTOCOL", PROTOCOL), ("MODULE_TYPE", MODULE_TYPE), ("NAME", NAME),
                                        ("MANUFACTURER", MANUFACTURER), ("MODEL_ID", MODEL_ID),
                                        ("CHANNEL", CHANNEL0), ("SWITCHING", SWITCHING), ("PARAM", PARAM1),
                                        ("AUDIO", AUDIO), ("POWER_LV", POWER_LV)])
def test_missing_required_record_names_it(name, chunk):
    blob = MIN.replace(chunk, b"", 1)
    with raises("MissingRequired") as e:
        D.parse_descriptor(blob)
    assert name in str(e.value)
    r = D.validate_descriptor(blob)
    assert r.status == "MissingRequired" and r.type == getattr(G, f"TLV_{name}") and r.offset == len(blob)


def test_optional_records_may_be_absent():
    assert D.validate_descriptor(MIN).status == "Ok"  # no SERIAL, PARAM_ENUM, POWER_TUBE, VENDOR


def test_duplicate_non_repeated_record():
    with raises("DuplicateRecord"):
        D.parse_descriptor(MIN + PROTOCOL)
    r = D.validate_descriptor(MIN + PROTOCOL)
    assert r.status == "DuplicateRecord" and r.type == G.TLV_PROTOCOL and r.offset == len(MIN)
    # repeated types may repeat
    assert D.validate_descriptor(MIN + CHANNEL1 + PARAM_ENUM + PARAM_ENUM + VENDOR + VENDOR).status == "Ok"


def test_string_limits_from_tlv_info():
    ok = MIN.replace(NAME, tlv(G.TLV_NAME, b"x" * 24), 1)
    assert D.validate_descriptor(ok).status == "Ok"
    with raises("StringTooLong"):
        D.parse_descriptor(MIN.replace(NAME, tlv(G.TLV_NAME, b"x" * 25), 1))
    with raises("StringTooLong"):
        D.parse_descriptor(MIN + tlv(G.TLV_SERIAL, b"s" * 17))
    assert D.validate_descriptor(MIN + tlv(G.TLV_SERIAL, b"s" * 16)).status == "Ok"
    with raises("StringTooLong"):
        D.build_descriptor([D.NameRec("x" * 25)] + D.parse_descriptor(MIN)[1:])
    assert D.validate_descriptor(MIN.replace(NAME, tlv(G.TLV_NAME, b""), 1)).status == "Ok"  # empty allowed


def test_blob_cap_is_checked_before_any_record():
    pad = MIN
    while len(pad) < G.LIMIT_max_descriptor_bytes - 2:
        pad += tlv(0x60, b"\x00" * min(200, G.LIMIT_max_descriptor_bytes - 2 - len(pad)))
    pad += tlv(0x61, b"") if len(pad) == G.LIMIT_max_descriptor_bytes - 2 else b""
    assert len(pad) == G.LIMIT_max_descriptor_bytes
    assert D.validate_descriptor(pad).status == "Ok"
    over = b"\xFF" + pad  # first "record" is garbage AND the blob is one byte too long
    r = D.validate_descriptor(over)
    assert r.status == "BlobTooLarge" and r.offset == 0
    with raises("BlobTooLarge"):
        D.parse_descriptor(over)
    with raises("BlobTooLarge"):
        D.build_descriptor(D.parse_descriptor(pad) + [D.UnknownRec(0x62, b"")])


def test_truncation_never_reads_past_the_blob():
    with raises("Truncated"):
        D.parse_descriptor(MIN[:-1])  # last record's len overruns
    with raises("Truncated"):
        D.parse_descriptor(MIN + b"\x55")  # lone type byte, no len
    r = D.validate_descriptor(MIN + bytes([0x55, 10, 1, 2]))
    assert r.status == "Truncated" and r.type == 0x55 and r.offset == len(MIN)


def test_invalid_utf8_rejected_on_parse_and_build():
    with raises("InvalidUtf8"):
        D.parse_descriptor(MIN.replace(NAME, tlv(G.TLV_NAME, b"\xff\xfe"), 1))
    with raises("InvalidUtf8"):
        D.parse_descriptor(MIN.replace(NAME, tlv(G.TLV_NAME, b"\xc0\xaf"), 1))  # overlong
    with raises("InvalidUtf8"):
        D.parse_descriptor(MIN.replace(NAME, tlv(G.TLV_NAME, b"\xed\xa0\x80"), 1))  # surrogate
    assert D.parse_descriptor(MIN.replace(NAME, tlv(G.TLV_NAME, "Ré".encode()), 1))[2] == D.NameRec("Ré")


def test_malformed_fixed_length_records():
    with raises("MalformedRecord"):
        D.parse_descriptor(MIN.replace(PROTOCOL, tlv(G.TLV_PROTOCOL, b"\x01"), 1))
    with raises("MalformedRecord"):
        D.parse_descriptor(MIN.replace(CHANNEL0, tlv(G.TLV_CHANNEL, b""), 1))
    with raises("MalformedRecord"):
        D.parse_descriptor(MIN.replace(SWITCHING, tlv(G.TLV_SWITCHING, b"\x01\x78\x00\x00"), 1))
    with raises("MalformedRecord"):
        D.parse_descriptor(MIN.replace(PARAM1, tlv(G.TLV_PARAM, b"\x01\xff\x00\x00"), 1))


def test_value_ranges():
    with raises("OutOfRange"):
        D.parse_descriptor(MIN.replace(MODULE_TYPE, tlv(G.TLV_MODULE_TYPE, b"\x63"), 1))
    with raises("OutOfRange"):
        D.parse_descriptor(MIN.replace(PARAM1, tlv(G.TLV_PARAM, bytes([1, 0xFF, 6]) + b"\x00\x08G"), 1))
    with raises("OutOfRange"):
        D.parse_descriptor(MIN.replace(PARAM1, tlv(G.TLV_PARAM, bytes([1, 0xFF, 0]) + b"\x00\x10G"), 1))  # 4096
    with raises("OutOfRange"):
        D.parse_descriptor(MIN.replace(AUDIO, tlv(G.TLV_AUDIO, bytes.fromhex("03 02 F401 B004")), 1))
    with raises("OutOfRange"):
        D.parse_descriptor(MIN + tlv(G.TLV_POWER_TUBE, bytes.fromhex("00 02 04 5802 BC02 FA00 0C 14")))
    with raises("OutOfRange"):
        D.parse_descriptor(MIN + tlv(G.TLV_POWER_TUBE, bytes.fromhex("05 02 04 5802 BC02 FA00 0C 14")))
    with raises("OutOfRange"):
        D.build_descriptor([D.ProtocolRec(1, 0), D.ModuleTypeRec(0x63)])


def test_builder_enforces_the_same_rules():
    with raises("DuplicateRecord"):
        D.build_descriptor(D.parse_descriptor(MIN) + [D.ProtocolRec(1, 0)])
    with raises("MissingRequired"):
        D.build_descriptor([D.ProtocolRec(1, 0)])
    with raises("InvalidUtf8"):
        D.build_descriptor([D.NameRec("\udc80")] + D.parse_descriptor(MIN)[1:] + [D.NameRec("x")][:0])


def test_iter_records_is_raw_and_lazy():
    views = list(D.iter_records(FULL))
    assert views[0] == D.Record(G.TLV_PROTOCOL, bytes([1, 0]), 0)
    assert views[-1].type == 0x55 and views[-1].offset == len(FULL) - 9
    with raises("Truncated"):
        list(D.iter_records(MIN[:-1]))


def test_descriptor_crc_is_ccitt_false_over_the_blob():
    assert D.descriptor_crc(b"123456789") == 0x29B1
    assert D.descriptor_crc(b"") == 0xFFFF
    assert D.descriptor_crc(FULL) == D.descriptor_crc(bytes(FULL))
