#!/usr/bin/env python3
"""Differential test: C++ implementation vs Python reference over seeded corpora.

Constitution Principle III: the two independent implementations must agree byte-for-byte on
encoding and field-for-field (canonical text) on decoding, and reject the same invalid
inputs with the same Status. One long-lived `l3_helper` process serves thousands of
requests (contracts/canonical-text.md), so the corpus stays inside the per-commit budget
(spec 001 SC-003: < 2 min).

    python3 tools/diffcheck.py [--count N] [--seed S] [--index I]

Every case is replayable from (seed, index): `--index I` runs only that case and prints
the request and both results.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import random
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "refimpl"))

from _gen import P  # noqa: E402
from omgp_crc import crc16_ccitt_false  # noqa: E402
import canonical as C  # noqa: E402
import omgp_l3 as l3  # noqa: E402

G = P()
BIN = ROOT / "build" / "native"
DEFAULT_SEED = 0xB0071E  # fixed seed: reproducible corpus
CHUNK = 400  # requests in flight per pipe round trip (well under the 64 KiB pipe buffer)


class Helper:
    """One l3_helper process; requests are streamed in while a reader thread drains the
    answers, so neither pipe can fill and deadlock (descriptor lines run to several KB)."""

    def __init__(self, path: pathlib.Path):
        if not path.exists():
            sys.exit(f"diffcheck: build the native preset first ({path.name} missing)")
        self.p = subprocess.Popen([str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                  bufsize=1)

    def ask(self, lines: list[str]) -> list[str]:
        import threading

        out: list[str] = []

        def reader():
            for _ in lines:
                out.append(self.p.stdout.readline().rstrip("\n"))

        t = threading.Thread(target=reader, daemon=True)
        t.start()
        for i in range(0, len(lines), CHUNK):
            self.p.stdin.write("".join(l + "\n" for l in lines[i:i + CHUNK]))
            self.p.stdin.flush()
        t.join()
        return out

    def close(self):
        try:
            self.p.stdin.write("QUIT\n")
            self.p.stdin.flush()
        except BrokenPipeError:
            pass
        self.p.wait(timeout=5)


# --- corpus generation -------------------------------------------------------------------

_DIRS = [(e["name"], d) for e in G.PAYLOAD_INFO for d in ("request", "response")
         if not (d == "request" and e["name"] == "ERROR")]


def _u8(rng, lo=0, hi=255):
    return rng.randint(lo, hi)


def _tail(rng, max_len):
    n = rng.choice([0, 1, max_len, rng.randint(0, max_len)])
    return bytes(rng.randrange(256) for _ in range(n))


def random_payload(rng: random.Random, name: str, direction: str):
    """A valid typed payload for (opcode, direction), boundary-biased."""
    codec = l3.codec_for(name, direction)
    if codec is None:
        return None
    cls = codec[0]
    vmax = G.LIMIT_param_value_max
    pick = lambda hi: rng.choice([0, hi, rng.randint(0, hi)])  # noqa: E731
    if cls is l3.IdentifyResp:
        return cls(_u8(rng), _u8(rng), rng.choice(list(G.MODULE_TYPE_NAMES)), pick(G.LIMIT_max_descriptor_bytes),
                   pick(0xFFFF))
    if cls is l3.ReadDescReq:
        return cls(pick(G.LIMIT_max_descriptor_bytes - 1), _u8(rng))
    if cls is l3.ReadDescResp:
        return cls(pick(G.LIMIT_max_descriptor_bytes - 1), _tail(rng, G.LIMIT_max_l3_payload - 3))
    if cls is l3.SelectChannelReq:
        return cls(_u8(rng))
    if cls is l3.SetBypassReq:
        return cls(rng.randint(0, 1))
    if cls is l3.SetParamReq:
        return cls(_u8(rng), _u8(rng), pick(vmax))
    if cls is l3.GetParamReq:
        return cls(_u8(rng), _u8(rng))
    if cls is l3.GetParamResp:
        return cls(_u8(rng), _u8(rng), pick(vmax))
    if cls is l3.StatusBlock:
        return cls(rng.choice(list(G.STATE_NAMES)), _u8(rng), rng.randint(0, 1), _u8(rng), pick(0xFFFF), _u8(rng))
    if cls is l3.GetEventResp:
        evt = rng.choice(list(G.EVENT_NAMES) + [rng.randint(G.EVT_USER_DEFINED_MIN, G.EVT_USER_DEFINED_MAX)])
        return cls(evt, _u8(rng), _tail(rng, G.LIMIT_max_l3_payload - 2))
    if cls is l3.OpaquePayload:
        return cls(_tail(rng, G.LIMIT_max_l3_payload))
    if cls is l3.ErrorResp:
        return cls(rng.choice(list(G.ERROR_NAMES)), _tail(rng, G.LIMIT_max_l3_payload - 1))
    raise AssertionError(cls)


def valid_message(rng: random.Random) -> tuple[l3.Header, object]:
    if rng.random() < 0.02:  # unknown opcode: forwarded verbatim by both sides
        unknown = rng.choice([0x00, 0x30, G.RESERVED_FIRMWARE_UPDATE_MIN, 0x63, 0x7E, 0x90, 0xFF])
        flags = rng.choice([0, G.FLAG_response])
        node = _u8(rng, 0, G.ADDR_module_max) if not flags else _u8(rng)
        return l3.Header(unknown, node, _u8(rng), flags, 0), l3.RawPayload(_tail(rng, G.LIMIT_max_l3_payload))
    name, direction = rng.choice(_DIRS)
    opcode = next(e["code"] for e in G.PAYLOAD_INFO if e["name"] == name)
    flags = G.FLAG_response if direction == "response" else 0
    if name == "ERROR":
        flags |= G.FLAG_error
    node = _u8(rng, 0, G.ADDR_module_max) if direction == "request" else _u8(rng)
    return l3.Header(opcode, node, _u8(rng), flags, 0), random_payload(rng, name, direction)


def invalid_corpus(rng: random.Random) -> list[bytes]:
    """Byte strings both sides must judge identically: truncations of every vector at every
    offset, length-field corruption, random byte flips, junk."""
    out: list[bytes] = []
    for f in sorted((ROOT / "tests" / "vectors").glob("msg_*.json")):
        raw = bytes.fromhex(json.loads(f.read_text())["bytes"].replace(" ", ""))
        out.extend(raw[:i] for i in range(len(raw)))
        out.append(raw + b"\x00")
        for plen in (len(raw) - 5 + 1, len(raw) - 5 - 1, 65, 0):
            if 0 <= plen <= 255:
                out.append(raw[:4] + bytes([plen]) + raw[5:])
        for _ in range(40):
            b = bytearray(raw)
            for _ in range(rng.randint(1, 3)):
                b[rng.randrange(len(b))] = rng.randrange(256)
            out.append(bytes(b))
    for _ in range(300):
        out.append(bytes(rng.randrange(256) for _ in range(rng.randint(0, 80))))
    return out


# --- runs ---------------------------------------------------------------------------------

def run_crc(rng: random.Random, count: int = 200) -> int:
    helper = BIN / "crc_helper"
    if not helper.exists():
        sys.exit("diffcheck: build the native preset first (crc_helper missing)")
    for i in range(count):
        data = bytes(rng.randrange(256) for _ in range(rng.randrange(0, 80)))
        cpp = int(subprocess.run([str(helper)], input=data, capture_output=True).stdout.strip(), 16)
        ref = crc16_ccitt_false(data)
        if cpp != ref:
            sys.exit(f"diffcheck: CRC MISMATCH on case {i}: cpp={cpp:#06x} ref={ref:#06x} data={data.hex()}")
    return count


def run_messages(helper: Helper, seed: int, count: int, only: int | None) -> int:
    rng = random.Random(seed)
    cases = [valid_message(rng) for _ in range(count)]
    indices = [only] if only is not None else range(count)
    requests, expect = [], []
    for i in indices:
        h, obj = cases[i]
        raw = C.encode_message(h, obj)
        canon = C.message_to_canonical(l3.decode_header(raw), obj)
        requests += [f"ENC {canon}", f"DEC {raw.hex()}"]
        expect += [(i, "ENC", canon, raw.hex()), (i, "DEC", raw.hex(), canon)]
    answers = helper.ask(requests)
    for (i, verb, req, want), got in zip(expect, answers):
        if got != want:
            print(f"diffcheck: MESSAGE MISMATCH (seed={seed:#x}, index={i})\n  {verb} {req}\n  C++   : {got}\n"
                  f"  Python: {want}\n  replay: python3 tools/diffcheck.py --seed {seed:#x} --index {i}")
            return -1
        if only is not None:
            print(f"  {verb} {req}\n  both: {got}")
    return len(indices)


def run_invalid(helper: Helper, seed: int) -> int:
    rng = random.Random(seed ^ 0xBAD)
    corpus = invalid_corpus(rng)
    answers = helper.ask([f"DEC {b.hex()}" for b in corpus])
    agree = disagree = 0
    for b, got in zip(corpus, answers):
        want = C.render_bytes(b)
        if got == want:
            agree += 1
        else:
            print(f"diffcheck: INVALID-CORPUS MISMATCH\n  DEC {b.hex()}\n  C++   : {got}\n  Python: {want}")
            disagree += 1
            if disagree >= 5:
                break
    return -1 if disagree else agree


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--count", type=int, default=10000, help="valid message cases (default 10000)")
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=DEFAULT_SEED)
    ap.add_argument("--index", type=int, default=None, help="replay one message case")
    args = ap.parse_args(argv)

    t0 = time.monotonic()
    crc = run_crc(random.Random(args.seed))
    helper = Helper(BIN / "l3_helper")
    try:
        msgs = run_messages(helper, args.seed, args.count, args.index)
        if msgs < 0:
            return 1
        if args.index is not None:
            return 0
        inval = run_invalid(helper, args.seed)
        if inval < 0:
            return 1
        desc = 0
        try:
            import diffcheck_descriptors  # type: ignore  # arrives with US3
            desc = diffcheck_descriptors.run(helper, args.seed)
            if desc < 0:
                return 1
        except ImportError:
            pass
    finally:
        helper.close()
    total = crc + msgs + inval + desc
    print(f"diffcheck: {total} cases, C++ and Python agree "
          f"(crc {crc}, messages {msgs}, invalid {inval}, descriptors {desc}) in {time.monotonic() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
