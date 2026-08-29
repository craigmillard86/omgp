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
  `ERR TooLong`); encode refusals as `ERR <Status>` (`ERR PayloadTooLong`,
  `ERR ReservedAddress`).

## Vectors (created once by `genvectors.py`, then immutable)

| name | content | why |
|---|---|---|
| `frame_ping_req` | dst 0x01, src 0x00, seq 0, payload = the L3 PING request bytes | minimal real frame; the vector feature 001 named `msg_ping_req` travels inside it |
| `frame_response` | dst 0x00, src 0x01, response=1, seq 5, payload = an L3 PING response | response bit, non-zero seq |
| `frame_retry` | as `frame_ping_req` with retry=1, seq 15 | retry bit; seq at the 4-bit maximum |
| `frame_max_payload` | 64-byte payload (0x00..0x3F) | `len` at the limit; 72 wire bytes with no stuffing |
| `frame_worst_stuffing` | 64 × 0x7E payload, dst/src chosen so header and CRC also escape where possible | 142 wire bytes (SC-008) |

## `l3_helper` verbs (host-only, `tools/l3_helper.cpp`, `tools/canonical.cpp`)

| verb | in | out |
|---|---|---|
| `FENC <canonical frame line>` | fields | `OK <hex wire bytes>` or `ERR <Status>` |
| `FDEC <hex wire bytes>` | one frame's bytes | `OK <canonical frame line>` or `ERR <Discard>` (first discard reason) |
| `FSTREAM <hex stream>` | any byte stream | one `OK <canonical frame line>` per delivered frame, in order, then `END <discards>` |

`diffcheck.py --frames` uses `FENC`/`FDEC` on a seeded corpus of random valid frames
(≥ 10 000: every dst/src in range, seq 0–15, flags, payload lengths 0–64 with 7E/7D-heavy
content) and `FSTREAM` on every torture element, comparing line-for-line with the Python
reference; the first mismatch prints `(seed, index)`, the request and both outputs.
