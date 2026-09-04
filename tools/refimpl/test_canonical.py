"""Canonical-text tests (spec 001 T029). Exact strings from contracts/canonical-text.md."""
from __future__ import annotations

import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from _gen import P  # noqa: E402
import canonical as C  # noqa: E402
import omgp_l3 as l3  # noqa: E402

G = P()
H = l3.Header

CASES = [
    ("op=PING node=0x01 seq=0 flags=0x00", H(G.OP_PING, 1, 0, 0, 0), None),
    ("op=IDENTIFY node=0x10 seq=5 flags=0x01 major=1 minor=0 mt=TUBE_PREAMP desc_len=612 desc_crc=0x4A3F",
     H(G.OP_IDENTIFY, 0x10, 5, G.FLAG_response, 7), l3.IdentifyResp(1, 0, G.MT_TUBE_PREAMP, 612, 0x4A3F)),
    ("op=SET_PARAM node=0x10 seq=3 flags=0x00 param_id=1 scope=0xFF value=4095",
     H(G.OP_SET_PARAM, 0x10, 3, 0, 4), l3.SetParamReq(1, 0xFF, 4095)),
    ("op=GET_EVENT node=0x10 seq=9 flags=0x01 evt=NONE remaining=0 detail=",
     H(G.OP_GET_EVENT, 0x10, 9, G.FLAG_response, 2), l3.GetEventResp(G.EVT_NONE, 0, b"")),
    ("op=BP_POWER node=0x02 seq=1 flags=0x00 opaque=01ff00",
     H(G.OP_BP_POWER, 2, 1, 0, 3), l3.OpaquePayload(bytes.fromhex("01ff00"))),
    ("op=ERROR node=0x10 seq=3 flags=0x03 err=ERR_BUSY detail=",
     H(G.OP_ERROR, 0x10, 3, G.FLAG_response | G.FLAG_error, 1), l3.ErrorResp(G.ERR_BUSY, b"")),
    ("op=READ_DESC node=0x10 seq=2 flags=0x00 offset=1987 max_len=61",
     H(G.OP_READ_DESC, 0x10, 2, 0, 3), l3.ReadDescReq(1987, 61)),
    ("op=READ_DESC node=0x10 seq=2 flags=0x01 offset=2040 bytes=0102030405060708",
     H(G.OP_READ_DESC, 0x10, 2, G.FLAG_response, 11), l3.ReadDescResp(2040, bytes(range(1, 9)))),
    ("op=GET_STATUS node=0x10 seq=4 flags=0x01 state=READY channel=2 bypass=0 fault=0x00 uptime_s=77 pending=1",
     H(G.OP_GET_STATUS, 0x10, 4, G.FLAG_response, 7), l3.StatusBlock(G.STATE_READY, 2, 0, 0, 77, 1)),
    ("op=GET_EVENT node=0x10 seq=9 flags=0x01 evt=0xF0 remaining=0 detail=deadbeef",
     H(G.OP_GET_EVENT, 0x10, 9, G.FLAG_response, 6), l3.GetEventResp(0xF0, 0, bytes.fromhex("deadbeef"))),
]


@pytest.mark.parametrize("line,header,obj", CASES, ids=[c[0].split()[0] + c[0].split()[3] for c in CASES])
def test_message_round_trip(line, header, obj):
    assert C.message_to_canonical(header, obj) == line
    h, o = C.canonical_to_message(line)
    assert (h, o) == (header, obj)


def test_unknown_opcode_renders_hex_with_raw_payload():
    line = "op=0x63 node=0x10 seq=1 flags=0x00 raw=0a0b"
    h, o = C.canonical_to_message(line)
    assert h == H(0x63, 0x10, 1, 0, 2) and o == l3.RawPayload(bytes.fromhex("0a0b"))
    assert C.message_to_canonical(h, o) == line


def test_status_block_canonical():
    line = "state=READY channel=2 bypass=0 fault=0x00 uptime_s=77 pending=1"
    sb = l3.StatusBlock(G.STATE_READY, 2, 0, 0, 77, 1)
    assert C.status_to_canonical(sb) == line
    assert C.canonical_to_status(line) == sb


def test_error_rendering():
    assert C.error_to_canonical(l3.L3Error("Truncated", "x")) == "ERR Truncated"


