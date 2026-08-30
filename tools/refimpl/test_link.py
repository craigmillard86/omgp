"""Reference-implementation tests for the trunk L2 frame codec (spec 002 US1, T012).

Written from docs/trunk-link-layer.md §4 and the Q1 ruling (2026-08-29,
docs/OPEN-QUESTIONS.md: a single invalid escape aborts the frame at once) — not from
the C++ code. Wire bytes below are hand-computed from the same algorithm (stuff/CRC),
not read back from `omgp_link` itself, so a bug shared between the codec and a
fixture-builder cannot hide here. `tools/refimpl/omgp_link.py` does not exist yet
(T018); this file is expected to fail at collection (`ModuleNotFoundError`) until then
(CLAUDE.md rule 8).
"""
from __future__ import annotations

import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from _gen import P  # noqa: E402
import omgp_link as L  # noqa: E402

G = P()

FLAG = G.TRUNK_flag_byte
ESC = G.TRUNK_escape_byte
XOR = G.TRUNK_escape_xor
MAX_PAYLOAD = G.LIMIT_max_l3_payload


def raises(reason: str):
    return pytest.raises(L.FrameError, match=rf"^{reason}\b")


# dst=0x02 src=0x00 response=1 retry=0 seq=7 payload=b"hi"; crc over unstuffed dst..payload.
MARKER_FULL = bytes.fromhex("7e020071026869fda07e")
MARKER_FRAME = L.Frame(dst=0x02, src=0x00, response=True, retry=False, seq=7, payload=b"hi")

# dst=0x03 src=0x00 response=0 retry=1 seq=1 payload=b"Q"
MARKER_B_FULL = bytes.fromhex("7e030012015138ab7e")
MARKER_B_FRAME = L.Frame(dst=0x03, src=0x00, response=False, retry=True, seq=1, payload=b"Q")


# --- stuff/unstuff identities (FR-001) ---

def test_stuff_unstuff_round_trip_mixed_payload():
    payload = bytes([0x00, 0x7E, 0x01, 0x7D, 0xFF, 0x02])
    assert L.unstuff(L.stuff(payload)) == payload


def test_stuff_unstuff_round_trip_worst_case_7e_7d_only_payload():
    payload = bytes([0x7E, 0x7D] * 32)  # every byte needs escaping
    stuffed = L.stuff(payload)
    assert stuffed == bytes([ESC, FLAG ^ XOR, ESC, ESC ^ XOR] * 32)
    assert L.unstuff(stuffed) == payload


def test_stuff_maps_flag_and_escape_bytes():
    assert L.stuff(bytes([FLAG])) == bytes([ESC, FLAG ^ XOR])
    assert L.stuff(bytes([ESC])) == bytes([ESC, ESC ^ XOR])


def test_unstuff_rejects_invalid_escape():
    with raises("BadEscape"):
        L.unstuff(bytes([ESC, 0x00]))


def test_unstuff_rejects_trailing_escape():
    with raises("BadEscape"):
        L.unstuff(bytes([0x01, ESC]))


# --- crc() (published CCITT-FALSE check vector) ---

def test_crc_matches_published_check_value():
    assert L.crc(b"123456789") == 0x29B1


# --- encode_frame() against hand-computed bytes (FR-001) ---

def test_encode_frame_minimal_ping_matches_hand_computed_bytes():
    f = L.Frame(dst=0x01, src=0x00, response=False, retry=False, seq=0, payload=b"")
    assert L.encode_frame(f) == bytes.fromhex("7e0100000074f27e")


def test_encode_frame_max_payload_matches_hand_computed_bytes():
    payload = bytes(range(MAX_PAYLOAD))
    f = L.Frame(dst=0x01, src=0x00, response=True, retry=False, seq=5, payload=payload)
    assert L.encode_frame(f) == bytes.fromhex(
        "7e01005140000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
        "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f8b237e"
    )


# --- encode_frame() refuses before emitting a byte (FR-005) ---

def test_encode_frame_refuses_oversize_payload():
    f = L.Frame(dst=0x01, src=0x00, response=False, retry=False, seq=0,
                payload=bytes(MAX_PAYLOAD + 1))
    with raises("PayloadTooLong"):
        L.encode_frame(f)


def test_encode_frame_refuses_reserved_dst():
    # dst 0xFF: reserved broadcast address, "MUST NOT be used in v1" (trunk-link-layer.md §5).
    f = L.Frame(dst=0xFF, src=0x00, response=False, retry=False, seq=0, payload=b"")
    with raises("ReservedAddress"):
        L.encode_frame(f)


