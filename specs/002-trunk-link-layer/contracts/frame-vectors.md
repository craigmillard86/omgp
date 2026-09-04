# Contract: `frame` golden vectors, canonical frame line, helper verbs

## Schema amendment (feature-001 `contracts/golden-vector.schema.json`, additive)

- `kind` enum gains `"frame"`; `name` pattern gains the `frame_` prefix.
- `fields` for `kind=frame`: `{"dst": int, "src": int, "response": bool, "retry": bool,
  "seq": int, "payload": "<hex, may be empty>"}`.
- `bytes`: the stuffed wire bytes **including both FLAGs**.
- `spec_ref`: `"trunk §4 …"`.

## Canonical frame line (extends feature-001 `canonical-text.md`)

```
frame dst=0x01 src=0x00 flags=0x00 seq=3 payload=0101000000
```
- `flags` renders the two low ctrl bits (`response` bit0, `retry` bit1) as `0x%02X`;
  `seq` decimal 0–15; `payload` contiguous lowercase hex, empty allowed.
- Rendering is of the **unstuffed fields**; the wire bytes are the vector's `bytes`.
- Discards render as `ERR <Discard>` (`ERR BadCrc`, `ERR BadLength`, `ERR BadEscape`,
  `ERR TooLong`, `ERR ReservedAddress` — a reserved-dst frame ARRIVING is a discard, so
  `FDEC` of one answers `ERR ReservedAddress`); encode refusals as `ERR <Status>`
  (`ERR PayloadTooLong`, `ERR ReservedAddress` — the same spelling for a reserved dst
  REQUESTED of the encoder). Malformed input TEXT — an unparseable canonical line,
  a field the LINE PARSER rejects as out of range, or bad hex — renders as
  `ERR BadRequest` on every frame verb (`FSTREAM` additionally emits its `END 0`
  terminator — see the table below). Carve-out (red-team round 4 on #122): a
  payload of 65–255 bytes PARSES and is refused by the codec as
  `ERR PayloadTooLong`; only ≥ 256 bytes fails the parser as `ERR BadRequest` —
  the 256 B split is deliberate (`tools/canonical.hpp`) and locked by
  `test_canonical_frame.cpp`.
  For `FDEC`, well-formed hex whose bytes run out mid-frame with no discard counted
  is ALSO `ERR BadRequest` (locked by `tests/unit/test_canonical_frame.cpp`
  "bytes running out mid-frame"), so `BadRequest` is not purely pre-codec there.
  Strict rejection is normative on both implementations (ruling 2026-09-03,
  docs/OPEN-QUESTIONS.md; wording corrected per red-team on #122).

## Vectors (created once by `genvectors.py`, then immutable)

| name | content | why |
|---|---|---|
| `frame_ping_req` | dst 0x01, src 0x00, seq 0, payload = the L3 PING request bytes | minimal real frame; the vector feature 001 named `msg_ping_req` travels inside it |
| `frame_response` | dst 0x00, src 0x01, response=1, seq 5, payload = an L3 PING response | response bit, non-zero seq |
| `frame_retry` | as `frame_ping_req` with retry=1, seq 15 | retry bit; seq at the 4-bit maximum |
| `frame_max_payload` | dst 0x01, src 0x00, flags 0, seq 0, 64-byte payload (0x00..0x3F) | `len` at the limit; exactly 72 wire bytes — demonstrated by execution with these pinned fields (CRC = 0xE3F2, neither byte escapes); an unpinned header could add 0–2 CRC stuffing bytes (ruling 2026-08-31) |
| `frame_worst_stuffing` | dst = src = 0x7D, response = 1, retry = 1, seq = 11 (`ctrl` 0xB3), payload = 64-byte 0x7D/0x7E mix `7E7E7E7D 7D7E7E7D 7E7E7E7D 7E7E7E7E 7D7D7E7D 7E7E7E7D 7D7D7D7E 7E7E7E7D 7E7D7D7E 7D7E7D7E 7E7D7D7D 7E7E7D7D 7D7D7E7E 7D7E7D7E 7E7E7D7D 7D7E7E7D` — CRC = 0x7D7E, so every escapable byte escapes | **140 wire bytes** — the structural ceiling, achieved (SC-008, ruling 2026-08-31; demonstrated by execution); an encoder-emitted `ctrl`/`len` can never escape, and CRC parity bars the receive-path 141 (SC-008), so `kMaxWire = 142` is a sizing bound, not a reachable length |

## `l3_helper` verbs (host-only, `tools/l3_helper.cpp`, `tools/canonical.cpp`)

| verb | in | out |
|---|---|---|
| `FENC <canonical frame line>` | fields | `OK <hex wire bytes>`, `ERR <Status>`, or `ERR BadRequest` (malformed text) |
| `FDEC <hex wire bytes>` | one frame's bytes | `OK <canonical frame line>`, `ERR <Discard>` (first discard reason), or `ERR BadRequest` (malformed hex, or bytes exhausted mid-frame) |
| `FSTREAM <hex stream>` | any byte stream | one `OK <canonical frame line>` per delivered frame, in order, then `END <discards>`; malformed hex yields `ERR BadRequest` **then `END 0`** — the `END` terminator is ALWAYS sent, so a read-until-`END` driver never blocks (locked by `test_canonical_frame.cpp`) |

`diffcheck.py --frames` uses `FENC`/`FDEC` on a seeded corpus of random valid frames
(≥ 10 000: every dst/src in range, seq 0–15, flags, payload lengths 0–64 with 7E/7D-heavy
content) and `FSTREAM` on every torture element, comparing line-for-line with the Python
reference; the first mismatch prints `(seed, index)`, the request and both outputs.
