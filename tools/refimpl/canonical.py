"""Canonical text ⇄ typed messages (reference renderer).

Format: specs/001-protocol-foundation/contracts/canonical-text.md. Both implementations
must render identical strings for identical values — string equality is the semantic
identity test used by tools/diffcheck.py and the golden vectors.
"""
from __future__ import annotations

import re
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
    # round 13 on #121: the numeric fallback routes through _int so named-table fields
    # (mt/evt/err/state/kind/tlv) inherit the round-12 whitespace/NUL rejection — C++'s
    # parse_named falls through to the hardened parse_uint, and "\t3" must not name 3
    # here while it is ERR BadRequest there.
    try:
        return _int(tok)
    except CanonicalError:
        raise CanonicalError(f"{kind}: unknown name {tok!r}") from None


def _int(tok: str) -> int:
    # round 12 on #121: C++ parse_uint now rejects any token containing whitespace or an
    # embedded NUL (strtoul skipped leading whitespace — re-enabling legacy octal, so
    # "\t011" named 9 — and c_str() truncated at NUL). int(tok, 0) tolerates both; mirror
    # the rejection here so every verb sharing these parsers stays in lockstep (round 13:
    # _parse_named routes its numeric fallback here and _tokens rejects non-space
    # whitespace at line level, closing the two layers this claim originally missed).
    if any(c.isspace() or c == "\0" for c in tok):
        raise CanonicalError(f"whitespace/NUL inside integer token: {tok!r}")
    # round 15 on #121: int(tok, 0) also accepted 0o/0b/1_0 and non-ASCII decimal digits
    # that parse_uint rejects -- closed for frame lines in round 9, now closed here too by
    # the same compiled grammar, so message/status/descriptor fields are in lockstep.
    if not _FRAME_UINT_RE.fullmatch(tok):
        raise CanonicalError(f"not an integer: {tok!r}")
    digits = tok.lstrip("+")
    return int(digits, 16) if digits[:2].lower() == "0x" else int(digits, 10)


def _hexbytes(tok: str) -> bytes:
    # bytes.fromhex silently tolerates ASCII whitespace INSIDE the token; the C++ hex
    # parsers do not (red-team round 8 on #121: "01\t02" parsed here, ERR BadRequest
    # there). Contiguous hex only.
    if any(c.isspace() for c in tok):
        raise CanonicalError(f"not hex: {tok!r}")
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
    # round 13 on #121 (review): line-level strip() removed leading/trailing TAB/VT/FF/CR
    # for every non-frame verb before any token parser could reject them, while the C++
    # tokenizer keys "\top" as a token (take() fails) or leaves the whitespace inside the
    # last token (parse_uint rejects it since round 12). Reject non-space whitespace up
    # front; SPACES stay fine on both sides — both tokenizers skip empty tokens.
    # round 15 on #121: pop exactly ONE trailing CR first -- l3_helper's reader does this
    # for EVERY verb, so a CRLF-terminated message line must parse here too (round 13's
    # guard alone made the reference STRICTER than the C++ end-to-end path). A second CR
    # still rejects below, exactly as it does through the helper.
    if line.endswith("\r"):
        line = line[:-1]
    if any(c.isspace() and c != " " for c in line):
        raise CanonicalError(f"non-space whitespace in canonical line: {line!r}")
    kv: dict[str, str] = {}
    for tok in line.split(" "):
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


# --- frames (specs/002-trunk-link-layer/contracts/frame-vectors.md "Canonical frame line") ---

import omgp_link as link  # noqa: E402  (after the message half so import order is explicit)


def frame_to_canonical(f: link.Frame) -> str:
    flags = (0x01 if f.response else 0) | (0x02 if f.retry else 0)
    return f"frame dst={_hex2(f.dst)} src={_hex2(f.src)} flags={_hex2(flags)} seq={f.seq} payload={f.payload.hex()}"


# parse_uint's grammar, compiled once (review round 11 on #121: _frame_uint runs 4x per
# frame line, ~40k times per diffcheck run — no per-call import/regex-cache lookups):
# optional leading '+', ASCII 0x-hex or decimal, leading-zero decimal only when every
# digit is zero. Semantics documented at _frame_uint below.
_FRAME_UINT_RE = re.compile(r"\+?(0[xX][0-9a-fA-F]+|0+|[1-9][0-9]*)")