# --- Deframer: stats shape, incremental parse, discard reasons, resync (FR-002/FR-003) ---

def test_deframer_stats_start_at_zero():
    d = L.Deframer()
    assert d.stats == {"delivered": 0, "BadCrc": 0, "BadLength": 0, "BadEscape": 0, "TooLong": 0}


def test_deframer_delivers_marker_frame():
    d = L.Deframer()
    assert d.feed_bytes(MARKER_FULL) == [MARKER_FRAME]
    assert d.stats["delivered"] == 1


def test_deframer_feed_single_bytes_matches_feed_bytes():
    # FR-002: "any byte at a time, including one" — feed() one byte at a time must agree
    # with feed_bytes() fed the whole stream at once.
    d = L.Deframer()
    delivered = [d.feed(byte) for byte in MARKER_FULL]
    assert [f for f in delivered if f is not None] == [MARKER_FRAME]


def test_deframer_discards_bad_crc_and_resyncs():
    corrupt = bytes.fromhex("7e020071026869fda17e")  # last CRC byte flipped: a0 -> a1
    d = L.Deframer()
    assert d.feed_bytes(corrupt + MARKER_FULL) == [MARKER_FRAME]
    assert d.stats["BadCrc"] == 1
    assert d.stats["delivered"] == 1


def test_deframer_discards_length_outside_range_and_resyncs():
    # dst=0x01 src=0x00 ctrl=0x00 len=0xC8 (200, outside 0-64) then two arbitrary bytes.
    bad = bytes.fromhex("7e010000c800007e")
    d = L.Deframer()
    assert d.feed_bytes(bad + MARKER_FULL) == [MARKER_FRAME]
    assert d.stats["BadLength"] == 1


def test_deframer_discards_length_count_mismatch_and_resyncs():
    # dst=0x01 src=0x00 ctrl=0x00 len=0x05 (declares 5 payload bytes) but only 4 more
    # bytes precede the FLAG — declared length and actual byte count disagree.
    bad = bytes.fromhex("7e01000005aabbccdd7e")
    d = L.Deframer()
    assert d.feed_bytes(bad + MARKER_FULL) == [MARKER_FRAME]
    assert d.stats["BadLength"] == 1


def test_deframer_discards_bad_escape_and_resyncs():
    # A valid frame (dst=4 payload=b"\x7e") with its escape's second byte corrupted:
    # 7d 5e -> 7d 00 (0x00 is neither 0x5E nor 0x5D).
    bad = bytes.fromhex("7e040020017d00a4b27e")
    d = L.Deframer()
    assert d.feed_bytes(bad + MARKER_FULL) == [MARKER_FRAME]
    assert d.stats["BadEscape"] == 1


def test_deframer_discards_trailing_escape_before_flag_and_resyncs():
    # Edge case: "an escape byte as the last byte before a FLAG (0x7D 0x7E)". The FLAG
    # that would end this dangling escape is the same FLAG that opens the next frame.
    bad = bytes.fromhex("7e010000007d")
    d = L.Deframer()
    assert d.feed_bytes(bad + MARKER_FULL) == [MARKER_FRAME]
    assert d.stats["BadEscape"] == 1


def test_deframer_discards_71_byte_unstuffed_frame_as_too_long_and_resyncs():
    # Max valid unstuffed body is 4 (header) + 64 (payload) + 2 (crc) = 70 bytes; 71 is
    # the smallest value that must be TooLong.
    assert MAX_PAYLOAD == 64
    bad = bytes([FLAG]) + bytes([0x01]) * 71 + bytes([FLAG])
    d = L.Deframer()
    assert d.feed_bytes(bad + MARKER_FULL) == [MARKER_FRAME]
    assert d.stats["TooLong"] == 1


def test_deframer_discards_empty_frame_silently_and_delivers_shared_flag_next_frame():
    # Edge case: a FLAG immediately after a FLAG (empty frame) is discarded silently;
    # the shared FLAG still opens the next frame.
    stream = bytes([FLAG]) + MARKER_FULL
    d = L.Deframer()
    assert d.feed_bytes(stream) == [MARKER_FRAME]


def test_deframer_delivers_back_to_back_frames_sharing_one_flag():
    stream = MARKER_FULL[:-1] + MARKER_B_FULL  # MARKER's closing FLAG == MARKER_B's opening FLAG
    d = L.Deframer()
    assert d.feed_bytes(stream) == [MARKER_FRAME, MARKER_B_FRAME]
    assert d.stats["delivered"] == 2
