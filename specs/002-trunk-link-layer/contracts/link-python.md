# Contract: Python reference for framing (`tools/refimpl/omgp_link.py`) and the torture generator

Independent reference for the frame codec only (constitution Principle III; the engines
and health tracker are behavioural and are verified by the scripted-transport tests).
Written from `docs/trunk-link-layer.md` §4 and the rulings, before the C++ deframer.
Python 3.10-compatible; constants from `_gen.P()` (generated module), never restated.

## `omgp_link.py`

```python
@dataclass(frozen=True)
class Frame: dst: int; src: int; response: bool; retry: bool; seq: int; payload: bytes

def stuff(b: bytes) -> bytes            # 0x7E→7D 5E, 0x7D→7D 5D
def unstuff(b: bytes) -> bytes          # raises FrameError("BadEscape") on 7D + other / trailing 7D
def encode_frame(f: Frame) -> bytes     # FLAG + stuff(hdr+payload+crc_le) + FLAG; FrameError PayloadTooLong / ReservedAddress
def crc(hdr_and_payload: bytes) -> int  # omgp_crc.crc16_ccitt_false

class Deframer:
    def feed(self, byte: int) -> Frame | None      # same three states and discard reasons as the C++ (R-03)
    def feed_bytes(self, data: bytes) -> list[Frame]
    stats: dict[str, int]                          # delivered, BadCrc, BadLength, BadEscape, TooLong, ReservedAddress
```
`FrameError.reason` uses the C++ `Status`/`Discard` names verbatim so the differential can
compare reasons textually. A received frame with `dst == 0xFF` is discarded as
`ReservedAddress` (never delivered) — it could never be re-encoded, since `encode_frame`
refuses that address (ruling 2026-08-31, docs/OPEN-QUESTIONS.md).

## `torture.py` (research R-08)

```python
def corpus(seed: int, frames: int = 10_000, per_class: int = 1_000) -> Iterator[Element]
Element = (stream: bytes, expected: list[Frame], expected_discards: int, recipe: str)
```
- Corruption classes: `flip`, `drop`, `insert`, `truncate`, `flag`, `bad_escape`,
  `garbage`, `overlength`; each applied to a copy of a valid frame or between frames.
- Self-check: the reference `Deframer` parses each corrupted segment in isolation; if it
  yields a valid frame (CRC-lucky), the corruption is regenerated with the next sub-seed.
  The emitted corpus therefore contains no accepted corruptions by construction.
- Deterministic: `corpus(seed)` yields identical elements on every run and machine
  (`random.Random(seed)` only, no set iteration order).

## pytest

- `test_link.py`: stuff/unstuff identities (including 7E/7D-only payloads); encode/decode
  of every `frame_*` vector byte-for-byte; the published CCITT-FALSE check
  (`b"123456789"` → 0x29B1) through `crc()`; deframer discard reasons for each malformed
  input; resync after each corruption class; a 71-byte unstuffed frame is `TooLong`.
- `test_torture.py`: corpus determinism; no element's corruption parses as a frame; every
  class present ≥ `per_class` times; total frames ≥ 10 000.