def canonical_to_frame(line: str) -> link.Frame:
    # Exactly ONE trailing '\r' is popped — the l3_helper line reader's behaviour, no more
    # (reviews on #121, rounds 4/8/9: strip(), then rstrip(), then rstrip("\r\n") were each
    # broader than that; a doubled CR must reject like any other stray whitespace).
    # parse_frame_line compares the FIRST token to "frame", so leading space rejects too.
    # (Round 15 closed the former shared-parser gap: _int now enforces the same ASCII
    # grammar _frame_uint uses, so 0o/0b/1_0 and non-ASCII digits reject on every verb.)
    if line.endswith("\r"):
        line = line[:-1]
    prefix, _, rest = line.partition(" ")
    if prefix != "frame":
        raise CanonicalError(f"not a frame line: {line!r}")
    # The C++ tokenizer splits on SPACES only; any other whitespace lands inside a token
    # (true for the uint path since round 12 — before that, strtoul silently SKIPPED
    # leading whitespace inside a token, so this guard was load-bearing on its own)
    # and fails its uint/hex parse (red-team round 8 on #121: trailing TAB/VT/FF parsed
    # here because the shared _tokens() strips them). Reject before tokenizing.
    if any(c.isspace() and c != " " for c in rest):
        raise CanonicalError(f"non-space whitespace in frame line: {line!r}")
    kv = _tokens(rest)

    def _frame_uint(tok: str) -> int:
        # parse_uint's EXACT shape (rounds 9+10 on #121 — round 9's first cut both over-
        # and under-shot it): optional leading '+' (C++ deliberately accepts it, red-team
        # @ 65922b5), no '-', ASCII 0x-hex or decimal, and a leading-zero decimal rejects
        # UNLESS every digit is zero ("0"/"00" name 0 on both sides; "010" must not
        # silently rename itself to 10). No 0o/0b/underscore/Unicode forms. Mirrors
        # tools/canonical.cpp parse_uint, including its digits_start-after-'+' scan.
        if not _FRAME_UINT_RE.fullmatch(tok):
            raise CanonicalError(f"not a frame uint: {tok!r}")
        digits = tok.lstrip("+")
        return int(digits, 16) if digits[:2].lower() == "0x" else int(digits, 10)

    dst, src = _frame_uint(_take(kv, "dst")), _frame_uint(_take(kv, "src"))
    flags, seq = _frame_uint(_take(kv, "flags")), _frame_uint(_take(kv, "seq"))
    payload = _hexbytes(_take(kv, "payload"))
    if kv:
        raise CanonicalError(f"unexpected keys {sorted(kv)}")
    # trunk §4: dst/src are single wire bytes, flags occupies ctrl bits 0-1 (bits 2-3
    # reserved-0, bits 4-7 are `seq`, supplied separately by this field), seq is a nibble,
    # and payload length must fit the one-byte wire length field. Reject out-of-range
    # fields here, at parse time, instead of masking (dropping high bits) or falling
    # through to a bare ValueError out of encode_frame's byte packing — matches
    # tools/canonical.cpp's parse_frame_line (docs/OPEN-QUESTIONS.md 2026-09-03 "Frame line
    # out-of-range fields"). dst=0xFF (reserved, in-range) and payload lengths 65-0xFF
    # (in-range here, refused by encode_frame's own PayloadTooLong) are deliberately left
    # for encode_frame's own checks, matching parse_frame_line's layering.
    if not (0 <= dst <= 0xFF and 0 <= src <= 0xFF and 0 <= flags <= 0x03 and 0 <= seq <= 0x0F
            and len(payload) <= 0xFF):
        raise CanonicalError(f"out-of-range frame field in {line!r}")
    return link.Frame(dst=dst, src=src, response=bool(flags & 0x01), retry=bool(flags & 0x02), seq=seq,
                       payload=payload)


def frame_error_to_canonical(reason: str) -> str:
    """Render a Deframer discard reason or an `encode_frame` `FrameError.reason`."""
    return f"ERR {reason}"


