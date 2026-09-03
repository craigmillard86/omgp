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

def test_random_frame_covers_every_dst_src_and_seq_over_frame_count():
    rng = random.Random(SEED)
    frames = [F.random_frame(rng, i) for i in range(F.FRAME_COUNT)]
    assert {fr.dst for fr in frames} == set(range(0xFF))  # every non-reserved dst
    assert {fr.src for fr in frames} == set(range(0x100))  # every src
    assert {fr.seq for fr in frames} == set(range(16))  # seq 0-15
    lengths = {len(fr.payload) for fr in frames}
    assert 0 in lengths and F.MAX_PAYLOAD in lengths  # payload lengths 0-64, boundaries hit


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
    a = [F.random_frame(random.Random(SEED), i) for i in range(500)]
    b = [F.random_frame(random.Random(SEED), i) for i in range(500)]
    assert a == b


def test_random_frame_always_encodes_without_error():
    rng = random.Random(SEED)
    for i in range(2000):
        link.encode_frame(F.random_frame(rng, i))  # must not raise


# --- run_frames: FENC/FDEC round trip --------------------------------------------------------

def test_run_frames_agrees_reports_case_count():
    assert F.run_frames(FakeHelper(), SEED, count=50) == 50


def test_run_frames_reports_first_mismatch():
    assert F.run_frames(FakeHelper(wrong_at=3), SEED, count=50) == -1


def test_run_frames_index_replays_a_single_case():
    assert F.run_frames(FakeHelper(), SEED, count=50, only=7) == 1


# --- run_torture: FSTREAM over torture.corpus(seed) ------------------------------------------

def test_run_torture_agrees_over_a_small_full_corpus():
    n = F.run_torture(FakeHelper(), SEED, frames=20, per_class=3)
    assert n == 20 + 3 * len(torture.CLASSES)


def test_run_torture_reports_first_mismatch():
    assert F.run_torture(FakeHelper(wrong_at=1), SEED, frames=20, per_class=3) == -1


def test_run_torture_index_replays_a_single_valid_element():
    assert F.run_torture(FakeHelper(), SEED, only=0, frames=20, per_class=3) == 1


def test_run_torture_index_replays_a_single_corrupted_element():
    assert F.run_torture(FakeHelper(), SEED, only=20, frames=20, per_class=3) == 1