def test_string_quoting_is_ascii_and_reversible():
    raw = b'a"b\\c\xc3\xa9 z'
    q = C.quote_str(raw)
    assert q == '"a\\"b\\\\c\\xc3\\xa9 z"'
    assert q.isascii() and C.unquote_str(q) == raw


def test_bad_canonical_is_rejected_as_bad_request():
    with pytest.raises(C.CanonicalError):
        C.canonical_to_message("op=PING node=0x01")
    with pytest.raises(C.CanonicalError):
        C.canonical_to_message("op=NOPE node=0x01 seq=0 flags=0x00")


# --- descriptors (spec 001 T046) ------------------------------------------------------------

import omgp_descriptor as D  # noqa: E402

SAMPLE_LINE = ('PROTOCOL major=1 minor=0 | MODULE_TYPE mt=TUBE_PREAMP | NAME s="British Preamp" | '
               'MANUFACTURER s="OMGP" | MODEL_ID model=0x0101 hw=0x0002 fw=0x0103 | SERIAL s="BP-0001" | '
               'CHANNEL idx=0 s="Clean" | CHANNEL idx=1 s="Crunch" | SWITCHING flags=0x01 settle_ms=120 | '
               'PARAM id=1 scope=0xFF kind=CONTINUOUS default=2048 s="Gain" | PARAM_ENUM id=3 idx=0 s="Bright" | '
               'AUDIO io=0x03 input_mode=1 in_max=500 out_max=1200 | POWER_LV p15=40 n15=40 p9=0 p5=20 | '
               'POWER_TUBE class=2 tubes=2 sections=4 heater_nom=600 heater_max=700 bplus_v=250 bplus_exp=12 '
               'bplus_max=20 | VENDOR vendor=0x1234 data=deadbeef | UNKNOWN type=0x55 data=01020304050607')


def test_descriptor_canonical_matches_contract_example():
    recs = C.canonical_to_descriptor(SAMPLE_LINE)
    assert recs[0] == D.ProtocolRec(1, 0) and recs[2] == D.NameRec("British Preamp")
    assert recs[-1] == D.UnknownRec(0x55, bytes.fromhex("01020304050607"))
    assert C.descriptor_to_canonical(recs) == SAMPLE_LINE
    blob = D.build_descriptor(recs)
    assert C.render_descriptor_bytes(blob) == SAMPLE_LINE
    assert C.validate_line(blob) == "OK skipped=1 channels=2 params=1"


def test_descriptor_strings_with_pipes_spaces_and_escapes_survive():
    recs = [D.ProtocolRec(1, 0), D.NameRec('a | b "c" \\ é')]
    line = C.descriptor_to_canonical(recs)
    assert line == 'PROTOCOL major=1 minor=0 | NAME s="a | b \\"c\\" \\\\ \\xc3\\xa9"'
    assert C.canonical_to_descriptor(line) == recs


def test_validate_line_error_form():
    assert C.validate_line(b"\x01\x02\x01") == "ERR Truncated type=0x01 offset=0"
    assert C.validate_line(b"") == f"ERR MissingRequired type=0x{G.TLV_PROTOCOL:02X} offset=0"
    assert C.render_descriptor_bytes(b"\x01") == "ERR Truncated"
    assert C.descriptor_to_canonical([]) == ""


# --- frames (spec 002 T019, contracts/frame-vectors.md "Canonical frame line") --------------

import omgp_link as L  # noqa: E402

FRAME_CASES = [
    ("all-zero-empty-payload",
     "frame dst=0x00 src=0x00 flags=0x00 seq=0 payload=",
     L.Frame(dst=0x00, src=0x00, response=False, retry=False, seq=0, payload=b"")),
    ("response-flag-nonzero-seq",
     "frame dst=0x00 src=0x01 flags=0x01 seq=5 payload=0101000000",
     L.Frame(dst=0x00, src=0x01, response=True, retry=False, seq=5, payload=bytes.fromhex("0101000000"))),
    ("retry-flag-max-seq",
     "frame dst=0x01 src=0x00 flags=0x02 seq=15 payload=0101000000",
     L.Frame(dst=0x01, src=0x00, response=False, retry=True, seq=15, payload=bytes.fromhex("0101000000"))),
    ("max-payload",
     "frame dst=0x01 src=0x00 flags=0x00 seq=0 payload=" + bytes(range(64)).hex(),
     L.Frame(dst=0x01, src=0x00, response=False, retry=False, seq=0, payload=bytes(range(64)))),
]


