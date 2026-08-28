"""Canonical text ⇄ typed messages (reference renderer).

Format: specs/001-protocol-foundation/contracts/canonical-text.md. Both implementations
must render identical strings for identical values — string equality is the semantic
identity test used by tools/diffcheck.py and the golden vectors.
"""
from __future__ import annotations

from typing import Any

from _gen import P
import omgp_l3 as l3

G = P()


class CanonicalError(Exception):
    """Malformed canonical text (a tooling error, never a codec Status)."""


# --- scalar rendering -----------------------------------------------------------------------

def _hex2(v: int) -> str:
    return f"0x{v:02X}"


def _name_or_hex(names: dict[int, str], v: int) -> str:
    name = names.get(v)
    # Range markers (USER_DEFINED_MIN/MAX) are not names of values: render such codes as hex.
    if name is None or name.endswith(("_MIN", "_MAX")):
        return _hex2(v)
    return name


_REV = {k: {n: c for c, n in m.items()} for k, m in (
    ("op", G.OPCODE_NAMES), ("mt", G.MODULE_TYPE_NAMES), ("evt", G.EVENT_NAMES),
    ("err", G.ERROR_NAMES), ("state", G.STATE_NAMES), ("kind", G.KIND_NAMES),
    ("tlv", G.TLV_NAMES))}


def _parse_named(kind: str, tok: str) -> int:
    if tok in _REV[kind]:
        return _REV[kind][tok]
    try:
        return int(tok, 0)
    except ValueError:
        raise CanonicalError(f"{kind}: unknown name {tok!r}") from None


def _int(tok: str) -> int:
    try:
        return int(tok, 0)
    except ValueError:
        raise CanonicalError(f"not an integer: {tok!r}") from None


def _hexbytes(tok: str) -> bytes:
    try:
        return bytes.fromhex(tok)
    except ValueError:
        raise CanonicalError(f"not hex: {tok!r}") from None


def quote_str(raw: bytes) -> str:
    out = ['"']
    for b in raw:
        if b == 0x22:
            out.append('\\"')
        elif b == 0x5C:
            out.append("\\\\")
        elif 0x20 <= b <= 0x7E:
            out.append(chr(b))
        else:
            out.append(f"\\x{b:02x}")
    out.append('"')
    return "".join(out)


def unquote_str(q: str) -> bytes:
    if len(q) < 2 or q[0] != '"' or q[-1] != '"':
        raise CanonicalError(f"not a quoted string: {q!r}")
    out = bytearray()
    i, body = 0, q[1:-1]
    while i < len(body):
        c = body[i]
        if c == "\\":
            nxt = body[i + 1] if i + 1 < len(body) else ""
            if nxt == "x":
                out.append(int(body[i + 2:i + 4], 16))
                i += 4
                continue
            if nxt in ('"', "\\"):
                out.append(ord(nxt))
                i += 2
                continue
            raise CanonicalError(f"bad escape in {q!r}")
        out.append(ord(c))
        i += 1
    return bytes(out)


# --- message payload rendering -------------------------------------------------------------

def _render_payload(obj: Any) -> str:
    if obj is None:
        return ""
    if isinstance(obj, l3.IdentifyResp):
        return (f"major={obj.major} minor={obj.minor} mt={_name_or_hex(G.MODULE_TYPE_NAMES, obj.module_type)} "
                f"desc_len={obj.desc_len} desc_crc=0x{obj.desc_crc:04X}")
    if isinstance(obj, l3.ReadDescReq):
        return f"offset={obj.offset} max_len={obj.max_len}"
    if isinstance(obj, l3.ReadDescResp):
        return f"offset={obj.offset} bytes={obj.data.hex()}"
    if isinstance(obj, l3.SelectChannelReq):
        return f"channel={obj.channel}"
    if isinstance(obj, l3.SetBypassReq):
        return f"bypass={obj.bypass}"
    if isinstance(obj, l3.SetParamReq):
        return f"param_id={obj.param_id} scope={_hex2(obj.scope)} value={obj.value}"
    if isinstance(obj, l3.GetParamReq):
        return f"param_id={obj.param_id} scope={_hex2(obj.scope)}"
    if isinstance(obj, l3.GetParamResp):
        return f"param_id={obj.param_id} scope={_hex2(obj.scope)} value={obj.value}"
    if isinstance(obj, l3.StatusBlock):
        return status_to_canonical(obj)
    if isinstance(obj, l3.GetEventResp):
        return (f"evt={_name_or_hex(G.EVENT_NAMES, obj.event_type)} remaining={obj.remaining_count} "
                f"detail={obj.detail.hex()}")
    if isinstance(obj, l3.OpaquePayload):
        return f"opaque={obj.data.hex()}"
    if isinstance(obj, l3.ErrorResp):
        return f"err={_name_or_hex(G.ERROR_NAMES, obj.code)} detail={obj.detail.hex()}"
    if isinstance(obj, l3.RawPayload):
        return f"raw={obj.data.hex()}"
    raise CanonicalError(f"cannot render {type(obj).__name__}")


def status_to_canonical(s: l3.StatusBlock) -> str:
    return (f"state={_name_or_hex(G.STATE_NAMES, s.state)} channel={s.active_channel} bypass={s.bypass} "
            f"fault={_hex2(s.fault_code)} uptime_s={s.uptime_s} pending={s.event_pending}")


def message_to_canonical(h: l3.Header, obj: Any) -> str:
    base = f"op={_name_or_hex(G.OPCODE_NAMES, h.opcode)} node={_hex2(h.node_id)} seq={h.seq} flags={_hex2(h.flags)}"
    tail = _render_payload(obj)
    return f"{base} {tail}" if tail else base


