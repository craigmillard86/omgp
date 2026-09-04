"""Tests for tools/refimpl/diffcheck_frames.py (spec 002 US1, T025), written first per
CLAUDE.md rule 8: this file must fail to collect until diffcheck_frames.py exists.

Exercises the pure-Python corpus generation and the FENC/FDEC/FSTREAM comparison logic
against a fake Helper (no l3_helper subprocess, no native build needed), so the
differential wiring — request/response pairing, mismatch detection, coverage of the
frame-vectors.md corpus description — is pinned independent of the C++ binary.
tools/diffcheck.py itself is exercised end-to-end against the real binary by the
`diffcheck` pipeline stage (contracts/tooling.md).
"""
from __future__ import annotations

import pathlib
import random

import pytest
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import canonical as C  # noqa: E402
import diffcheck_frames as F  # noqa: E402
import omgp_link as link  # noqa: E402
import torture  # noqa: E402

SEED = 20260903


class FakeHelper:
    """Answers FENC/FDEC/FSTREAM by recomputing the Python reference itself, so an
    agreeing run is the default; `wrong_at` corrupts the Nth answer (1-based) to prove a
    mismatch is caught and reported rather than silently accepted."""

    def __init__(self, wrong_at: int | None = None):
        self.wrong_at = wrong_at
        self.calls = 0

    def _answer_one(self, line: str) -> str:
        verb, _, arg = line.partition(" ")
        if verb == "FENC":
            return f"OK {link.encode_frame(C.canonical_to_frame(arg)).hex()}"
        if verb == "FDEC":
            return f"OK {C.frame_to_canonical(link.decode_frame(bytes.fromhex(arg)))}"
        raise AssertionError(f"unexpected verb: {line!r}")

    def ask(self, lines: list[str]) -> list[str]:
        out = []
        for line in lines:
            self.calls += 1
            answer = self._answer_one(line)
            out.append("WRONG" if self.calls == self.wrong_at else answer)
        return out

    def ask_stream(self, lines: list[str]) -> list[list[str]]:
        out = []
        for line in lines:
            self.calls += 1
            verb, _, arg = line.partition(" ")
            assert verb == "FSTREAM"
            d = link.Deframer()
            delivered = d.feed_bytes(bytes.fromhex(arg))
            discards = sum(v for k, v in d.stats.items() if k != "delivered")
            block = [f"OK {C.frame_to_canonical(fr)}" for fr in delivered] + [f"END {discards}"]
            out.append(["WRONG"] if self.calls == self.wrong_at else block)
        return out


# --- random_frame: coverage and validity (frame-vectors.md "l3_helper verbs") ---------------

def test_frame_count_meets_the_contract_threshold():
    # contracts/frame-vectors.md: "≥ 10 000 seeded frames". Without this pin, trimming
    # FRAME_COUNT to speed the stage up passes every other test here (review on #121:
    # coverage assertions stay green down to 256).
    assert F.FRAME_COUNT >= 10_000


def test_random_frame_covers_every_dst_src_and_seq_over_frame_count():
    rng = random.Random(SEED)
    frames = [F.random_frame(rng, i) for i in range(F.FRAME_COUNT)]
    # via F._RESERVED_DST, not a fourth 0xFF hardcoding (review round 8 on #121)
    assert {fr.dst for fr in frames} == set(d for d in range(0x100) if d != F._RESERVED_DST)
    assert {fr.src for fr in frames} == set(range(0x100))  # every src
    assert {fr.seq for fr in frames} == set(range(16))  # seq 0-15
    # flags: the last unpinned line of the contract's corpus spec (review on #121).
    assert {(fr.response, fr.retry) for fr in frames} == {(False, False), (False, True),
                                                         (True, False), (True, True)}
    lengths = {len(fr.payload) for fr in frames}
    assert 0 in lengths and F.MAX_PAYLOAD in lengths  # payload lengths 0-64, boundaries hit
    # red-team round 8 on #121: the boundary forcing and the 7E/7D bias were satisfied by
    # plain random draws too — pin the CONSTRUCTED properties, not the coincidence.
    assert [len(frames[i].payload) for i in range(4)] == [0, 1, F.MAX_PAYLOAD, F.MAX_PAYLOAD - 1]
    stuffed = sum(b in (F.G.TRUNK_flag_byte, F.G.TRUNK_escape_byte) for fr in frames for b in fr.payload)
    total = sum(len(fr.payload) for fr in frames)
    assert stuffed / total > 0.3, f"7E/7D bias missing: {stuffed}/{total}"


