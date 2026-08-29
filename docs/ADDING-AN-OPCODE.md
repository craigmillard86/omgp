# Adding an opcode end-to-end

The walkthrough spec 001 SC-009 promises: a developer with no conversation history can add
a new L3 opcode in one pull request from the repository alone. Every step names the check
that fails if you skip it.

## 0. Decide, and get the ruling recorded

An opcode is protocol, so it is a human ruling (GOVERNANCE.md §1). Record the layout in
`docs/OPEN-QUESTIONS.md` (append-only, dated) or cite the spec section that already fixes
it. If the layout is provisional, say so there.

## 1. Definition file (T3 — human PR)

Edit `protocol/omgp-protocol.yaml` **only**:

```yaml
opcodes:
  GET_TEMP: {code: 0x16, target: module, idempotent: true}
l3_payloads:
  GET_TEMP:
    request:  []
    response: [{name: sensor, type: u8}, {name: milli_c, type: u16}]
```

Field types are `u8`, `u16`, or a trailing `bytes` (optionally `len_from: <field>`); add
`max:` for a protocol-defined upper bound. Do not use a code inside a reserved range.
Update the `docs/protocol-l3.md` §3.1 table in the same commit (CLAUDE.md rule 1).

Fails otherwise: `python3 tools/codegen.py` exits 2 naming the conflict (duplicate code,
reserved-range overlap, missing `l3_payloads` entry); `codegen.py --check-docs` exits 1
if the docs table lacks the row or has a different code.

## 2. Generate

```bash
python3 tools/codegen.py            # build/gen/omgp_protocol.{h,py}, omgp_names.h, omgp_vectors.h
```

`OP_GET_TEMP`, `PAYLOAD_INFO`, `PAYLOAD_FIELDS`, `OPCODE_NAMES`/`OPCODE_TABLE` now exist in
both languages. Nothing else knows about the opcode yet: `tools/refimpl/test_codegen.py`
stays green, and `test_l3_payload` `[rules]` still passes `payload_bounds` for it because
bounds come from the table.

## 3. Reference implementation first (Python)

- `tools/refimpl/omgp_l3.py`: a frozen dataclass and `encode_*`/`decode_*` pair with the
  §3-style checks (`Truncated`/`LengthMismatch`/`OutOfRange`), registered in `_TYPED`.
- `tools/refimpl/canonical.py`: rendering and parsing of its fields (contract:
  `specs/001-protocol-foundation/contracts/canonical-text.md`).
- `tools/refimpl/test_l3.py`: hand-compute the bytes from the layout and assert them —
  from the YAML/doc, not from the code you just wrote.
- `tools/refimpl/genvectors.py`: add request and response vectors (boundary values), run
  `python3 tools/refimpl/genvectors.py`, and **commit the new JSON with a justification** —
  `tests/vectors/` is immutable afterwards (CLAUDE.md rule 9). `genvectors.py --check` in the
  pipeline proves nothing else moved.

Fails otherwise: `python3 -m pytest tools/refimpl` (`test_vectors.py` binds Python to the
committed files; `test_vectors_complete.py` insists every opcode has vectors).

## 4. C++ codec, test first

- `tests/unit/test_l3_payload.cpp` `[rules]`: the same hand-computed bytes and rejections.
  `[vectors]` already iterates `omgp::vectors::ALL`, so the new vectors fail until the
  codec exists — run it, see it red.
- `l3/l3_types.hpp`: the POD struct. `l3/l3_payload.{hpp,cpp}`: `encode_*`/`decode_*`
  citing `// protocol-l3 §3.1 GET_TEMP`, using generated constants only.
- `tools/canonical.cpp`: render/parse branch for the opcode (mirror the Python strings).
- `tests/property/test_l3_roundtrip.cpp`: add a case to the seeded generator.

Fails otherwise: `tools/check_embedded.py` (a bare `0x16`, a missing citation, any heap or
exception use); the `[vectors]` test; the heap guard if the codec allocates.

## 5. Differential

`tools/diffcheck.py` picks the opcode up from `PAYLOAD_INFO` automatically; teach
`random_payload()` how to build a valid random instance. Run `python3 tools/diffcheck.py`:
byte-identical encoding and identical canonical decoding across ≥10k cases, plus the
invalid corpus.

## 6. Gate

```bash
./pipeline.sh                    # codegen quality build unit refimpl diffcheck scenarios
./pipeline.sh esp32              # the codec must compile for the ESP32-S3 too (rule 10)
```

Raise `UNIT_TEST_FLOOR` in `pipeline.sh` to the new executed count minus slack (never lower
it). Open the PR with claim-labelled evidence (CLAUDE.md rule 11) and a NOT EXAMINED section.