@pytest.mark.parametrize("line,frame", [c[1:] for c in FRAME_CASES], ids=[c[0] for c in FRAME_CASES])
def test_frame_round_trip(line, frame):
    assert C.frame_to_canonical(frame) == line
    assert C.canonical_to_frame(line) == frame
    assert C.canonical_to_frame(C.frame_to_canonical(frame)) == frame
    assert C.frame_to_canonical(C.canonical_to_frame(line)) == line


# Out-of-range frame-line fields must be rejected, not masked or left to crash with a bare
# ValueError, matching tools/canonical.cpp's parse_frame_line (docs/OPEN-QUESTIONS.md
# 2026-09-03 "Frame line out-of-range fields": C++ strict rejection is normative, T025 aligns
# the Python reference). dst=0xFF (reserved, but in-range) and payload lengths 65-0xFF (in
# text-range, refused by encode_frame's own PayloadTooLong) are deliberately NOT here: those
# stay valid parses per parse_frame_line's own layering, covered by ENCODE_REFUSALS above.
OUT_OF_RANGE_FRAME_LINES = [
    ("dst-overflow", "frame dst=0x100 src=0x00 flags=0x00 seq=0 payload="),
    ("src-overflow", "frame dst=0x00 src=0x100 flags=0x00 seq=0 payload="),
    ("flags-above-0x03", "frame dst=0x00 src=0x00 flags=0x04 seq=0 payload="),
    ("seq-above-0x0f", "frame dst=0x00 src=0x00 flags=0x00 seq=16 payload="),
    ("payload-longer-than-wire-length-byte", "frame dst=0x00 src=0x00 flags=0x00 seq=0 payload=" + ("00" * 256)),
    # Folded in from the concurrent branch's loop-form test (review round 3 on #121:
    # three tests, one behaviour — the parametrized form names its failing case).
    ("negative-seq", "frame dst=0x01 src=0x00 flags=0x00 seq=-1 payload="),
    ("negative-dst", "frame dst=-1 src=0x00 flags=0x00 seq=0 payload="),
    # review round 4 on #121: parse_frame_line's first-token compare rejects a leading
    # space; the Python side must not silently tolerate it.
    ("leading-whitespace", " frame dst=0x00 src=0x00 flags=0x00 seq=0 payload="),
    # red-team round 8 on #121: rstrip() swallowed non-space trailing whitespace the C++
    # tokenizer rejects, and bytes.fromhex tolerated whitespace inside the hex token.
    ("trailing-tab", "frame dst=0x00 src=0x00 flags=0x00 seq=0 payload=0102\t"),
    ("trailing-vertical-tab", "frame dst=0x00 src=0x00 flags=0x00 seq=0 payload=0102\x0b"),
    ("trailing-form-feed", "frame dst=0x00 src=0x00 flags=0x00 seq=0 payload=0102\x0c"),
    ("tab-inside-payload", "frame dst=0x00 src=0x00 flags=0x00 seq=0 payload=01\t02"),
    # red-team round 9 on #121: parse_uint rejects the SIGN, so -0 (in-range numerically)
    # must reject; only ONE trailing CR is popped; and int()'s Unicode-digit tolerance
    # (arabic-indic, fullwidth) diverged from strtoul.
    ("negative-zero-dst", "frame dst=-0 src=0x00 flags=0x00 seq=0 payload="),
    ("negative-zero-seq", "frame dst=0x00 src=0x00 flags=0x00 seq=-0 payload="),
    ("double-cr", "frame dst=0x00 src=0x00 flags=0x00 seq=0 payload=\r\r"),
    ("arabic-indic-digit", "frame dst=١ src=0x00 flags=0x00 seq=0 payload="),
    ("fullwidth-digit", "frame dst=１ src=0x00 flags=0x00 seq=0 payload="),
    # round 10 on #121: parse_uint rejects a leading-zero DECIMAL (legacy-octal hazard:
    # "010" names 8 to strtoul and 10 to int(tok, 10)) unless every digit is zero.
    ("leading-zero-decimal-dst", "frame dst=010 src=0x00 flags=0x00 seq=0 payload="),
    ("leading-zero-long-seq", "frame dst=0x00 src=0x00 flags=0x00 seq=0000000001 payload="),
    ("plus-leading-zero", "frame dst=+010 src=0x00 flags=0x00 seq=0 payload="),
    # round 12 on #121: C++ c_str() truncated at an embedded NUL ("1\0zz" parsed as 1) and
    # strtoul skipped leading tab (re-enabling octal). Python already rejected both; these
    # pin the now-shared strictness.
    ("nul-in-dst", "frame dst=1\0zz src=0x00 flags=0x00 seq=0 payload="),
    ("tab-prefixed-dst", "frame dst=\t011 src=0x00 flags=0x00 seq=0 payload="),
]


