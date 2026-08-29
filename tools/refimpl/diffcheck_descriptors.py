"""Descriptor half of tools/diffcheck.py (spec 001 T051): seeded valid descriptors and an
invalid corpus, compared through the helper's DENC / DDEC / DVAL verbs.

run(helper, seed) -> number of agreeing cases, or -1 after printing the first mismatches.
"""
from __future__ import annotations

import json
import pathlib
import random

from _gen import P
import canonical as C
import omgp_descriptor as D

G = P()
ROOT = pathlib.Path(__file__).resolve().parents[2]
VALID_COUNT = 1000

_WORDS = ["Gain", "Bass", "Treble", "Clean", "Crunch", "Lead", "Boost", "é", "日本", "a|b", 'q"t', "b\\s", " ",
          "x", "Presence", "Master", "Bright"]


def _string(rng: random.Random, max_bytes: int) -> str:
    """Random UTF-8 string within max_bytes, biased to the limit and to awkward characters."""
    target = rng.choice([0, 1, max_bytes, rng.randint(0, max_bytes)])
    s = ""
    while len(s.encode()) < target:
        w = rng.choice(_WORDS)
        if len((s + w).encode()) > target:
            break
        s += w
    return s


def random_records(rng: random.Random) -> list:
    recs = [D.ProtocolRec(rng.randint(0, 255), rng.randint(0, 255)),
            D.ModuleTypeRec(rng.choice(list(G.MODULE_TYPE_NAMES))),
            D.NameRec(_string(rng, 24)), D.ManufacturerRec(_string(rng, 24)),
            D.ModelIdRec(rng.randint(0, 65535), rng.randint(0, 65535), rng.randint(0, 65535))]
    if rng.random() < 0.5:
        recs.append(D.SerialRec(_string(rng, 16)))
    for i in range(rng.randint(1, 6)):
        recs.append(D.ChannelRec(i, _string(rng, 30)))
    recs.append(D.SwitchingRec(rng.randint(0, 255), rng.randint(0, 65535)))
    for i in range(rng.randint(1, 12)):
        recs.append(D.ParamRec(i, rng.choice([0xFF, rng.randint(0, 255)]), rng.choice(list(G.KIND_NAMES)),
                               rng.choice([0, G.LIMIT_param_value_max, rng.randint(0, G.LIMIT_param_value_max)]),
                               _string(rng, 30)))
        if rng.random() < 0.3:
            recs.append(D.ParamEnumRec(i, rng.randint(0, 7), _string(rng, 12)))
    recs.append(D.AudioRec(rng.randint(0, 255), rng.randint(0, 1), rng.randint(0, 65535), rng.randint(0, 65535)))
    recs.append(D.PowerLvRec(*(rng.randint(0, 65535) for _ in range(4))))
    if rng.random() < 0.5:
        recs.append(D.PowerTubeRec(rng.randint(1, 4), rng.randint(0, 255), rng.randint(0, 255), rng.randint(0, 65535),
                                   rng.randint(0, 65535), rng.randint(0, 65535), rng.randint(0, 255),
                                   rng.randint(0, 255)))
    if rng.random() < 0.5:
        recs.append(D.VendorRec(rng.randint(0, 65535), bytes(rng.randrange(256) for _ in range(rng.randint(0, 60)))))
    for _ in range(rng.randint(0, 3)):
        recs.append(D.UnknownRec(rng.choice([0x07, 0x12, 0x55, 0x7D, 0x80, 0xFE]),
                                 bytes(rng.randrange(256) for _ in range(rng.randint(0, 40)))))
    rng.shuffle(recs)  # record order is free; only required presence matters
    if rng.random() < 0.15:  # pad toward the 2048 cap with unknown records
        blob_len = len(D.build_descriptor(recs))
        while blob_len + 2 + 200 <= G.LIMIT_max_descriptor_bytes:
            recs.append(D.UnknownRec(0x60, bytes(200)))
            blob_len += 202
        room = G.LIMIT_max_descriptor_bytes - blob_len - 2
        if room >= 0:
            recs.append(D.UnknownRec(0x61, bytes(min(room, 255))))
    return recs