# --- descriptors (contracts/canonical-text.md "Descriptors") ---------------------------------

import omgp_descriptor as D  # noqa: E402  (after the message half so import order is explicit)


def _hex4(v: int) -> str:
    return f"0x{v:04X}"


def _render_record(rec) -> str:
    if isinstance(rec, D.ProtocolRec):
        return f"PROTOCOL major={rec.major} minor={rec.minor}"
    if isinstance(rec, D.ModuleTypeRec):
        return f"MODULE_TYPE mt={_name_or_hex(G.MODULE_TYPE_NAMES, rec.type)}"
    if isinstance(rec, D.NameRec):
        return f"NAME s={quote_str(rec.s.encode('utf-8'))}"
    if isinstance(rec, D.ManufacturerRec):
        return f"MANUFACTURER s={quote_str(rec.s.encode('utf-8'))}"
    if isinstance(rec, D.ModelIdRec):
        return f"MODEL_ID model={_hex4(rec.vendor_model)} hw={_hex4(rec.hw_rev)} fw={_hex4(rec.fw_rev)}"
    if isinstance(rec, D.SerialRec):
        return f"SERIAL s={quote_str(rec.s.encode('utf-8'))}"
    if isinstance(rec, D.ChannelRec):
        return f"CHANNEL idx={rec.index} s={quote_str(rec.name.encode('utf-8'))}"
    if isinstance(rec, D.SwitchingRec):
        return f"SWITCHING flags={_hex2(rec.flags)} settle_ms={rec.settle_ms}"
    if isinstance(rec, D.ParamRec):
        return (f"PARAM id={rec.param_id} scope={_hex2(rec.scope)} kind={_name_or_hex(G.KIND_NAMES, rec.kind)} "
                f"default={rec.default} s={quote_str(rec.name.encode('utf-8'))}")
    if isinstance(rec, D.ParamEnumRec):
        return f"PARAM_ENUM id={rec.param_id} idx={rec.index} s={quote_str(rec.label.encode('utf-8'))}"
    if isinstance(rec, D.AudioRec):
        return (f"AUDIO io={_hex2(rec.io_flags)} input_mode={rec.input_mode} in_max={rec.in_max_mvrms} "
                f"out_max={rec.out_max_mvrms}")
    if isinstance(rec, D.PowerLvRec):
        return f"POWER_LV p15={rec.p15_ma} n15={rec.n15_ma} p9={rec.p9_ma} p5={rec.p5_ma}"
    if isinstance(rec, D.PowerTubeRec):
        return (f"POWER_TUBE class={rec.power_class} tubes={rec.tubes} sections={rec.sections} "
                f"heater_nom={rec.heater_nom_ma} heater_max={rec.heater_max_ma} bplus_v={rec.bplus_nom_v} "
                f"bplus_exp={rec.bplus_exp_ma} bplus_max={rec.bplus_max_ma}")
    if isinstance(rec, D.VendorRec):
        return f"VENDOR vendor={_hex4(rec.vendor_id)} data={rec.data.hex()}"
    if isinstance(rec, D.UnknownRec):
        return f"UNKNOWN type={_hex2(rec.type)} data={rec.data.hex()}"
    raise CanonicalError(f"cannot render {type(rec).__name__}")


def descriptor_to_canonical(records) -> str:
    return " | ".join(_render_record(r) for r in records)


def _split_records(text: str) -> list[str]:
    """Split on '|' outside quoted strings."""
    out, cur, quoted, esc = [], [], False, False
    for ch in text:
        if quoted:
            cur.append(ch)
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                quoted = False
            continue
        if ch == '"':
            quoted = True
            cur.append(ch)
        elif ch == "|":
            out.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if quoted:
        raise CanonicalError("unterminated string")
    out.append("".join(cur))
    return [s.strip() for s in out if s.strip()]