def test_random_frame_seq_is_not_a_function_of_src():
    # Regression: seq and src were both `index % k` with 16 | 256, so seq == src & 0x0F
    # for every generated frame — a C++ encode_frame that built ctrl from src instead of
    # seq would emit byte-identical wire bytes and diffcheck would never catch it. At
    # least one src value must map to more than one seq value across the corpus.
    rng = random.Random(SEED)
    frames = [F.random_frame(rng, i) for i in range(F.FRAME_COUNT)]
    seqs_by_src: dict[int, set[int]] = {}
    for fr in frames:
        seqs_by_src.setdefault(fr.src, set()).add(fr.seq)
    assert any(len(seqs) > 1 for seqs in seqs_by_src.values())
    assert not all(fr.seq == (fr.src & 0x0F) for fr in frames)


def test_random_frame_never_generates_the_reserved_destination():
    rng = random.Random(SEED)
    for i in range(F.FRAME_COUNT):
        assert F.random_frame(rng, i).dst != 0xFF


def test_random_frame_is_deterministic():
    # Shared rng threaded through successive calls — the shape run_frames actually builds
    # (review round 8 on #121: fresh-rng-per-call asserted only purity of one draw).
    rng_a, rng_b = random.Random(SEED), random.Random(SEED)
    a = [F.random_frame(rng_a, i) for i in range(500)]
    b = [F.random_frame(rng_b, i) for i in range(500)]
    assert a == b


def test_random_frame_always_encodes_without_error():
    rng = random.Random(SEED)
    for i in range(2000):
        link.encode_frame(F.random_frame(rng, i))  # must not raise


# --- run_frames: FENC/FDEC round trip --------------------------------------------------------

def test_run_frames_agrees_reports_case_count():
    assert F.run_frames(FakeHelper(), SEED, count=50) == 50


def test_run_frames_reports_first_mismatch(capsys):
    # contracts/frame-vectors.md's acceptance criterion: the first mismatch prints
    # (seed, index), the verb, and a replay command -- not just that -1 came back.
    # wrong_at=3 corrupts the 3rd call, which is case index 1's FENC answer (2 calls/case).
    assert F.run_frames(FakeHelper(wrong_at=3), SEED, count=50) == -1
    out = capsys.readouterr().out
    assert f"seed={SEED:#x}" in out and "index=1" in out
    assert "FENC" in out
    assert "--frames-only" in out and "--frame-index 1" in out  # hint replays directly (review r3)


def test_run_frames_index_replays_a_single_case():
    assert F.run_frames(FakeHelper(), SEED, count=50, only=7) == 1


# --- run_torture: FSTREAM over torture.corpus(seed) ------------------------------------------

def test_run_torture_agrees_over_a_small_full_corpus():
    n = F.run_torture(FakeHelper(), SEED, frames=20, per_class=3)
    assert n == 20 + 3 * len(torture.CLASSES)


def test_run_torture_reports_first_mismatch(capsys):
    # Same acceptance criterion as test_run_frames_reports_first_mismatch, for the
    # FSTREAM/torture side: (seed, index), the recipe, and a --torture-index replay line.
    assert F.run_torture(FakeHelper(wrong_at=1), SEED, frames=20, per_class=3) == -1
    out = capsys.readouterr().out
    assert f"seed={SEED:#x}" in out and "index=0" in out
    assert "recipe=" in out
    assert "--frames-only" in out and "--torture-index 0" in out  # hint replays directly (review r3)


def test_run_torture_index_replays_a_single_valid_element():
    assert F.run_torture(FakeHelper(), SEED, only=0, frames=20, per_class=3) == 1


def test_a_dead_helper_is_named_in_the_mismatch_report(capsys):
    # review round 4 on #121: death_note had no test on either path. A helper whose
    # process is dead and whose answers are the EOF-derived blanks must be reported as a
    # crash, never as a bare content mismatch with an empty C++ side.
    class DeadProc:
        def poll(self):
            return -11

    class DeadHelper(FakeHelper):
        p = DeadProc()

        def ask(self, lines):
            return ["" for _ in lines]

    assert F.run_frames(DeadHelper(), SEED, count=3) == -1
    out = capsys.readouterr().out
    assert "helper exited rc=-11: crash, not a content mismatch" in out