def error_to_canonical(e: l3.L3Error) -> str:
    return f"ERR {e.status}"


# --- parsing ---------------------------------------------------------------------------------

def _tokens(line: str) -> dict[str, str]:
    kv: dict[str, str] = {}
    for tok in line.strip().split(" "):
        if not tok:
            continue
        if "=" not in tok:
            raise CanonicalError(f"token without '=': {tok!r}")
        k, v = tok.split("=", 1)
        if k in kv:
            raise CanonicalError(f"duplicate key {k!r}")
        kv[k] = v
    return kv


def _take(kv: dict[str, str], key: str) -> str:
    if key not in kv:
        raise CanonicalError(f"missing {key}=")
    return kv.pop(key)


def canonical_to_status(line: str) -> l3.StatusBlock:
    kv = _tokens(line)
    s = l3.StatusBlock(_parse_named("state", _take(kv, "state")), _int(_take(kv, "channel")),
                       _int(_take(kv, "bypass")), _int(_take(kv, "fault")), _int(_take(kv, "uptime_s")),
                       _int(_take(kv, "pending")))
    if kv:
        raise CanonicalError(f"unexpected keys {sorted(kv)}")
    return s


def _parse_payload(opcode_name: str | None, direction: str, kv: dict[str, str]) -> Any:
    if opcode_name is None:
        return l3.RawPayload(_hexbytes(_take(kv, "raw")))
    codec = l3.codec_for(opcode_name, direction)
    if codec is None:
        return None
    cls = codec[0]
    if cls is l3.IdentifyResp:
        return cls(_int(_take(kv, "major")), _int(_take(kv, "minor")), _parse_named("mt", _take(kv, "mt")),
                   _int(_take(kv, "desc_len")), _int(_take(kv, "desc_crc")))
    if cls is l3.ReadDescReq:
        return cls(_int(_take(kv, "offset")), _int(_take(kv, "max_len")))
    if cls is l3.ReadDescResp:
        return cls(_int(_take(kv, "offset")), _hexbytes(_take(kv, "bytes")))
    if cls is l3.SelectChannelReq:
        return cls(_int(_take(kv, "channel")))
    if cls is l3.SetBypassReq:
        return cls(_int(_take(kv, "bypass")))
    if cls is l3.SetParamReq:
        return cls(_int(_take(kv, "param_id")), _int(_take(kv, "scope")), _int(_take(kv, "value")))
    if cls is l3.GetParamReq:
        return cls(_int(_take(kv, "param_id")), _int(_take(kv, "scope")))
    if cls is l3.GetParamResp:
        return cls(_int(_take(kv, "param_id")), _int(_take(kv, "scope")), _int(_take(kv, "value")))
    if cls is l3.StatusBlock:
        return canonical_to_status(" ".join(f"{k}={kv.pop(k)}" for k in
                                            ("state", "channel", "bypass", "fault", "uptime_s", "pending")
                                            if k in kv))
    if cls is l3.GetEventResp:
        return cls(_parse_named("evt", _take(kv, "evt")), _int(_take(kv, "remaining")),
                   _hexbytes(_take(kv, "detail")))
    if cls is l3.OpaquePayload:
        return cls(_hexbytes(_take(kv, "opaque")))
    if cls is l3.ErrorResp:
        return cls(_parse_named("err", _take(kv, "err")), _hexbytes(_take(kv, "detail")))
    raise CanonicalError(f"no parser for {cls.__name__}")


def canonical_to_message(line: str) -> tuple[l3.Header, Any]:
    kv = _tokens(line)
    op_tok = _take(kv, "op")
    node, seq, flags = _int(_take(kv, "node")), _int(_take(kv, "seq")), _int(_take(kv, "flags"))
    if op_tok in _REV["op"]:
        opcode, opcode_name = _REV["op"][op_tok], op_tok
    else:
        opcode = _int(op_tok)
        opcode_name = G.OPCODE_NAMES.get(opcode)
    direction = "response" if flags & G.FLAG_response else "request"
    obj = _parse_payload(opcode_name, direction, kv)
    if kv:
        raise CanonicalError(f"unexpected keys {sorted(kv)}")
    if opcode_name is None:
        payload_len = len(obj.data)
    else:
        payload_len = len(l3.encode_payload(opcode_name, direction, obj))
    return l3.Header(opcode, node, seq, flags, payload_len), obj


# --- whole-message helpers used by vectors, diffcheck and the helper protocol ----------------

def encode_message(h: l3.Header, obj: Any) -> bytes:
    """Header + payload bytes; payload_len is taken from the object, not the header."""
    name = G.OPCODE_NAMES.get(h.opcode)
    payload = obj.data if name is None else l3.encode_payload(name, l3.direction_of(h), obj)
    return l3.encode_header(l3.Header(h.opcode, h.node_id, h.seq, h.flags, len(payload))) + payload


def decode_message(b: bytes) -> tuple[l3.Header, Any]:
    h, payload = l3.decode_message(b)
    name = G.OPCODE_NAMES.get(h.opcode)
    if name is None:
        return h, l3.RawPayload(payload)
    return h, l3.decode_payload(name, l3.direction_of(h), payload)


def render_bytes(b: bytes) -> str:
    """Decode a wire message and render it, or 'ERR <Status>'."""
    try:
        h, obj = decode_message(b)
    except l3.L3Error as e:
        return error_to_canonical(e)
    return message_to_canonical(h, obj)