def _record_tokens(chunk: str) -> tuple[str, dict[str, str]]:
    """'NAME s="a b"' -> ('NAME', {'s': '"a b"'}), honouring quotes."""
    toks, cur, quoted, esc = [], [], False, False
    for ch in chunk:
        if quoted:
            cur.append(ch)
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                quoted = False
            continue
        if ch == '"':
            quoted = True
            cur.append(ch)
        elif ch == " ":
            if cur:
                toks.append("".join(cur))
                cur = []
        else:
            cur.append(ch)
    if cur:
        toks.append("".join(cur))
    if not toks:
        raise CanonicalError("empty record")
    kv: dict[str, str] = {}
    for tok in toks[1:]:
        if "=" not in tok:
            raise CanonicalError(f"token without '=': {tok!r}")
        k, v = tok.split("=", 1)
        if k in kv:
            raise CanonicalError(f"duplicate key {k!r}")
        kv[k] = v
    return toks[0], kv


def _parse_record(chunk: str):
    name, kv = _record_tokens(chunk)
    t = _take
    q = lambda key: unquote_str(t(kv, key)).decode("utf-8", errors="surrogateescape")  # noqa: E731
    if name == "PROTOCOL":
        rec = D.ProtocolRec(_int(t(kv, "major")), _int(t(kv, "minor")))
    elif name == "MODULE_TYPE":
        rec = D.ModuleTypeRec(_parse_named("mt", t(kv, "mt")))
    elif name == "NAME":
        rec = D.NameRec(q("s"))
    elif name == "MANUFACTURER":
        rec = D.ManufacturerRec(q("s"))
    elif name == "MODEL_ID":
        rec = D.ModelIdRec(_int(t(kv, "model")), _int(t(kv, "hw")), _int(t(kv, "fw")))
    elif name == "SERIAL":
        rec = D.SerialRec(q("s"))
    elif name == "CHANNEL":
        rec = D.ChannelRec(_int(t(kv, "idx")), q("s"))
    elif name == "SWITCHING":
        rec = D.SwitchingRec(_int(t(kv, "flags")), _int(t(kv, "settle_ms")))
    elif name == "PARAM":
        rec = D.ParamRec(_int(t(kv, "id")), _int(t(kv, "scope")), _parse_named("kind", t(kv, "kind")),
                         _int(t(kv, "default")), q("s"))
    elif name == "PARAM_ENUM":
        rec = D.ParamEnumRec(_int(t(kv, "id")), _int(t(kv, "idx")), q("s"))
    elif name == "AUDIO":
        rec = D.AudioRec(_int(t(kv, "io")), _int(t(kv, "input_mode")), _int(t(kv, "in_max")), _int(t(kv, "out_max")))
    elif name == "POWER_LV":
        rec = D.PowerLvRec(_int(t(kv, "p15")), _int(t(kv, "n15")), _int(t(kv, "p9")), _int(t(kv, "p5")))
    elif name == "POWER_TUBE":
        rec = D.PowerTubeRec(_int(t(kv, "class")), _int(t(kv, "tubes")), _int(t(kv, "sections")),
                             _int(t(kv, "heater_nom")), _int(t(kv, "heater_max")), _int(t(kv, "bplus_v")),
                             _int(t(kv, "bplus_exp")), _int(t(kv, "bplus_max")))
    elif name == "VENDOR":
        rec = D.VendorRec(_int(t(kv, "vendor")), _hexbytes(t(kv, "data")))
    elif name == "UNKNOWN":
        rec = D.UnknownRec(_int(t(kv, "type")), _hexbytes(t(kv, "data")))
    else:
        raise CanonicalError(f"unknown record {name!r}")
    if kv:
        raise CanonicalError(f"unexpected keys {sorted(kv)} in {name}")
    return rec


def canonical_to_descriptor(text: str) -> list:
    return [_parse_record(chunk) for chunk in _split_records(text)]


def validate_line(blob: bytes) -> str:
    """The DVAL line both implementations must produce."""
    r = D.validate_descriptor(blob)
    if r.status == "Ok":
        return f"OK skipped={r.skipped_unknown} channels={r.channel_count} params={r.param_count}"
    return f"ERR {r.status} type={_hex2(r.type)} offset={r.offset}"


def render_descriptor_bytes(blob: bytes) -> str:
    try:
        return descriptor_to_canonical(D.parse_descriptor(blob))
    except l3.L3Error as e:
        return error_to_canonical(e)
