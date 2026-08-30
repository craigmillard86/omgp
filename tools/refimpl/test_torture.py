"""Reference-implementation tests for the torture-corpus generator (spec 002 US1, T013).

Written from `specs/002-trunk-link-layer/contracts/link-python.md` ("torture.py" /
"pytest" sections), `research.md` R-08 and `data-model.md` §11 -- before
`tools/refimpl/torture.py` (T024) exists, per CLAUDE.md rule 8. Pins `corpus()`'s public
contract: `Element` is a flat one-corruption-per-element record (`stream`, `expected`,
`expected_discards`, `recipe`); see docs/OPEN-QUESTIONS.md "T013: torture-corpus
`Element` shape and `frames` parameter meaning" (2026-08-30) for why that reading was
chosen over data-model.md's richer `segments`/per-reason-dict sketch, and for why
`frames` is read as a floor on *valid, deliverable* frames rather than total elements.

This file must fail to collect until T024 lands (`torture` does not exist yet); the
failing run is the evidence CLAUDE.md rule 8 requires.
"""
from __future__ import annotations

import pathlib
import sys
from collections import Counter

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import omgp_link as L  # noqa: E402
import torture as T  # noqa: E402

SEED = 20260830

CLASSES = ("flip", "drop", "insert", "truncate", "flag", "bad_escape", "garbage", "overlength")


def _materialize(seed: int, **kwargs):
    return list(T.corpus(seed, **kwargs))


# --- determinism (contract: "corpus(seed) yields identical elements on every run") ---

def test_corpus_is_deterministic_across_independent_calls():
    # Two fully separate generator invocations -- not two iterations of one generator --
    # so this cannot pass by accident of shared internal state.
    first = _materialize(SEED)
    second = _materialize(SEED)
    assert len(first) == len(second)
    for a, b in zip(first, second):
        assert a.stream == b.stream
        assert a.expected == b.expected
        assert a.expected_discards == b.expected_discards
        assert a.recipe == b.recipe


# --- per-class minimum (contract: "every class present >= per_class times") ---

def test_every_corruption_class_meets_default_per_class_minimum():
    counts = Counter(element.recipe for element in T.corpus(SEED))
    for corruption_class in CLASSES:
        assert counts[corruption_class] >= 1_000, (
            f"class {corruption_class!r} occurred {counts[corruption_class]} times, "
            f"want >= 1000"
        )


# --- total size (contract: "total frames >= 10 000"; SC-002) ---

def test_corpus_meets_default_total_frame_minimum():
    corpus = T.corpus(SEED)
    delivered = sum(len(element.expected) for element in corpus)
    assert delivered >= 10_000


# --- no false accept (contract self-check: CRC-lucky corruptions are never emitted) ---

def test_no_corrupted_element_parses_as_a_frame_in_isolation():
    for element in T.corpus(SEED):
        if element.recipe not in CLASSES:
            continue  # not a corrupted element (recipe == "valid"): meant to parse
        delivered = L.Deframer().feed_bytes(element.stream)
        assert delivered == [], (
            f"corrupted element (recipe={element.recipe!r}) delivered a frame from its "
            f"corrupted segment in isolation: {delivered!r}"
        )


# --- class fidelity (review findings on PR #100, 2026-08-30) ---

def test_every_bad_escape_element_discards_as_BadEscape():
    # Review finding 1 (HIGH), red before the fix: the fallback branch inserted the ESC
    # and the bad byte at INDEPENDENT positions, so most short-frame "bad_escape"
    # elements never contained an invalid escape pair at all — they discarded as
    # BadLength/BadCrc while their recipe claimed BadEscape. The class label must mean
    # what it says: every bad_escape stream produces >= 1 BadEscape discard.
    # Full default class (>= 1000 elements): the pre-fix defect rate was 6/1000
    # (measured, seed 20260830), so a small sample cannot pin this.
    checked = 0
    for element in T.corpus(SEED):
        if element.recipe != "bad_escape":
            continue
        d = L.Deframer()
        d.feed_bytes(element.stream)
        assert d.stats["BadEscape"] >= 1, (
            f"bad_escape element produced no BadEscape discard (stats={d.stats}); "
            f"stream={element.stream.hex()}"
        )
        checked += 1
    assert checked >= 1_000


def test_overlength_class_crosses_the_cap_via_an_escape_at_least_sometimes():
    # Review finding 2 (MEDIUM), red before the fix: overlength bodies excluded ESC
    # entirely, so the corpus could never reproduce the escaped-71st-byte bug class
    # fixed in 1ad9ad3 (PR #99: the TooLong abort clobbered by the Escaped->InFrame
    # transition). Every overlength element must still discard TooLong, and a fair
    # share must reach the cap through a valid escape pair.
    total = escaped = 0
    for element in T.corpus(SEED):
        if element.recipe != "overlength":
            continue
        d = L.Deframer()
        d.feed_bytes(element.stream)
        assert d.stats["TooLong"] >= 1, (
            f"overlength element produced no TooLong discard (stats={d.stats})"
        )
        total += 1
        if T.ESC in element.stream[1:-1]:
            escaped += 1
    assert total >= 1_000
    assert escaped >= total // 4, (
        f"only {escaped} of {total} overlength elements contain an escape sequence — "
        f"the escaped-boundary class (1ad9ad3) is not represented"
    )


# --- parameterization (contract: corpus(seed, frames=..., per_class=...) honours args) ---

def test_corpus_honours_explicit_frames_and_per_class_arguments():
    frames, per_class = 200, 20
    corpus = _materialize(SEED, frames=frames, per_class=per_class)

    delivered = sum(len(element.expected) for element in corpus)
    assert delivered >= frames

    counts = Counter(element.recipe for element in corpus)
    for corruption_class in CLASSES:
        assert counts[corruption_class] >= per_class
