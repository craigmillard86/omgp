#!/usr/bin/env python3
"""Differential test: C++ implementation vs Python reference over seeded corpora.

Constitution Principle III: the two independent implementations must agree byte-for-byte on
encoding and field-for-field (canonical text) on decoding, and reject the same invalid
inputs with the same Status. One long-lived `l3_helper` process serves thousands of
requests (contracts/canonical-text.md), so the corpus stays inside the per-commit budget
(spec 001 SC-003: < 2 min).

    python3 tools/diffcheck.py [--count N] [--seed S] [--index I]
    python3 tools/diffcheck.py [--frames] [--frames-only] [--frame-index I] [--torture-index I]

Message, frame and torture cases are replayable from (seed, index): `--index I` runs only
that message case, `--frame-index I` one FENC/FDEC frame case, `--torture-index I` one
FSTREAM torture element; each prints the request and both results. Stream cases have no
index flag — their mismatch report prints a self-contained `printf ... | l3_helper`
replay line instead. The frame, torture and stream corpora (contracts/frame-vectors.md,
tools/refimpl/{torture,diffcheck_frames}.py) run by default alongside the
message/descriptor corpora; `--frames-only` restricts a run to just those three, for fast
local iteration on the link-layer codec (contracts/tooling.md).
"""
from __future__ import annotations

import argparse
import json
import pathlib
import queue
import random
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "refimpl"))

from _gen import P  # noqa: E402
from omgp_crc import crc16_ccitt_false  # noqa: E402
import canonical as C  # noqa: E402
import diffcheck_frames as F  # noqa: E402
import omgp_l3 as l3  # noqa: E402

G = P()
BIN = ROOT / "build" / "native"
DEFAULT_SEED = 0xB0071E  # fixed seed: reproducible corpus
# Requests in flight per pipe round trip. NOT a pipe-buffer bound (review on #121: one
# torture FSTREAM line reaches ~290 bytes, so a chunk can exceed the 64 KiB default pipe
# buffer) — the always-running _pump reader thread is what prevents write-side deadlock;
# the chunk size only bounds memory and batching latency.
CHUNK = 400
# A block that is still open after a protocol violation (a line that is neither "OK " nor
# "END ") waits this long for the "END " that a well-formed FSTREAM response always sends
# next (tools/canonical.cpp fstream_response()/fstream_lines(): both lines are flushed by
# l3_helper.cpp in a single write, so they are already sitting in the pipe and arrive well
# inside this bound). If that wait ever actually times out, the helper is presumed dead for
# the rest of the PROCESS: Helper._dead is per-Helper and never cleared, so every later
# block — and every later ask()/ask_stream() on this Helper — returns immediately instead
# of re-arming this timeout once per remaining request. That breadth is safe today only
# because main() returns non-zero on the first mismatch, so nothing consults the blanked
# answers (a control from main()'s current shape, not a Helper property — review round 8
# on #121). Without the stickiness, run_torture's whole-corpus batches would pay this
# timeout once per remaining request rather than once per run.
_STALL_TIMEOUT = 2.0


