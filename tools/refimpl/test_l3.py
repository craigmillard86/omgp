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
    # F10 (#110/#154): LIMIT_max_l3_payload is 59 (was 64, before the max_l3_message split).
    at_max = G.LIMIT_max_l3_payload
    assert l3.encode_header(H(G.OP_BP_POWER, 2, 0, 0, at_max))[4] == at_max
    with raises("OutOfRange"):
        l3.encode_header(H(G.OP_BP_POWER, 2, 0, 0, at_max + 1))
    with raises("OutOfRange"):
        l3.decode_header(bytes([0x21, 0x02, 0x00, 0x00, at_max + 1]))
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
    # tail bound = LIMIT_max_l3_payload - 3 (offset u16 + len u8); F10, #110/#154. The
    # response's own `len` byte (tail_max) is NOT the request's max_len (61) above — they
    # only coincided by accident at the old 64-byte cap (both were 61).
    tail_max = G.LIMIT_max_l3_payload - 3
    tail = bytes(range(tail_max))
    enc = l3.encode_read_desc_resp(l3.ReadDescResp(1987, tail))
    assert enc == hx("C3 07") + bytes([tail_max]) + tail and len(enc) == G.LIMIT_max_l3_payload
    assert l3.decode_read_desc_resp(enc) == l3.ReadDescResp(1987, tail)
    with raises("OutOfRange"):
        l3.encode_read_desc_resp(l3.ReadDescResp(0, bytes(tail_max + 1)))
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
    # detail max = LIMIT_max_l3_payload - 2 (event_type + remaining_count); F10, #110/#154.
    with raises("OutOfRange"):
        l3.encode_get_event_resp(l3.GetEventResp(G.EVT_NONE, 0, bytes(G.LIMIT_max_l3_payload - 1)))
    with raises("Truncated"):
        l3.decode_get_event_resp(hx("00"))


def test_error_response():
    assert l3.encode_error_resp(l3.ErrorResp(G.ERR_BUSY, b"")) == hx("04")
    assert l3.decode_error_resp(hx("02 04 05")) == l3.ErrorResp(G.ERR_BAD_PAYLOAD, hx("04 05"))
    with raises("OutOfRange"):
        l3.decode_error_resp(hx("63"))
    with raises("Truncated"):
        l3.decode_error_resp(b"")
    # F2 (#110/#154): detail has its own 4-byte cap, not LIMIT_max_l3_payload - 1.
    at_max = G.LIMIT_error_detail_max
    assert l3.encode_error_resp(l3.ErrorResp(G.ERR_BUSY, bytes(at_max))) == bytes([G.ERR_BUSY]) + bytes(at_max)
    with raises("OutOfRange"):
        l3.encode_error_resp(l3.ErrorResp(G.ERR_BUSY, bytes(at_max + 1)))
    with raises("OutOfRange"):
        l3.decode_error_resp(bytes([G.ERR_BUSY]) + bytes(at_max + 1))


def test_opaque_backplane_payloads_pass_through():
    assert l3.encode_opaque(l3.OpaquePayload(hx("01 FF 00"))) == hx("01 FF 00")
    assert l3.decode_opaque(hx("01 FF 00")) == l3.OpaquePayload(hx("01 FF 00"))
    assert l3.decode_opaque(b"") == l3.OpaquePayload(b"")
    with raises("OutOfRange"):
        l3.encode_opaque(l3.OpaquePayload(bytes(G.LIMIT_max_l3_payload + 1)))


def test_bp_slot_map_round_trips_at_zero_one_eight_and_the_cap():
    for slot_count in (0, 1, 8, G.LIMIT_bp_slot_map_max_slots):
        blen = (slot_count + 7) // 8
        occ, chg = bytes([0xA5]) * blen, bytes([0x5A]) * blen
        r = l3.BpSlotMapResp(slot_count, occ, chg)
        enc = l3.encode_bp_slot_map_resp(r)
        assert len(enc) == 1 + 2 * blen and enc[0] == slot_count
        assert l3.decode_bp_slot_map_resp(enc) == r


