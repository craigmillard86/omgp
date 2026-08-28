"""Independent Python reference implementation of the OMGP L3 message codecs.

Written from docs/protocol-l3.md §3 / §3.1 / §3.3 and the 2026-08-28 rulings in
docs/OPEN-QUESTIONS.md (spec 001 data-model.md §2) — not from the C++ code. Protocol
values come only from the generated module (constitution Principle I). Errors are
L3Error(status) where status is the exact C++ Status name, so the differential test
can compare rejection categories as strings.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Any, Callable

from _gen import P

G = P()

HEADER_LEN = 5
_RESERVED_FLAG_MASK = 0xFF & ~(G.FLAG_response | G.FLAG_error)


class L3Error(Exception):
    """A codec rejection. str() starts with the Status name."""

    def __init__(self, status: str, detail: str = ""):
        super().__init__(f"{status}: {detail}" if detail else status)
        self.status = status
        self.detail = detail


def _need(cond: bool, status: str, detail: str = "") -> None:
    if not cond:
        raise L3Error(status, detail)


def _fixed(b: bytes, n: int) -> None:
    _need(len(b) >= n, "Truncated", f"need {n} bytes, have {len(b)}")
    _need(len(b) == n, "LengthMismatch", f"expected {n} bytes, have {len(b)}")


def _pack(fmt: str, *vals: int) -> bytes:
    try:
        return struct.pack(fmt, *vals)
    except struct.error as e:
        raise L3Error("OutOfRange", str(e)) from None


# --- §3 common header --------------------------------------------------------------------

@dataclass(frozen=True)
class Header:
    opcode: int
    node_id: int
    seq: int
    flags: int
    payload_len: int


def encode_header(h: Header) -> bytes:
    _need(not (h.flags & _RESERVED_FLAG_MASK), "ReservedViolation", "reserved flag bits set")
    if not (h.flags & G.FLAG_response):
        _need(h.node_id < G.ADDR_reserved_min, "ReservedViolation", "reserved node id in request")
    _need(h.payload_len <= G.LIMIT_max_l3_payload, "OutOfRange", "payload_len")
    return _pack("<BBBBB", h.opcode, h.node_id, h.seq, h.flags, h.payload_len)


def decode_header(b: bytes) -> Header:
    _need(len(b) >= HEADER_LEN, "Truncated", "header")
    h = Header(*struct.unpack("<BBBBB", bytes(b[:HEADER_LEN])))
    _need(h.payload_len <= G.LIMIT_max_l3_payload, "OutOfRange", "payload_len")
    return h


def decode_message(b: bytes) -> tuple[Header, bytes]:
    h = decode_header(b)
    rest = bytes(b[HEADER_LEN:])
    _need(len(rest) >= h.payload_len, "Truncated", "payload")
    _need(len(rest) == h.payload_len, "LengthMismatch", "payload_len disagrees with bytes")
    return h, rest


# --- §3.1 payloads -------------------------------------------------------------------------

@dataclass(frozen=True)
class IdentifyResp:
    major: int
    minor: int
    module_type: int
    desc_len: int
    desc_crc: int


@dataclass(frozen=True)
class ReadDescReq:
    offset: int
    max_len: int


@dataclass(frozen=True)
class ReadDescResp:
    offset: int
    data: bytes


@dataclass(frozen=True)
class SelectChannelReq:
    channel: int


@dataclass(frozen=True)
class SetBypassReq:
    bypass: int


@dataclass(frozen=True)
class SetParamReq:
    param_id: int
    scope: int
    value: int


@dataclass(frozen=True)
class GetParamReq:
    param_id: int
    scope: int


@dataclass(frozen=True)
class GetParamResp:
    param_id: int
    scope: int
    value: int


@dataclass(frozen=True)
class StatusBlock:  # §3.3
    state: int
    active_channel: int
    bypass: int
    fault_code: int
    uptime_s: int
    event_pending: int


@dataclass(frozen=True)
class GetEventResp:
    event_type: int
    remaining_count: int
    detail: bytes


@dataclass(frozen=True)
class OpaquePayload:  # BP_SLOT_MAP / BP_POWER / BP_ROUTE (format not yet defined)
    data: bytes


@dataclass(frozen=True)
class ErrorResp:
    code: int
    detail: bytes


@dataclass(frozen=True)
class RawPayload:  # payload of an opcode the codec does not know (forwarded verbatim)
    data: bytes


_MAX_PAYLOAD = G.LIMIT_max_l3_payload
_MAX_DESC = G.LIMIT_max_descriptor_bytes


def _check_identify(r: IdentifyResp) -> None:
    _need(r.module_type in G.MODULE_TYPE_NAMES, "OutOfRange", "module_type")
    _need(r.desc_len <= _MAX_DESC, "OutOfRange", "desc_len")


def encode_identify_resp(r: IdentifyResp) -> bytes:
    _check_identify(r)
    return _pack("<BBBHH", r.major, r.minor, r.module_type, r.desc_len, r.desc_crc)


def decode_identify_resp(b: bytes) -> IdentifyResp:
    _fixed(b, 7)
    r = IdentifyResp(*struct.unpack("<BBBHH", b))
    _check_identify(r)
    return r


def encode_read_desc_req(r: ReadDescReq) -> bytes:
    _need(r.offset < _MAX_DESC, "OutOfRange", "offset")
    return _pack("<HB", r.offset, r.max_len)


def decode_read_desc_req(b: bytes) -> ReadDescReq:
    _fixed(b, 3)
    r = ReadDescReq(*struct.unpack("<HB", b))
    _need(r.offset < _MAX_DESC, "OutOfRange", "offset")
    return r


_READ_DESC_TAIL_MAX = _MAX_PAYLOAD - 3


def encode_read_desc_resp(r: ReadDescResp) -> bytes:
    _need(r.offset < _MAX_DESC, "OutOfRange", "offset")
    _need(len(r.data) <= _READ_DESC_TAIL_MAX, "OutOfRange", "len")
    return _pack("<HB", r.offset, len(r.data)) + bytes(r.data)


def decode_read_desc_resp(b: bytes) -> ReadDescResp:
    _need(len(b) >= 3, "Truncated", "read_desc header")
    offset, n = struct.unpack("<HB", b[:3])
    tail = bytes(b[3:])
    _need(len(tail) >= n, "Truncated", "read_desc bytes")
    _need(len(tail) == n, "LengthMismatch", "len disagrees with bytes")
    _need(offset < _MAX_DESC, "OutOfRange", "offset")
    _need(n <= _READ_DESC_TAIL_MAX, "OutOfRange", "len")
    return ReadDescResp(offset, tail)


def encode_select_channel(r: SelectChannelReq) -> bytes:
    return _pack("<B", r.channel)


def decode_select_channel(b: bytes) -> SelectChannelReq:
    _fixed(b, 1)
    return SelectChannelReq(b[0])


def encode_set_bypass(r: SetBypassReq) -> bytes:
    _need(r.bypass in (0, 1), "OutOfRange", "bypass")
    return _pack("<B", r.bypass)


def decode_set_bypass(b: bytes) -> SetBypassReq:
    _fixed(b, 1)
    _need(b[0] in (0, 1), "OutOfRange", "bypass")
    return SetBypassReq(b[0])


def encode_set_param(r: SetParamReq) -> bytes:  # absolute values only (§3, Principle V)
    _need(r.value <= G.LIMIT_param_value_max, "OutOfRange", "value")
    return _pack("<BBH", r.param_id, r.scope, r.value)


def decode_set_param(b: bytes) -> SetParamReq:
    _fixed(b, 4)
    r = SetParamReq(*struct.unpack("<BBH", b))
    _need(r.value <= G.LIMIT_param_value_max, "OutOfRange", "value")
    return r


def encode_get_param_req(r: GetParamReq) -> bytes:
    return _pack("<BB", r.param_id, r.scope)


def decode_get_param_req(b: bytes) -> GetParamReq:
    _fixed(b, 2)
    return GetParamReq(*struct.unpack("<BB", b))


def encode_get_param_resp(r: GetParamResp) -> bytes:
    _need(r.value <= G.LIMIT_param_value_max, "OutOfRange", "value")
    return _pack("<BBH", r.param_id, r.scope, r.value)


def decode_get_param_resp(b: bytes) -> GetParamResp:
    _fixed(b, 4)
    r = GetParamResp(*struct.unpack("<BBH", b))
    _need(r.value <= G.LIMIT_param_value_max, "OutOfRange", "value")
    return r


def _check_status(s: StatusBlock) -> None:
    _need(s.state in G.STATE_NAMES, "OutOfRange", "state")
    _need(s.bypass in (0, 1), "OutOfRange", "bypass")


def encode_status_block(s: StatusBlock) -> bytes:
    _check_status(s)
    return _pack("<BBBBHB", s.state, s.active_channel, s.bypass, s.fault_code, s.uptime_s,
                 s.event_pending)


def decode_status_block(b: bytes) -> StatusBlock:
    _fixed(b, 7)
    s = StatusBlock(*struct.unpack("<BBBBHB", b))
    _check_status(s)
    return s


def _valid_event(t: int) -> bool:
    return t in G.EVENT_NAMES or G.EVT_USER_DEFINED_MIN <= t <= G.EVT_USER_DEFINED_MAX


def encode_get_event_resp(r: GetEventResp) -> bytes:
    _need(_valid_event(r.event_type), "OutOfRange", "event_type")
    _need(len(r.detail) <= _MAX_PAYLOAD - 2, "OutOfRange", "detail")
    return _pack("<BB", r.event_type, r.remaining_count) + bytes(r.detail)


def decode_get_event_resp(b: bytes) -> GetEventResp:
    _need(len(b) >= 2, "Truncated", "event header")
    r = GetEventResp(b[0], b[1], bytes(b[2:]))
    _need(_valid_event(r.event_type), "OutOfRange", "event_type")
    _need(len(r.detail) <= _MAX_PAYLOAD - 2, "OutOfRange", "detail")
    return r


def encode_opaque(r: OpaquePayload) -> bytes:
    _need(len(r.data) <= _MAX_PAYLOAD, "OutOfRange", "opaque length")
    return bytes(r.data)


def decode_opaque(b: bytes) -> OpaquePayload:
    _need(len(b) <= _MAX_PAYLOAD, "OutOfRange", "opaque length")
    return OpaquePayload(bytes(b))


def encode_error_resp(r: ErrorResp) -> bytes:
    _need(r.code in G.ERROR_NAMES, "OutOfRange", "error code")
    _need(len(r.detail) <= _MAX_PAYLOAD - 1, "OutOfRange", "detail")
    return _pack("<B", r.code) + bytes(r.detail)


def decode_error_resp(b: bytes) -> ErrorResp:
    _need(len(b) >= 1, "Truncated", "error code")
    r = ErrorResp(b[0], bytes(b[1:]))
    _need(r.code in G.ERROR_NAMES, "OutOfRange", "error code")
    _need(len(r.detail) <= _MAX_PAYLOAD - 1, "OutOfRange", "detail")
    return r


# --- dispatch ---------------------------------------------------------------------------

Codec = tuple[type, Callable[[Any], bytes], Callable[[bytes], Any]]

_TYPED: dict[tuple[str, str], Codec] = {
    ("IDENTIFY", "response"): (IdentifyResp, encode_identify_resp, decode_identify_resp),
    ("READ_DESC", "request"): (ReadDescReq, encode_read_desc_req, decode_read_desc_req),
    ("READ_DESC", "response"): (ReadDescResp, encode_read_desc_resp, decode_read_desc_resp),
    ("SELECT_CHANNEL", "request"): (SelectChannelReq, encode_select_channel, decode_select_channel),
    ("SET_BYPASS", "request"): (SetBypassReq, encode_set_bypass, decode_set_bypass),
    ("SET_PARAM", "request"): (SetParamReq, encode_set_param, decode_set_param),
    ("GET_PARAM", "request"): (GetParamReq, encode_get_param_req, decode_get_param_req),
    ("GET_PARAM", "response"): (GetParamResp, encode_get_param_resp, decode_get_param_resp),
    ("GET_STATUS", "response"): (StatusBlock, encode_status_block, decode_status_block),
    ("GET_EVENT", "response"): (GetEventResp, encode_get_event_resp, decode_get_event_resp),
    ("ERROR", "response"): (ErrorResp, encode_error_resp, decode_error_resp),
}
_OPAQUE: Codec = (OpaquePayload, encode_opaque, decode_opaque)
_INFO_BY_NAME = {e["name"]: e for e in G.PAYLOAD_INFO}
_INFO_BY_CODE = {e["code"]: e for e in G.PAYLOAD_INFO}
_TARGET = {e["name"]: e["target"] for e in G.OPCODE_INFO}


def codec_for(opcode_name: str, direction: str) -> Codec | None:
    """(type, encode, decode) for a known opcode/direction; None means an empty payload.
    Raises UnknownOpcode for unknown opcodes and for requests to a response-only opcode."""
    info = _INFO_BY_NAME.get(opcode_name)
    _need(info is not None, "UnknownOpcode", opcode_name)
    _need(not (direction == "request" and _TARGET[opcode_name] == "response_only"),
          "UnknownOpcode", f"{opcode_name} is response-only")
    if info["opaque"]:
        return _OPAQUE
    return _TYPED.get((opcode_name, direction))


def payload_bounds(opcode: int, direction: str) -> tuple[int, int, bool]:
    info = _INFO_BY_CODE.get(opcode)
    _need(info is not None, "UnknownOpcode", f"{opcode:#04x}")
    codec_for(info["name"], direction)  # response-only check
    if direction == "request":
        return info["req_min"], info["req_max"], info["opaque"]
    return info["resp_min"], info["resp_max"], info["opaque"]


def encode_payload(opcode_name: str, direction: str, obj: Any) -> bytes:
    codec = codec_for(opcode_name, direction)
    if codec is None:
        _need(obj is None, "OutOfRange", f"{opcode_name} {direction} carries no payload")
        return b""
    cls, enc, _ = codec
    _need(isinstance(obj, cls), "OutOfRange", f"expected {cls.__name__}")
    return enc(obj)


def decode_payload(opcode_name: str, direction: str, b: bytes) -> Any:
    codec = codec_for(opcode_name, direction)
    if codec is None:
        _need(len(b) == 0, "LengthMismatch", f"{opcode_name} {direction} carries no payload")
        return None
    return codec[2](b)


def direction_of(h: Header) -> str:
    return "response" if h.flags & G.FLAG_response else "request"
