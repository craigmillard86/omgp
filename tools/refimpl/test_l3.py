"""Reference-implementation tests for L3 header + payload codecs (spec 001 US2, T028).

Byte expectations are hand-computed from data-model.md §2 / protocol-l3 §3 — never from
the implementation. Symbols come from the generated module (single source of truth).
"""
from __future__ import annotations

import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from _gen import P  # noqa: E402
import omgp_l3 as l3  # noqa: E402

G = P()
H = l3.Header


def hx(s: str) -> bytes:
    return bytes.fromhex(s.replace(" ", ""))


def raises(status: str):
    return pytest.raises(l3.L3Error, match=rf"^{status}\b")


# --- header (§3: opcode, node_id, seq, flags, payload_len) --------------------------------

def test_header_layout():
    assert l3.encode_header(H(G.OP_SET_PARAM, 0x10, 3, 0, 4)) == hx("12 10 03 00 04")
    assert l3.decode_header(hx("12 10 03 00 04")) == H(0x12, 0x10, 3, 0, 4)


def test_header_reserved_flag_bits_refused_on_encode_preserved_on_decode():
    with raises("ReservedViolation"):
        l3.encode_header(H(G.OP_PING, 1, 0, 0x04, 0))
    assert l3.decode_header(hx("01 01 00 04 00")).flags == 0x04


def test_header_reserved_node_id_refused_for_requests_only():
    with raises("ReservedViolation"):
        l3.encode_header(H(G.OP_PING, G.ADDR_reserved_min, 0, 0, 0))
    # a response *from* 0x80 decodes fine; encoding a response with it is allowed too
    assert l3.encode_header(H(G.OP_PING, G.ADDR_reserved_min, 0, G.FLAG_response, 0)) == hx("01 80 00 01 00")
    assert l3.decode_header(hx("01 80 00 00 00")).node_id == 0x80


def test_header_payload_len_bound():
    assert l3.encode_header(H(G.OP_BP_POWER, 2, 0, 0, 64))[4] == 64
    with raises("OutOfRange"):
        l3.encode_header(H(G.OP_BP_POWER, 2, 0, 0, 65))
    with raises("OutOfRange"):
        l3.decode_header(hx("21 02 00 00 41"))
    with raises("Truncated"):
        l3.decode_header(hx("21 02 00 00"))


def test_decode_message_delimits_payload():
    h, p = l3.decode_message(hx("12 10 03 00 04 01 FF FF 0F"))
    assert h.payload_len == 4 and p == hx("01 FF FF 0F")
    with raises("Truncated"):
        l3.decode_message(hx("12 10 03 00 04 01 FF"))
    with raises("LengthMismatch"):
        l3.decode_message(hx("12 10 03 00 04 01 FF FF 0F 00"))


# --- payloads ---------------------------------------------------------------------------

def test_set_param_is_absolute_and_bounded():
    assert l3.encode_set_param(l3.SetParamReq(1, 0xFF, 4095)) == hx("01 FF FF 0F")
    assert l3.decode_set_param(hx("01 FF FF 0F")) == l3.SetParamReq(1, 0xFF, 4095)
    with raises("OutOfRange"):
        l3.encode_set_param(l3.SetParamReq(1, 0xFF, 4096))
    with raises("OutOfRange"):
        l3.decode_set_param(hx("01 FF 00 10"))
    with raises("Truncated"):
        l3.decode_set_param(hx("01 FF FF"))
    with raises("LengthMismatch"):
        l3.decode_set_param(hx("01 FF FF 0F 00"))


def test_identify_response_layout():
    r = l3.IdentifyResp(1, 0, G.MT_TUBE_PREAMP, 612, 0x4A3F)
    assert l3.encode_identify_resp(r) == hx("01 00 03 64 02 3F 4A")
    assert l3.decode_identify_resp(hx("01 00 03 64 02 3F 4A")) == r
    with raises("OutOfRange"):
        l3.decode_identify_resp(hx("01 00 63 64 02 3F 4A"))  # module type 0x63 undefined
    with raises("OutOfRange"):
        l3.encode_identify_resp(l3.IdentifyResp(1, 0, G.MT_TUBE_PREAMP, 2049, 0))


def test_read_desc_request_and_response():
    assert l3.encode_read_desc_req(l3.ReadDescReq(1987, 61)) == hx("C3 07 3D")
    assert l3.decode_read_desc_req(hx("C3 07 3D")) == l3.ReadDescReq(1987, 61)
    with raises("OutOfRange"):
        l3.encode_read_desc_req(l3.ReadDescReq(2048, 1))
    tail = bytes(range(61))
    enc = l3.encode_read_desc_resp(l3.ReadDescResp(1987, tail))
    assert enc == hx("C3 07 3D") + tail and len(enc) == 64
    assert l3.decode_read_desc_resp(enc) == l3.ReadDescResp(1987, tail)
    with raises("OutOfRange"):
        l3.encode_read_desc_resp(l3.ReadDescResp(0, bytes(62)))
    with raises("Truncated"):
        l3.decode_read_desc_resp(hx("00 00 05 01 02"))
    with raises("LengthMismatch"):
        l3.decode_read_desc_resp(hx("00 00 01 01 02"))