def test_replay_index_range_guards_exit_with_a_diagnostic():
    # review round 3 on #121: the range guards landed untested. Out-of-range and negative
    # indices must sys.exit with the documented message, never IndexError or silent wrap.
    for bad in (50, -1):
        with pytest.raises(SystemExit, match="--frame-index must be 0"):
            F.run_frames(FakeHelper(), SEED, count=50, only=bad)
    with pytest.raises(SystemExit, match="--torture-index must be 0"):
        F.run_torture(FakeHelper(), SEED, only=999, frames=20, per_class=3)
    with pytest.raises(SystemExit, match="--torture-index must be 0"):
        F.run_torture(FakeHelper(), SEED, only=-1, frames=20, per_class=3)


def test_run_torture_index_replays_a_single_corrupted_element():
    assert F.run_torture(FakeHelper(), SEED, only=20, frames=20, per_class=3) == 1


# --- run_torture compares FULL blocks; run_streams covers order + ReservedAddress -----------
# (red-team round 8 on #121)

class _RefStreamHelper:
    """Honest FSTREAM helper via the reference deframer; `mangle` post-processes blocks."""

    def __init__(self, mangle=None):
        self.mangle = mangle or (lambda b: b)

    def ask_stream(self, lines):
        out = []
        for ln in lines:
            d = F.link.Deframer()
            delivered = d.feed_bytes(bytes.fromhex(ln.split(" ", 1)[1]))
            disc = sum(v for k, v in d.stats.items() if k != "delivered")
            block = [f"OK {F.C.frame_to_canonical(f)}" for f in delivered] + [f"END {disc}"]
            out.append(self.mangle(list(block)))
        return out


def test_run_torture_rejects_a_correct_end_with_a_wrong_ok_line():
    # A terminator-only comparison accepts this; the full-block comparison must not.
    def lie(block):
        if len(block) > 1:
            block[0] = block[0].replace("dst=0x", "dst=1x", 1)
        return block
    assert F.run_torture(_RefStreamHelper(lie), SEED, frames=20, per_class=3) == -1


def test_run_torture_rejects_a_dropped_ok_line_with_a_correct_end():
    def drop(block):
        return block[1:] if len(block) > 1 else block
    assert F.run_torture(_RefStreamHelper(drop), SEED, frames=20, per_class=3) == -1


def test_run_streams_agrees_with_the_reference_and_covers_order_and_reserved():
    assert F.run_streams(_RefStreamHelper(), SEED, count=60) == 60


def test_run_streams_rejects_reversed_frame_order():
    def rev(block):
        return list(reversed(block[:-1])) + block[-1:]
    assert F.run_streams(_RefStreamHelper(rev), SEED, count=60) == -1


def test_run_streams_corpus_has_multiframe_and_reserved_elements():
    # red-team round 9 on #121: the earlier version asserted against a COPY of the
    # generator, pinning nothing about run_streams itself. This one captures the ACTUAL
    # requests run_streams sends and derives every property from those bytes:
    # multi-frame delivery, shared delimiters (no doubled FLAG anywhere — unshared
    # concatenation would put two raw 0x7E bytes back to back), and the ReservedAddress
    # discard really occurring. Also pins the corpus floor, like FRAME_COUNT.
    assert F.STREAM_COUNT >= 1000

    class Capturing(_RefStreamHelper):
        def __init__(self):
            super().__init__()
            self.streams = []

        def ask_stream(self, lines):
            self.streams += [bytes.fromhex(ln.split(" ", 1)[1]) for ln in lines]
            return super().ask_stream(lines)

    helper = Capturing()
    assert F.run_streams(helper, SEED, count=60) == 60
    assert len(helper.streams) == 60
    multi = reserved = 0
    for stream in helper.streams:
        assert b"\x7e\x7e" not in stream  # shared delimiters: never two raw FLAGs adjacent
        d = F.link.Deframer()
        delivered = d.feed_bytes(stream)
        multi += len(delivered) >= 2
        reserved += d.stats.get("ReservedAddress", 0)
    assert multi == 60 and reserved == 12