@pytest.mark.parametrize("line", [c[1] for c in OUT_OF_RANGE_FRAME_LINES],
                         ids=[c[0] for c in OUT_OF_RANGE_FRAME_LINES])
def test_canonical_to_frame_rejects_out_of_range_fields(line):
    with pytest.raises(C.CanonicalError):
        C.canonical_to_frame(line)


def test_canonical_to_frame_does_not_mask_seq():
    # seq=16 must be rejected outright, never silently reinterpreted as seq=0 (the masking
    # `encode_frame` itself still applies to an already-valid Frame is a different, narrower
    # guard than this parse-time check).
    with pytest.raises(C.CanonicalError):
        C.canonical_to_frame("frame dst=0x00 src=0x00 flags=0x00 seq=16 payload=")


# Hand-crafted bad streams and their expected discard, lifted from the same fixtures
# test_link.py drives the Deframer with (T012/#30), so the reason names round-trip through
# the renderer exactly as the Deframer actually reports them, not just as literal strings.
DISCARD_STREAMS = [
    ("BadCrc", bytes.fromhex("7e020071026869fda17e")),  # last CRC byte flipped: a0 -> a1
    ("BadLength", bytes.fromhex("7e010000c800007e")),  # len=0xC8 (200) outside 0-64
    ("BadEscape", bytes.fromhex("7e040020017d00a4b27e")),  # escape's second byte corrupted
    ("TooLong", bytes([G.TRUNK_flag_byte]) + bytes([0x01]) * 71 + bytes([G.TRUNK_flag_byte])),
]


@pytest.mark.parametrize("reason,stream", DISCARD_STREAMS, ids=[r for r, _ in DISCARD_STREAMS])
def test_deframer_discard_reason_renders_err(reason, stream):
    d = L.Deframer()
    assert d.feed_bytes(stream) == []
    assert d.stats[reason] == 1
    assert C.frame_error_to_canonical(reason) == f"ERR {reason}"


ENCODE_REFUSALS = [
    ("PayloadTooLong",
     L.Frame(dst=0x01, src=0x00, response=False, retry=False, seq=0, payload=bytes(G.LIMIT_max_l3_payload + 1))),
    ("ReservedAddress",
     L.Frame(dst=0xFF, src=0x00, response=False, retry=False, seq=0, payload=b"")),
]


@pytest.mark.parametrize("reason,frame", ENCODE_REFUSALS, ids=[r for r, _ in ENCODE_REFUSALS])
def test_encode_frame_refusal_renders_err(reason, frame):
    with pytest.raises(L.FrameError) as exc:
        L.encode_frame(frame)
    assert exc.value.reason == reason
    assert C.frame_error_to_canonical(exc.value.reason) == f"ERR {reason}"

# The loop-form duplicate of the parametrized rejects test was folded into
# OUT_OF_RANGE_FRAME_LINES above (review round 3 on #121: merge-union residue).


def test_canonical_to_frame_payload_65_to_255_parses_and_defers_to_the_codec():
    # contracts/frame-vectors.md carve-out: 65-255 B PARSES (the codec refuses it as
    # PayloadTooLong); only >= 256 B fails the parser. Locked on the C++ side by
    # test_canonical_frame.cpp; this is the Python half of the same boundary.
    f = C.canonical_to_frame("frame dst=0x01 src=0x00 flags=0x00 seq=0 payload=" + "aa" * 65)
    with pytest.raises(L.FrameError) as e:
        L.encode_frame(f)
    assert e.value.reason == "PayloadTooLong"


def test_frame_uint_accepts_the_plus_and_all_zero_shapes_parse_uint_accepts():
    # round 10 on #121: round 9's _frame_uint opened NEW divergences — it rejected "+1"
    # (C++ parse_uint deliberately accepts a leading '+', red-team @ 65922b5) and
    # accepted "010" as decimal 10 (C++ rejects the leading-zero shape outright).
    f = C.canonical_to_frame("frame dst=+1 src=+0x10 flags=0x00 seq=00 payload=aa")
    assert (f.dst, f.src, f.seq) == (1, 16, 0)