def invalid_corpus(rng: random.Random) -> list[bytes]:
    sample = bytes.fromhex(json.loads((ROOT / "tests" / "vectors" / "descriptor_sample.json").read_text())["bytes"]
                           .replace(" ", ""))
    recs = D.parse_descriptor(sample)
    out: list[bytes] = [sample[:i] for i in range(len(sample))]  # every truncation
    # each required record dropped; each non-repeated duplicated
    for i, r in enumerate(recs):
        t = D.TYPE_OF.get(type(r))
        if t is None:
            continue
        info = next(e for e in G.TLV_INFO if e["type"] == t)
        others = recs[:i] + recs[i + 1:]
        if info["required"]:
            out.append(_unchecked_build(others))
        if not info["repeated"]:
            out.append(_unchecked_build(recs + [r]))
    # rule violations spliced in raw
    out.append(_unchecked_build([D.NameRec("x" * 25)] + [r for r in recs if not isinstance(r, D.NameRec)]))
    out.append(_unchecked_build(recs + [D.SerialRec("s" * 17)]))
    out.append(sample + bytes([G.TLV_NAME, 2, 0xFF, 0xFE]))
    out.append(sample + bytes([G.TLV_PROTOCOL, 1, 1]))
    out.append(sample + bytes([G.TLV_CHANNEL, 0]))
    out.append(sample + bytes([G.TLV_MODULE_TYPE, 1, 0x63]))
    out.append(sample + bytes([G.TLV_AUDIO, 6, 3, 2, 0, 0, 0, 0]))
    out.append(sample + bytes([G.TLV_POWER_TUBE, 11, 0, 2, 4, 0, 0, 0, 0, 0, 0, 0, 0]))
    out.append(sample + bytes([G.TLV_PARAM, 6, 1, 0xFF, 6, 0, 0, 0x41]))
    out.append(sample + bytes([G.TLV_PARAM, 6, 1, 0xFF, 0, 0x00, 0x10, 0x41]))
    out.append(sample + bytes([0x55, 10, 1]))
    out.append(sample + b"\x55")
    out.append(sample + bytes(2049 - len(sample)))  # over the cap
    for _ in range(300):  # random byte flips
        b = bytearray(sample)
        for _ in range(rng.randint(1, 4)):
            b[rng.randrange(len(b))] = rng.randrange(256)
        out.append(bytes(b))
    for _ in range(200):  # junk
        out.append(bytes(rng.randrange(256) for _ in range(rng.randint(0, 300))))
    return out


def _unchecked_build(recs: list) -> bytes:
    """Concatenate records without validation, so rule violations reach the parsers."""
    out = bytearray()
    for r in recs:
        try:
            t, value = D.encode_value(r)
        except Exception:  # noqa: BLE001 — for corpus purposes any encodable form is fine
            continue
        out += bytes([t, len(value) & 0xFF]) + value[:255]
    return bytes(out)


def run(helper, seed: int) -> int:
    rng = random.Random(seed ^ 0xDE5C)
    requests, expect = [], []
    for i in range(VALID_COUNT):
        recs = random_records(rng)
        blob = D.build_descriptor(recs)
        canon = C.descriptor_to_canonical(recs)
        requests += [f"DENC {canon}", f"DDEC {blob.hex()}", f"DVAL {blob.hex()}"]
        expect += [(i, "DENC", canon, blob.hex()), (i, "DDEC", blob.hex(), canon),
                   (i, "DVAL", blob.hex(), C.validate_line(blob))]
    corpus = invalid_corpus(rng)
    for b in corpus:
        requests += [f"DDEC {b.hex()}", f"DVAL {b.hex()}"]
        expect += [(-1, "DDEC", b.hex(), C.render_descriptor_bytes(b)), (-1, "DVAL", b.hex(), C.validate_line(b))]
    answers = helper.ask(requests)
    bad = 0
    for (i, verb, req, want), got in zip(expect, answers):
        if got != want:
            print(f"diffcheck: DESCRIPTOR MISMATCH (seed={seed:#x}, index={i})\n  {verb} {req[:200]}\n"
                  f"  C++   : {got[:300]}\n  Python: {want[:300]}")
            bad += 1
            if bad >= 5:
                break
    return -1 if bad else VALID_COUNT + len(corpus)
