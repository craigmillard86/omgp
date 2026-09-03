"""Frame + torture halves of tools/diffcheck.py (spec 002 US1, T025): a seeded corpus of
valid frames round-tripped through FENC/FDEC, and every torture.corpus(seed) element
replayed through FSTREAM — both compared against the Python reference
(contracts/tooling.md, contracts/frame-vectors.md "l3_helper verbs").

run_frames(helper, seed, count, only) / run_torture(helper, seed, only, frames, per_class)
-> number of agreeing cases, or -1 after printing the first mismatch. `helper` only needs
`.ask(lines) -> list[str]` (FENC/FDEC: one line in, one line out) and
`.ask_stream(lines) -> list[list[str]]` (FSTREAM: one line in, a variable-length block of
response lines out, the caller's Helper.ask_stream splits on "END " terminators).

Blind spot (stated per the working agreement; review on #121): the corpus is valid-only
by construction and FSTREAM reports discards as a count, so the FENC refusal spellings
(ERR PayloadTooLong/ReservedAddress) and per-reason FDEC discards get no differential
coverage here — they are pinned by unit tests on each side, not by C++/Python agreement.
"""
from __future__ import annotations

import random
import sys

from _gen import P
import canonical as C
import omgp_link as link
import torture

G = P()
FRAME_COUNT = 10_000
MAX_PAYLOAD = G.LIMIT_max_l3_payload
# trunk §5: dst 0xFF is reserved; excluded from the valid-frame corpus. Imported from the
# reference codec rather than restated (review on #121: this was the third independent
# hardcoding — the YAML has no symbol for the L2 reserved dst, so link.py is the anchor).
_RESERVED_DST = link._RESERVED_DST
_VALID_DST = [d for d in range(0x100) if d != _RESERVED_DST]  # explicit, not a coincidental modulus
_FRAME_SEED_XOR = 0xF12A5E
_TORTURE_SEED_XOR = 0x70127E


def _biased_byte(rng: random.Random) -> int:
    """7E/7D-heavy content (frame-vectors.md): half the bytes are FLAG/ESC so the corpus
    stresses byte-stuffing on both encode and decode."""
    if rng.random() < 0.5:
        return rng.choice((G.TRUNK_flag_byte, G.TRUNK_escape_byte))
    return rng.randrange(256)


def _payload_len(rng: random.Random, index: int) -> int:
    boundary = (0, 1, MAX_PAYLOAD, MAX_PAYLOAD - 1)
    return boundary[index] if index < len(boundary) else rng.randint(0, MAX_PAYLOAD)


def random_frame(rng: random.Random, index: int) -> link.Frame:
    """A boundary- and coverage-biased valid frame: every dst/src address and seq value is
    hit at least once over FRAME_COUNT draws (frame-vectors.md's `l3_helper` verbs
    section: "every dst/src in range, seq 0-15, flags, payload lengths 0-64 with
    7E/7D-heavy content")."""
    dst = _VALID_DST[index % len(_VALID_DST)]  # every non-reserved dst, over >= 255 draws
    src = index % 0x100  # 0..255: every src, over >= 256 draws
    # seq is drawn independently of src/index (once its 0-15 range is covered by the first
    # 16 draws) so it is never a fixed function of src — see
    # test_random_frame_seq_is_not_a_function_of_src.
    seq = index % 16 if index < 16 else rng.randrange(16)
    length = _payload_len(rng, index)
    payload = bytes(_biased_byte(rng) for _ in range(length))
    return link.Frame(dst=dst, src=src, response=rng.getrandbits(1) == 1, retry=rng.getrandbits(1) == 1,
                      seq=seq, payload=payload)


def _death_note(helper) -> str:
    """Names a crashed helper in a mismatch report (review round 3 on #121: EOF maps to an
    empty string, so a segfault printed as a content mismatch with a blank C++ side)."""
    proc = getattr(helper, "p", None)
    rc = proc.poll() if proc is not None else None
    return "" if rc is None else f"   <-- helper exited rc={rc}: crash, not a content mismatch"


def run_frames(helper, seed: int, count: int = FRAME_COUNT, only: int | None = None) -> int:
    # Operator-facing replay path: a clean diagnostic beats an IndexError, and a negative
    # index must not silently wrap to case count-1 while the report prints -1 (review #121).
    if only is not None and not 0 <= only < count:
        sys.exit(f"--frame-index must be 0..{count - 1}")
    rng = random.Random(seed ^ _FRAME_SEED_XOR)
    cases = [random_frame(rng, i) for i in range(count)]
    indices = [only] if only is not None else range(count)
    requests, expect = [], []
    for i in indices:
        f = cases[i]
        canon = C.frame_to_canonical(f)
        wire_hex = link.encode_frame(f).hex()
        requests += [f"FENC {canon}", f"FDEC {wire_hex}"]
        expect += [(i, "FENC", canon, f"OK {wire_hex}"), (i, "FDEC", wire_hex, f"OK {canon}")]
    answers = helper.ask(requests)
    for (i, verb, req, want), got in zip(expect, answers):
        if got != want:
            print(f"diffcheck: FRAME MISMATCH (seed={seed:#x}, index={i})\n  {verb} {req}\n  C++   : {got}{_death_note(helper)}\n"
                  f"  Python: {want}\n  replay: python3 tools/diffcheck.py --frames-only --seed {seed:#x} --frame-index {i}")
            return -1
        if only is not None:
            print(f"  {verb} {req}\n  both: {got}")
    return len(indices)


def run_torture(helper, seed: int, only: int | None = None, **corpus_kwargs) -> int:
    # Corpus sizing lives in torture.corpus's OWN defaults (review on #121: re-declaring
    # frames/per_class here silently pinned the old sizes if torture.py ever raised its
    # coverage — the same drift the FRAME_COUNT pin closes on the frame side). Tests may
    # still pass small frames=/per_class= overrides; they flow through unre-declared.
    elements = list(torture.corpus(seed ^ _TORTURE_SEED_XOR, **corpus_kwargs))
    if only is not None and not 0 <= only < len(elements):
        sys.exit(f"--torture-index must be 0..{len(elements) - 1}")
    indices = [only] if only is not None else range(len(elements))
    requests = [f"FSTREAM {elements[i].stream.hex()}" for i in indices]
    blocks = helper.ask_stream(requests)
    for i, block in zip(indices, blocks):
        elem = elements[i]
        want = [f"OK {C.frame_to_canonical(fr)}" for fr in elem.expected] + [f"END {elem.expected_discards}"]
        if block != want:
            print(f"diffcheck: TORTURE MISMATCH (seed={seed:#x}, index={i}, recipe={elem.recipe})\n"
                  f"  FSTREAM {elem.stream.hex()}\n  C++   : {block}{_death_note(helper)}\n  Python: {want}\n"
                  f"  replay: python3 tools/diffcheck.py --frames-only --seed {seed:#x} --torture-index {i}")
            return -1
        if only is not None:
            print(f"  FSTREAM {elem.stream.hex()}\n  both: {block}")
    return len(indices)
