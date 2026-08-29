"""Independent Python reference implementation of the OMGP descriptor (TLV) codec.

Written from docs/protocol-l3.md §4 / §4.1 and spec 001 data-model.md §3 — not from the
C++ code. required/repeated/max_len come from the generated TLV_INFO (FR-020).

Check order inside a record (both implementations follow it, so the differential test
compares Status names): DuplicateRecord → MalformedRecord (length shape) → StringTooLong
(max_len) → InvalidUtf8 → OutOfRange (field values). Whole-blob order: BlobTooLarge before
any record; Truncated when a length field overruns; MissingRequired after the last record.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Iterator

from _gen import P
from omgp_crc import crc16_ccitt_false
from omgp_l3 import L3Error

G = P()
_INFO = {e["type"]: e for e in G.TLV_INFO}
_MAX = G.LIMIT_max_descriptor_bytes


def _name(t: int) -> str:
    return G.TLV_NAMES.get(t, f"0x{t:02X}")


# --- raw and typed records ----------------------------------------------------------------

@dataclass(frozen=True)
class Record:
    type: int
    value: bytes
    offset: int


@dataclass(frozen=True)
class Report:
    status: str
    type: int
    offset: int
    skipped_unknown: int
    channel_count: int
    param_count: int


@dataclass(frozen=True)
class ProtocolRec:
    major: int
    minor: int


@dataclass(frozen=True)
class ModuleTypeRec:
    type: int


@dataclass(frozen=True)
class NameRec:
    s: str


@dataclass(frozen=True)
class ManufacturerRec:
    s: str


@dataclass(frozen=True)
class ModelIdRec:
    vendor_model: int
    hw_rev: int
    fw_rev: int


@dataclass(frozen=True)
class SerialRec:
    s: str


@dataclass(frozen=True)
class ChannelRec:
    index: int
    name: str


@dataclass(frozen=True)
class SwitchingRec:
    flags: int
    settle_ms: int


@dataclass(frozen=True)
class ParamRec:
    param_id: int
    scope: int
    kind: int
    default: int
    name: str


@dataclass(frozen=True)
class ParamEnumRec:
    param_id: int
    index: int
    label: str


@dataclass(frozen=True)
class AudioRec:
    io_flags: int
    input_mode: int
    in_max_mvrms: int
    out_max_mvrms: int


@dataclass(frozen=True)
class PowerLvRec:
    p15_ma: int
    n15_ma: int
    p9_ma: int
    p5_ma: int


@dataclass(frozen=True)
class PowerTubeRec:
    power_class: int
    tubes: int
    sections: int
    heater_nom_ma: int
    heater_max_ma: int
    bplus_nom_v: int
    bplus_exp_ma: int
    bplus_max_ma: int


@dataclass(frozen=True)
class VendorRec:
    vendor_id: int
    data: bytes


@dataclass(frozen=True)
class UnknownRec:
    type: int
    data: bytes


TYPE_OF = {
    ProtocolRec: G.TLV_PROTOCOL, ModuleTypeRec: G.TLV_MODULE_TYPE, NameRec: G.TLV_NAME,
    ManufacturerRec: G.TLV_MANUFACTURER, ModelIdRec: G.TLV_MODEL_ID, SerialRec: G.TLV_SERIAL,
    ChannelRec: G.TLV_CHANNEL, SwitchingRec: G.TLV_SWITCHING, ParamRec: G.TLV_PARAM,
    ParamEnumRec: G.TLV_PARAM_ENUM, AudioRec: G.TLV_AUDIO, PowerLvRec: G.TLV_POWER_LV,
    PowerTubeRec: G.TLV_POWER_TUBE, VendorRec: G.TLV_VENDOR,
}
_STRING_ONLY = {G.TLV_NAME: NameRec, G.TLV_MANUFACTURER: ManufacturerRec, G.TLV_SERIAL: SerialRec}


def _err(status: str, t: int, detail: str = "") -> L3Error:
    return L3Error(status, f"{_name(t)}{(' ' + detail) if detail else ''}")


def _utf8(t: int, raw: bytes) -> str:
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        raise _err("InvalidUtf8", t) from None


def _utf8_bytes(t: int, s: str) -> bytes:
    try:
        return s.encode("utf-8")
    except UnicodeEncodeError:
        raise _err("InvalidUtf8", t) from None


def _string(t: int, raw: bytes) -> str:
    max_len = _INFO[t]["max_len"]
    if max_len and len(raw) > max_len:
        raise _err("StringTooLong", t, f"{len(raw)} > {max_len}")
    return _utf8(t, raw)


# --- typed decode / encode --------------------------------------------------------------------

def decode_value(t: int, v: bytes):
    """Typed record from a raw value; UnknownRec for types the protocol does not define."""
    if t not in _INFO:
        return UnknownRec(t, bytes(v))
    n = len(v)
    if t in _STRING_ONLY:
        return _STRING_ONLY[t](_string(t, v))
    if t == G.TLV_PROTOCOL:
        if n != 2:
            raise _err("MalformedRecord", t)
        return ProtocolRec(v[0], v[1])
    if t == G.TLV_MODULE_TYPE:
        if n != 1:
            raise _err("MalformedRecord", t)
        if v[0] not in G.MODULE_TYPE_NAMES:
            raise _err("OutOfRange", t, "module type")
        return ModuleTypeRec(v[0])
    if t == G.TLV_MODEL_ID:
        if n != 6:
            raise _err("MalformedRecord", t)
        return ModelIdRec(*struct.unpack("<HHH", v))
    if t == G.TLV_CHANNEL:
        if n < 1:
            raise _err("MalformedRecord", t)
        return ChannelRec(v[0], _utf8(t, v[1:]))
    if t == G.TLV_SWITCHING:
        if n != 3:
            raise _err("MalformedRecord", t)
        return SwitchingRec(v[0], struct.unpack("<H", v[1:3])[0])
    if t == G.TLV_PARAM:
        if n < 5:
            raise _err("MalformedRecord", t)
        name = _utf8(t, v[5:])
        default = struct.unpack("<H", v[3:5])[0]
        if v[2] not in G.KIND_NAMES:
            raise _err("OutOfRange", t, "kind")
        if default > G.LIMIT_param_value_max:
            raise _err("OutOfRange", t, "default")
        return ParamRec(v[0], v[1], v[2], default, name)
    if t == G.TLV_PARAM_ENUM:
        if n < 2:
            raise _err("MalformedRecord", t)
        return ParamEnumRec(v[0], v[1], _utf8(t, v[2:]))
    if t == G.TLV_AUDIO:
        if n != 6:
            raise _err("MalformedRecord", t)
        r = AudioRec(v[0], v[1], *struct.unpack("<HH", v[2:6]))
        if r.input_mode > 1:
            raise _err("OutOfRange", t, "input_mode")
        return r
    if t == G.TLV_POWER_LV:
        if n != 8:
            raise _err("MalformedRecord", t)
        return PowerLvRec(*struct.unpack("<HHHH", v))
    if t == G.TLV_POWER_TUBE:
        if n != 11:
            raise _err("MalformedRecord", t)
        r = PowerTubeRec(*struct.unpack("<BBBHHHBB", v))
        if not 1 <= r.power_class <= 4:
            raise _err("OutOfRange", t, "power_class")
        return r
    if t == G.TLV_VENDOR:
        if n < 2:
            raise _err("MalformedRecord", t)
        return VendorRec(struct.unpack("<H", v[:2])[0], bytes(v[2:]))
    raise AssertionError(t)  # every TLV_INFO type is handled above


def encode_value(rec) -> tuple[int, bytes]:
    """(type, raw value) for a typed record, validating the same rules as decode."""
    if isinstance(rec, UnknownRec):
        return rec.type, bytes(rec.data)
    t = TYPE_OF[type(rec)]
    if t in _STRING_ONLY:
        raw = _utf8_bytes(t, rec.s)
        max_len = _INFO[t]["max_len"]
        if max_len and len(raw) > max_len:
            raise _err("StringTooLong", t, f"{len(raw)} > {max_len}")
        return t, raw
    try:
        if isinstance(rec, ProtocolRec):
            return t, struct.pack("<BB", rec.major, rec.minor)
        if isinstance(rec, ModuleTypeRec):
            if rec.type not in G.MODULE_TYPE_NAMES:
                raise _err("OutOfRange", t, "module type")
            return t, struct.pack("<B", rec.type)
        if isinstance(rec, ModelIdRec):
            return t, struct.pack("<HHH", rec.vendor_model, rec.hw_rev, rec.fw_rev)
        if isinstance(rec, ChannelRec):
            return t, struct.pack("<B", rec.index) + _utf8_bytes(t, rec.name)
        if isinstance(rec, SwitchingRec):
            return t, struct.pack("<BH", rec.flags, rec.settle_ms)
        if isinstance(rec, ParamRec):
            name = _utf8_bytes(t, rec.name)
            if rec.kind not in G.KIND_NAMES:
                raise _err("OutOfRange", t, "kind")
            if rec.default > G.LIMIT_param_value_max:
                raise _err("OutOfRange", t, "default")
            return t, struct.pack("<BBBH", rec.param_id, rec.scope, rec.kind, rec.default) + name
        if isinstance(rec, ParamEnumRec):
            return t, struct.pack("<BB", rec.param_id, rec.index) + _utf8_bytes(t, rec.label)
        if isinstance(rec, AudioRec):
            if rec.input_mode > 1:
                raise _err("OutOfRange", t, "input_mode")
            return t, struct.pack("<BBHH", rec.io_flags, rec.input_mode, rec.in_max_mvrms, rec.out_max_mvrms)
        if isinstance(rec, PowerLvRec):
            return t, struct.pack("<HHHH", rec.p15_ma, rec.n15_ma, rec.p9_ma, rec.p5_ma)
        if isinstance(rec, PowerTubeRec):
            if not 1 <= rec.power_class <= 4:
                raise _err("OutOfRange", t, "power_class")
            return t, struct.pack("<BBBHHHBB", rec.power_class, rec.tubes, rec.sections, rec.heater_nom_ma,
                                  rec.heater_max_ma, rec.bplus_nom_v, rec.bplus_exp_ma, rec.bplus_max_ma)
        if isinstance(rec, VendorRec):
            return t, struct.pack("<H", rec.vendor_id) + bytes(rec.data)
    except struct.error as e:
        raise _err("OutOfRange", t, str(e)) from None
    raise AssertionError(type(rec))


# --- blob level ---------------------------------------------------------------------------------

def iter_records(blob: bytes) -> Iterator[Record]:
    """Raw TLV records in order. Does not check the cap; Truncated when a length overruns."""
    n, off = len(blob), 0
    while off < n:
        if n - off < 2:
            raise L3Error("Truncated", f"{_name(blob[off])} @ {off}: no length byte")
        t, ln = blob[off], blob[off + 1]
        if n - off - 2 < ln:
            raise L3Error("Truncated", f"{_name(t)} @ {off}: len {ln} overruns blob")
        yield Record(t, bytes(blob[off + 2:off + 2 + ln]), off)
        off += 2 + ln


def _walk(blob: bytes) -> tuple[Report, list]:
    n = len(blob)
    if n > _MAX:
        return Report("BlobTooLarge", 0, 0, 0, 0, 0), []
    seen: set[int] = set()
    skipped = channels = params = 0
    recs: list = []
    off = 0
    while off < n:
        t = blob[off]
        if n - off < 2:
            return Report("Truncated", t, off, skipped, channels, params), recs
        ln = blob[off + 1]
        if n - off - 2 < ln:
            return Report("Truncated", t, off, skipped, channels, params), recs
        value = bytes(blob[off + 2:off + 2 + ln])
        info = _INFO.get(t)
        if info is None:
            skipped += 1
            recs.append(UnknownRec(t, value))
        else:
            if not info["repeated"] and t in seen:
                return Report("DuplicateRecord", t, off, skipped, channels, params), recs
            seen.add(t)
            try:
                recs.append(decode_value(t, value))
            except L3Error as e:
                return Report(e.status, t, off, skipped, channels, params), recs
            if t == G.TLV_CHANNEL:
                channels += 1
            elif t == G.TLV_PARAM:
                params += 1
        off += 2 + ln
    for e in G.TLV_INFO:  # sorted by type
        if e["required"] and e["type"] not in seen:
            return Report("MissingRequired", e["type"], n, skipped, channels, params), recs
    return Report("Ok", 0, 0, skipped, channels, params), recs


def validate_descriptor(blob: bytes) -> Report:
    return _walk(blob)[0]


def parse_descriptor(blob: bytes) -> list:
    report, recs = _walk(blob)
    if report.status != "Ok":
        raise L3Error(report.status, f"{_name(report.type)} @ {report.offset}")
    return recs


def build_descriptor(records) -> bytes:
    out = bytearray()
    seen: set[int] = set()
    for rec in records:
        t, value = encode_value(rec)
        info = _INFO.get(t)
        if info is not None:
            if not info["repeated"] and t in seen:
                raise _err("DuplicateRecord", t)
            seen.add(t)
        if len(value) > 0xFF:
            raise _err("StringTooLong" if not isinstance(rec, (VendorRec, UnknownRec)) else "OutOfRange", t,
                       "value exceeds the length byte")
        if len(out) + 2 + len(value) > _MAX:
            raise _err("BlobTooLarge", t)
        out += bytes([t, len(value)]) + value
    for e in G.TLV_INFO:
        if e["required"] and e["type"] not in seen:
            raise _err("MissingRequired", e["type"])
    return bytes(out)


def descriptor_crc(blob: bytes) -> int:
    """CRC-16/CCITT-FALSE over the whole blob (ruled 2026-08-28; YAML descriptor.crc)."""
    return crc16_ccitt_false(bytes(blob))
