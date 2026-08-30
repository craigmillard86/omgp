"""Reference implementation of the trunk L2 frame codec (spec 002 US1, T018).

Independent of the C++ codec (`link/frame.{hpp,cpp}`, not yet written) — see
specs/002-trunk-link-layer/contracts/link-python.md and docs/trunk-link-layer.md §4.
Deframer state machine and discard semantics per
specs/002-trunk-link-layer/research.md R-03: states Hunting/InFrame/Escaped, unstuffing
into a 70-byte accumulator as bytes arrive, discarding silently and resyncing on the
next FLAG. Q1 ruling (docs/OPEN-QUESTIONS.md, 2026-08-29): any invalid escape aborts the
frame immediately rather than tolerating up to eight violations.
"""
from __future__ import annotations

import pathlib
import sys
from dataclasses import dataclass

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from _gen import P  # noqa: E402
from omgp_crc import crc16_ccitt_false  # noqa: E402

_G = P()

FLAG = _G.TRUNK_flag_byte
ESC = _G.TRUNK_escape_byte
XOR = _G.TRUNK_escape_xor
MAX_PAYLOAD = _G.LIMIT_max_l3_payload

_HEADER_LEN = 4  # dst, src, ctrl, len (trunk §4)
_CRC_LEN = 2
_MAX_UNSTUFFED = _HEADER_LEN + MAX_PAYLOAD + _CRC_LEN  # 70

# trunk §5: 0xFF is reserved for future broadcast and MUST NOT be used in v1.
_RESERVED_DST = 0xFF


class FrameError(Exception):
    """`.reason` is a C++ `Status`/`Discard` name verbatim (link-python.md)."""

    def __init__(self, reason: str, detail: str = ""):
        self.reason = reason
        super().__init__(f"{reason}: {detail}" if detail else reason)


@dataclass(frozen=True)
class Frame:
    dst: int
    src: int
    response: bool
    retry: bool
    seq: int
    payload: bytes


def stuff(data: bytes) -> bytes:
    out = bytearray()
    for b in data:
        if b == FLAG:
            out += bytes([ESC, FLAG ^ XOR])
        elif b == ESC:
            out += bytes([ESC, ESC ^ XOR])
        else:
            out.append(b)
    return bytes(out)


def unstuff(data: bytes) -> bytes:
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        b = data[i]
        if b == ESC:
            if i + 1 >= n:
                raise FrameError("BadEscape", "trailing escape byte")
            nxt = data[i + 1]
            if nxt == FLAG ^ XOR:
                out.append(FLAG)
            elif nxt == ESC ^ XOR:
                out.append(ESC)
            else:
                raise FrameError("BadEscape", f"invalid escape byte 0x{nxt:02x}")
            i += 2
        else:
            out.append(b)
            i += 1
    return bytes(out)


def crc(data: bytes) -> int:
    return crc16_ccitt_false(data)


def encode_frame(f: Frame) -> bytes:
    if len(f.payload) > MAX_PAYLOAD:
        raise FrameError("PayloadTooLong", f"{len(f.payload)} > {MAX_PAYLOAD}")
    if f.dst == _RESERVED_DST:
        raise FrameError("ReservedAddress", f"dst=0x{f.dst:02x}")
    ctrl = (0x01 if f.response else 0) | (0x02 if f.retry else 0) | ((f.seq & 0x0F) << 4)
    body = bytes([f.dst, f.src, ctrl, len(f.payload)]) + f.payload
    c = crc(body)
    body += bytes([c & 0xFF, (c >> 8) & 0xFF])
    return bytes([FLAG]) + stuff(body) + bytes([FLAG])


class Deframer:
    """Byte-at-a-time trunk frame parser (research.md R-03)."""

    def __init__(self) -> None:
        self._state = "Hunting"  # Hunting | InFrame | Escaped
        self._buf = bytearray()
        self.stats = {"delivered": 0, "BadCrc": 0, "BadLength": 0, "BadEscape": 0, "TooLong": 0}

    def feed(self, byte: int) -> Frame | None:
        if byte == FLAG:
            return self._on_flag()
        if self._state == "Hunting":
            return None
        if self._state == "Escaped":
            return self._on_escaped_byte(byte)
        if byte == ESC:
            self._state = "Escaped"
            return None
        self._append_unstuffed(byte)
        return None

    def feed_bytes(self, data: bytes) -> list[Frame]:
        delivered = []
        for byte in data:
            frame = self.feed(byte)
            if frame is not None:
                delivered.append(frame)
        return delivered

    def _append_unstuffed(self, byte: int) -> None:
        # A 71st unstuffed byte aborts the frame at once (Q1: no tolerance for partial
        # violations before the "≥ 8" bound the trunk doc allows for babble).
        if len(self._buf) >= _MAX_UNSTUFFED:
            self.stats["TooLong"] += 1
            self._state = "Hunting"
            self._buf = bytearray()
            return
        self._buf.append(byte)

    def _on_escaped_byte(self, byte: int) -> Frame | None:
        if byte == FLAG ^ XOR:
            self._append_unstuffed(FLAG)
        elif byte == ESC ^ XOR:
            self._append_unstuffed(ESC)
        else:
            self.stats["BadEscape"] += 1
            self._state = "Hunting"
            self._buf = bytearray()
            return None
        # _append_unstuffed aborts to Hunting when this was the 71st unstuffed byte
        # (TooLong); that abort must win over the Escaped->InFrame transition, or the
        # parser would treat later bytes as frame content with no FLAG ever seen
        # (trunk §4: discard silently, resynchronise on the next FLAG; review finding
        # on PR #99, 2026-08-30).
        if self._state != "Hunting":
            self._state = "InFrame"
        return None

    def _on_flag(self) -> Frame | None:
        # An escape byte as the last byte before a FLAG never gets its second byte.
        if self._state == "Escaped":
            self.stats["BadEscape"] += 1
            self._state = "InFrame"
            self._buf = bytearray()
            return None
        # Hunting or InFrame: this FLAG opens the next frame either way (shared
        # delimiter); when Hunting, `_buf` is already empty so the frame is empty (n==0).
        body, self._buf = bytes(self._buf), bytearray()
        self._state = "InFrame"
        n = len(body)
        if n == 0:
            return None  # empty frame / shared delimiter: discarded silently
        if n < _HEADER_LEN + _CRC_LEN:
            self.stats["BadLength"] += 1
            return None
        dst, src, ctrl, length = body[0], body[1], body[2], body[3]
        if length != n - (_HEADER_LEN + _CRC_LEN):
            self.stats["BadLength"] += 1
            return None
        payload = body[_HEADER_LEN : n - _CRC_LEN]
        expected_crc = body[n - _CRC_LEN] | (body[n - _CRC_LEN + 1] << 8)
        actual_crc = crc(body[: n - _CRC_LEN])
        if actual_crc != expected_crc:
            self.stats["BadCrc"] += 1
            return None
        self.stats["delivered"] += 1
        return Frame(
            dst=dst,
            src=src,
            response=bool(ctrl & 0x01),
            retry=bool(ctrl & 0x02),
            seq=(ctrl >> 4) & 0x0F,
            payload=payload,
        )