def test_small_requests():
    assert l3.encode_select_channel(l3.SelectChannelReq(1)) == hx("01")
    assert l3.encode_set_bypass(l3.SetBypassReq(1)) == hx("01")
    with raises("OutOfRange"):
        l3.encode_set_bypass(l3.SetBypassReq(2))
    with raises("OutOfRange"):
        l3.decode_set_bypass(hx("02"))
    assert l3.encode_get_param_req(l3.GetParamReq(2, 0)) == hx("02 00")
    assert l3.encode_get_param_resp(l3.GetParamResp(2, 0, 2048)) == hx("02 00 00 08")
    with raises("OutOfRange"):
        l3.decode_get_param_resp(hx("02 00 00 10"))


def test_status_block_layout():
    sb = l3.StatusBlock(G.STATE_READY, 2, 0, 0, 77, 1)
    assert l3.encode_status_block(sb) == hx("01 02 00 00 4D 00 01")
    assert l3.decode_status_block(hx("01 02 00 00 4D 00 01")) == sb
    with raises("OutOfRange"):
        l3.decode_status_block(hx("05 02 00 00 4D 00 01"))
    with raises("OutOfRange"):
        l3.decode_status_block(hx("01 02 02 00 4D 00 01"))
    with raises("Truncated"):
        l3.decode_status_block(hx("01 02 00 00 4D 00"))


def test_get_event_response():
    assert l3.encode_get_event_resp(l3.GetEventResp(G.EVT_NONE, 0, b"")) == hx("00 00")
    assert l3.decode_get_event_resp(hx("00 00")) == l3.GetEventResp(G.EVT_NONE, 0, b"")
    assert l3.encode_get_event_resp(l3.GetEventResp(G.EVT_CHANNEL_SETTLED, 2, hx("01"))) == hx("01 02 01")
    user = l3.GetEventResp(G.EVT_USER_DEFINED_MIN, 0, hx("DE AD BE EF"))
    assert l3.decode_get_event_resp(hx("F0 00 DE AD BE EF")) == user
    with raises("OutOfRange"):
        l3.decode_get_event_resp(hx("63 00"))  # 0x63 is no event type
    with raises("OutOfRange"):
        l3.encode_get_event_resp(l3.GetEventResp(G.EVT_NONE, 0, bytes(63)))
    with raises("Truncated"):
        l3.decode_get_event_resp(hx("00"))


def test_error_response():
    assert l3.encode_error_resp(l3.ErrorResp(G.ERR_BUSY, b"")) == hx("04")
    assert l3.decode_error_resp(hx("02 04 05")) == l3.ErrorResp(G.ERR_BAD_PAYLOAD, hx("04 05"))
    with raises("OutOfRange"):
        l3.decode_error_resp(hx("63"))
    with raises("Truncated"):
        l3.decode_error_resp(b"")


def test_opaque_backplane_payloads_pass_through():
    assert l3.encode_opaque(l3.OpaquePayload(hx("01 FF 00"))) == hx("01 FF 00")
    assert l3.decode_opaque(hx("01 FF 00")) == l3.OpaquePayload(hx("01 FF 00"))
    assert l3.decode_opaque(b"") == l3.OpaquePayload(b"")
    with raises("OutOfRange"):
        l3.encode_opaque(l3.OpaquePayload(bytes(65)))


def test_empty_payloads():
    assert l3.encode_payload("PING", "request", None) == b""
    assert l3.decode_payload("PING", "request", b"") is None
    with raises("LengthMismatch"):
        l3.decode_payload("PING", "request", hx("00"))
    assert l3.decode_payload("SET_PARAM", "response", b"") is None


def test_payload_bounds_and_dispatch():
    assert l3.payload_bounds(G.OP_SET_PARAM, "request") == (4, 4, False)
    assert l3.payload_bounds(G.OP_GET_EVENT, "response") == (2, G.LIMIT_max_l3_payload, False)
    assert l3.payload_bounds(G.OP_BP_POWER, "request") == (0, G.LIMIT_max_l3_payload, True)
    with raises("UnknownOpcode"):
        l3.payload_bounds(0x63, "request")
    with raises("UnknownOpcode"):
        l3.payload_bounds(G.RESERVED_FIRMWARE_UPDATE_MIN, "request")
    with raises("UnknownOpcode"):
        l3.payload_bounds(G.OP_ERROR, "request")  # response-only
    with raises("UnknownOpcode"):
        l3.payload_bounds(0x00, "request")
    assert l3.decode_payload("SET_PARAM", "request", hx("01 FF FF 0F")) == l3.SetParamReq(1, 0xFF, 4095)
    assert l3.encode_payload("GET_STATUS", "response", l3.StatusBlock(1, 2, 0, 0, 77, 1)) == hx("01 02 00 00 4D 00 01")