class Helper:
    """One l3_helper process. A single pump thread continuously drains stdout into a queue
    for the process's whole lifetime, so stdin writes (from ask()/ask_stream(), on whichever
    thread calls them) never block on a full stdout pipe (descriptor lines run to several
    KB)."""

    def __init__(self, path):
        # A list is taken as a full argv (review on #121: tests launch fake helpers via
        # `[sys.executable, script]` so they run under the SAME interpreter, independent of
        # shebang resolution and of noexec tmp filesystems); a single path is the binary.
        if isinstance(path, (list, tuple)):
            cmd = [str(p) for p in path]
        else:
            if not path.exists():
                sys.exit(f"diffcheck: build the native preset first ({path.name} missing)")
            cmd = [str(path)]
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                  bufsize=1)
        self._lines: queue.Queue[str | None] = queue.Queue()
        self._eof = False
        self._dead = False  # set once a stalled wait (see _STALL_TIMEOUT) times out: sticky
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in iter(self.p.stdout.readline, ""):
            self._lines.put(line.rstrip("\n"))
        self._lines.put(None)  # EOF: stdout closed (process exited), sent exactly once

    def _next_line(self, timeout: float | None = None) -> str | None:
        """A line, or None for EOF. Once EOF is seen it is remembered locally: the single
        sentinel in the queue must not be consumed by one caller and hidden from the rest of
        a batch still in flight. Likewise, once the helper is confirmed dead (self._dead) no
        further waiting is attempted at all -- not even the timeout the caller asked for."""
        if self._eof or self._dead:
            return None
        ln = self._lines.get(timeout=timeout)  # queue.Empty propagates to the caller
        if ln is None:
            self._eof = True
        return ln

    def _write(self, lines: list[str]) -> None:
        for i in range(0, len(lines), CHUNK):
            try:
                self.p.stdin.write("".join(l + "\n" for l in lines[i:i + CHUNK]))
                self.p.stdin.flush()
            except BrokenPipeError:
                # The helper died mid-batch (review round 4 on #121: an early segfault made
                # the NEXT chunk's write raise an unhandled traceback with no (seed, index)
                # and no crash attribution). Stop writing; the read side drains to EOF and
                # the mismatch report — with death_note naming the exit code — fires.
                return

    def ask(self, lines: list[str]) -> list[str]:
        # SCOPE (review round 8 on #121, rule 11): unlike ask_stream, this read has no
        # stall guard — a live-but-silent helper would block until the CI timeout.
        # Unreachable today because l3_helper prints exactly one response string per
        # request (a repo-contents control, the same one ask_stream's note names).
        self._write(lines)
        return [self._next_line() or "" for _ in lines]

    def ask_stream(self, lines: list[str]) -> list[list[str]]:
        """Like ask(), but each line is a FSTREAM request whose response is a
        variable-length block of "OK <frame>" lines terminated by one "END <n>" line
        (contracts/frame-vectors.md); the reader groups lines per request by that
        terminator instead of assuming one line per request."""
        self._write(lines)
        out: list[list[str]] = []
        for _ in lines:
            block: list[str] = []
            while True:
                # First line of a block, or still inside a normal "OK "-line run: no
                # timeout, since the helper may legitimately take a while to answer.
                # After a protocol violation (anything else) -- or once a prior block has
                # already confirmed the helper dead -- bound the wait for the "END " a
                # well-formed response always sends next, instead of blocking forever.
                # SCOPE (reviews on #121, rounds 4+9, rule 11): this guard arms only after
                # a NON-"OK " line. Two silent cases stay unbounded: a request answered
                # with ZERO lines (first line never arms), and one answered with "OK "
                # lines that then goes silent (last line keeps the guard off). Both are
                # unreachable today only because l3_helper's fstream response always ends
                # with an END line in the same flushed write — a control from the repo's
                # current contents, not a guarantee.
                stalled = self._dead or (bool(block) and not block[-1].startswith("OK "))
                try:
                    ln = self._next_line(timeout=_STALL_TIMEOUT if stalled else None)
                except queue.Empty:
                    self._dead = True  # confirmed silent: don't wait again for the rest of this run
                    break  # alive but silent after a protocol violation: truncated block
                if ln is None:
                    break  # EOF, or (self._dead already set) a later block in this same batch
                block.append(ln)
                if ln.startswith("END "):
                    break
            out.append(block)
        return out

    def close(self):
        try:
            self.p.stdin.write("QUIT\n")
            self.p.stdin.flush()
        except BrokenPipeError:
            pass  # helper already exited (e.g. after a crash we reported): nothing left to tell it
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            # review round 7 on #121: an abandoned mid-corpus batch leaves ~18k unread
            # requests behind QUIT; the child may not exit in time, and a TimeoutExpired
            # out of a `finally` would bury the already-printed mismatch report. Kill it —
            # the reported failure stays the one the operator needs.
            self.p.kill()
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
    if only is not None and not (0 <= only < count):
        # round 13 on #121: same guard as --frame-index/--torture-index — a negative index
        # must not silently wrap to the last case while the report prints -1.
        sys.exit(f"--index must be 0..{count - 1}")
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
            print(f"diffcheck: MESSAGE MISMATCH (seed={seed:#x}, index={i})\n  {verb} {req}\n"
                  f"  C++   : {got}{F.death_note(helper)}\n"
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
            # death_note (review round 5 on #121): the frame paths gained crash naming in
            # round 3; the message/invalid corpora share the same Helper and the same
            # EOF-derived blank, so they carry the same note for consistency.
            print(f"diffcheck: INVALID-CORPUS MISMATCH\n  DEC {b.hex()}\n  C++   : {got}{F.death_note(helper)}\n  Python: {want}")
            disagree += 1
            if disagree >= 5:
                break
    return -1 if disagree else agree


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--count", type=int, default=10000, help="valid message cases (default 10000)")
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=DEFAULT_SEED)
    # The three single-case replay flags are mutually exclusive (review round 5 on #121:
    # combining them silently ignored one), and --index needs the corpus --frames-only
    # skips.
    replay = ap.add_mutually_exclusive_group()
    replay.add_argument("--index", type=int, default=None, help="replay one message case")
    # contracts/tooling.md "tools/diffcheck.py --frames": the frame + torture corpora
    # already run by default (below); --frames is the explicit spelling for scripts that
    # want to say so, --frames-only restricts a local iteration run to just those three.
    # The DEFAULT inclusion is pinned by test_tooling's default-path test (frames >=
    # 10000 and torture > 0 asserted on a no-flag run — round 14; the earlier claim that
    # the `--frames-only --frames` invocation pinned it was false, that run passes the
    # flag). The --frames no-op itself is exercised by that same invocation.
    ap.add_argument("--frames", action="store_true", help="include the frame/torture/stream corpora (default)")
    ap.add_argument("--frames-only", action="store_true", help="run only the frame/torture/stream corpora")
    replay.add_argument("--frame-index", type=int, default=None, help="replay one frame (FENC/FDEC) case")
    replay.add_argument("--torture-index", type=int, default=None, help="replay one torture (FSTREAM) element")
    args = ap.parse_args(argv)
    if args.frames_only and args.index is not None:
        ap.error("--index replays a MESSAGE case, which --frames-only skips")

    t0 = time.monotonic()
    crc = msgs = inval = desc = streams = 0
    if not args.frames_only:
        crc = run_crc(random.Random(args.seed))
    helper = Helper(BIN / "l3_helper")
    try:
        if not args.frames_only:
            msgs = run_messages(helper, args.seed, args.count, args.index)
            if msgs < 0:
                return 1
            if args.index is not None:
                return 0
            inval = run_invalid(helper, args.seed)
            if inval < 0:
                return 1
            try:
                import diffcheck_descriptors  # type: ignore  # arrives with US3
                desc = diffcheck_descriptors.run(helper, args.seed)
                if desc < 0:
                    return 1
            except ImportError:
                pass  # descriptor corpus only exists once US3 lands — by design, not an error

        # A torture replay skips the frame corpus (review round 5 on #121: --torture-index
        # alone paid the full 10k-case frame run before reaching its one element).
        frames = 0 if args.torture_index is not None else F.run_frames(helper, args.seed, only=args.frame_index)
        if frames < 0:
            return 1
        if args.frame_index is not None:
            return 0
        torture_n = F.run_torture(helper, args.seed, only=args.torture_index)
        if torture_n < 0:
            return 1
        if args.torture_index is not None:
            return 0
        streams = F.run_streams(helper, args.seed)
        if streams < 0:
            return 1
    finally:
        helper.close()
    total = crc + msgs + inval + desc + frames + torture_n + streams
    notes = []
    if args.frames_only:
        notes.append("blind spot: crc/message/invalid/descriptor corpora not run")
    if torture_n:
        # review round 7 on #121: the reason-blindness was stated only in a module
        # docstring; the operator reading the run sees it here, like the other blind spot.
        notes.append("torture compares discard COUNTS; per-reason parity rests on each side's unit tests")
    blind_spot = f" ({'; '.join(notes)})" if notes else ""
    print(f"diffcheck: {total} cases, C++ and Python agree "
          f"(crc {crc}, messages {msgs}, invalid {inval}, descriptors {desc}, "
          f"frames {frames}, torture {torture_n}, streams {streams}) in {time.monotonic() - t0:.1f}s{blind_spot}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