def test_bp_slot_map_over_the_cap_is_out_of_range_encode_and_decode():
    over = G.LIMIT_bp_slot_map_max_slots + 1
    blen = (over + 7) // 8
    with raises("OutOfRange"):
        l3.encode_bp_slot_map_resp(l3.BpSlotMapResp(over, bytes(blen), bytes(blen)))
    with raises("OutOfRange"):
        l3.decode_bp_slot_map_resp(bytes([over]) + bytes(2 * blen))


def test_bp_slot_map_over_cap_slot_count_is_out_of_range_even_off_a_short_wire_legal_payload():
    """Red team, PR #773 round 1: a payload this short can't legally carry slot_count=233's
    own claimed shape (61 bytes, more than LIMIT_max_l3_payload itself), so decoding it must
    not be able to answer Truncated instead -- the cap check runs off the single slot_count
    byte, before any length check."""
    over = G.LIMIT_bp_slot_map_max_slots + 1
    with raises("OutOfRange"):
        l3.decode_bp_slot_map_resp(bytes([over]))
    at_max_payload = bytes([over]) + bytes(G.LIMIT_max_l3_payload - 1)
    with raises("OutOfRange"):
        l3.decode_bp_slot_map_resp(at_max_payload)


def test_bp_slot_map_reserved_padding_bits_beyond_slot_count_are_passed_through():
    """slot_count=4 needs one bitmap byte; bits 4-7 are reserved padding (golden rule 7:
    forward compatibility, not a shape violation to reject)."""
    wire = bytes([4, 0xF0, 0x00])
    d = l3.decode_bp_slot_map_resp(wire)
    assert (d.slot_count, d.occupied) == (4, bytes([0xF0]))
    assert l3.encode_bp_slot_map_resp(d) == wire


def test_bp_slot_map_encoder_rejects_a_bitmap_length_that_disagrees_with_slot_count():
    with raises("OutOfRange"):
        l3.encode_bp_slot_map_resp(l3.BpSlotMapResp(8, bytes(2), bytes(1)))
    with raises("OutOfRange"):
        l3.encode_bp_slot_map_resp(l3.BpSlotMapResp(8, bytes(1), bytes(2)))


def test_bp_slot_map_decoder_truncated_and_length_mismatch():
    with raises("Truncated"):
        l3.decode_bp_slot_map_resp(b"")
    with raises("Truncated"):
        l3.decode_bp_slot_map_resp(bytes([8, 0xFF]))  # needs 1 + 2*1 = 3
    with raises("LengthMismatch"):
        l3.decode_bp_slot_map_resp(bytes([8, 0xFF, 0xFF, 0x00]))  # one more than 3
    d = l3.decode_bp_slot_map_resp(bytes([0]))
    assert d == l3.BpSlotMapResp(0, b"", b"")


def test_bp_slot_map_payload_bounds_matches_generated_payload_info():
    assert l3.payload_bounds(G.OP_BP_SLOT_MAP, "request") == (0, 0, False)
    assert l3.payload_bounds(G.OP_BP_SLOT_MAP, "response") == (1, 1 + 2 * 29, False)


def test_bp_slot_map_worst_case_size_at_the_cap_fits_max_l3_payload():
    # AC4 (#676): computed from the generated constants, not restated as a literal.
    worst_case_bitmap_len = (G.LIMIT_bp_slot_map_max_slots + 7) // 8
    assert 1 + 2 * worst_case_bitmap_len <= G.LIMIT_max_l3_payload
    one_more_bitmap_len = (G.LIMIT_bp_slot_map_max_slots + 1 + 7) // 8
    assert 1 + 2 * one_more_bitmap_len > G.LIMIT_max_l3_payload


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
