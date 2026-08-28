# Data Model: Protocol Foundation

**Feature**: 001-protocol-foundation | **Date**: 2026-08-28
**Sources**: `protocol/omgp-protocol.yaml`; `docs/protocol-l3.md` §2, §3, §3.1–3.4, §4,
§4.1; clarification rulings 2026-08-28 (spec FR-008, FR-009, FR-034); research R-11.

All multi-byte integers are **little-endian** (§4). Sizes in bytes. "LE16" = `u16`.

## 1. Protocol Definition (source of truth)

`protocol/omgp-protocol.yaml`, sections and the attributes each entry carries:

| Section | Key → value | Attributes emitted to code |
|---|---|---|
| `protocol` | `major`, `minor` | `PROTOCOL_MAJOR/MINOR` (stays 1.0 — ruling) |
| `limits` | name → int | `LIMIT_<name>` |
| `addressing` | name → int | `ADDR_<name>` |
| `l3_flags` | name → bit | `FLAG_<name>` |
| `opcodes` | NAME → `{code, target, idempotent, semantics?, note?, status?}` | `OP_<NAME>`; `OPCODE_INFO[]` rows `{code, target, idempotent}` |
| `reserved_opcode_ranges` | name → `{min,max}` | `RESERVED_<NAME>_MIN/MAX` |
| `error_codes` | ERR_* → int | as named |
| `node_states` | NAME → int | `STATE_<NAME>` |
| `events` | NAME → int (+ **new** `NONE: 0x00`, R-11) | `EVT_<NAME>` |
| `param_kinds` | NAME → int | `KIND_<NAME>` |
| `tlv` | NAME → `{type, required, repeated?, max_len?}` | `TLV_<NAME>`; `TLV_INFO[]` rows `{type, required, repeated, max_len}` sorted by type |
| `module_types` | NAME → int | `MT_<NAME>` |
| `link_trunk` | name → int or string | `TRUNK_<name>` (strings as `const char*`) |
| `module_bus` | name → int or string | `MBUS_<name>` |
| **new** `l3_payloads` | OPCODE → `{request: [fields], response: [fields]}` (FR-008 layouts; `opaque: true` for BP_*) | `PAYLOAD_INFO[]` `{code, req_len_min, req_len_max, resp_len_min, resp_len_max, opaque}` |
| **new** `descriptor` | `crc: crc16_ccitt_false` only — the 2048 cap remains `limits.max_descriptor_bytes` (single source) | `DESC_CRC` name; `DESC_MAX_BYTES` emitted as an alias of `LIMIT_max_descriptor_bytes` |

Validation on load (FR-004): unique `code` across `opcodes`; unique `type` across `tlv`;
unique values across `error_codes`, `events`, `module_types`, `node_states`,
`param_kinds`; every `code`/`type` ≤ 0xFF; `target ∈ {any, module, backplane,
response_only}`; `max_len` ≤ 255; reserved ranges do not overlap allocated opcodes.

## 2. L3 Message

### 2.1 Header (§3) — 5 bytes

| Offset | Field | Type | Rules |
|---|---|---|---|
| 0 | `opcode` | u8 | any value decodes; payload dispatch uses `OPCODE_INFO` |
| 1 | `node_id` | u8 | encode: request `node_id` ∈ [0x00, 0x7F] else `ReservedViolation` (§2 "MUST NOT be used in v1"); decode: any |
| 2 | `seq` | u8 | opaque to codec |
| 3 | `flags` | u8 | bit0 response, bit1 error; bits 2–7 reserved: encode refuses non-zero (`ReservedViolation`), decode preserves |
| 4 | `payload_len` | u8 | ≤ `LIMIT_max_l3_payload` (64) else `OutOfRange`; must equal bytes supplied after header else `Truncated` (fewer) / `LengthMismatch` (more, when decoding a delimited message) |

`Message` = `Header` + `payload[payload_len]` (view into caller buffer).

### 2.2 Opcode payloads (§3.1 + FR-008/FR-009 rulings + R-11 defaults)

Direction: H→N request, N→H response. "empty" = `payload_len` 0.

| Opcode | Code | Request payload | Response payload |
|---|---|---|---|
| PING | 0x01 | empty | empty (seq echoed in header) |
| IDENTIFY | 0x02 | empty | `major u8, minor u8, module_type u8, desc_len u16, desc_crc u16` — 7 B; `desc_len ≤ 2048`, `module_type` ∈ `module_types` else `OutOfRange` |
| READ_DESC | 0x03 | `offset u16, max_len u8` — 3 B; `offset < 2048` | `offset u16, len u8, bytes[len]` — 3+len B; `len ≤ 61`; `len` must equal remaining bytes |
| SELECT_CHANNEL | 0x10 | `channel u8` — 1 B | empty = accepted (R-11) |
| SET_BYPASS | 0x11 | `bypass u8` — 1 B; value ∈ {0,1} | empty (R-11) |
| SET_PARAM | 0x12 | `param_id u8, scope u8, value u16` — 4 B; `value ≤ 4095` (absolute — Principle V) | empty (R-11) |
| GET_PARAM | 0x13 | `param_id u8, scope u8` — 2 B | `param_id u8, scope u8, value u16` — 4 B; `value ≤ 4095` |
| GET_STATUS | 0x14 | empty | Status Block (2.3) — 7 B |
| GET_EVENT | 0x15 | empty | `event_type u8, remaining_count u8, detail[]` — ≥2 B; `event_type` ∈ events ∪ {0x00 NONE} ∪ [0xF0,0xFF] else `OutOfRange`; `detail` ≤ 62 B |
| BP_SLOT_MAP | 0x20 | opaque bytes (FR-009) | opaque bytes |
| BP_POWER | 0x21 | opaque | opaque |
| BP_ROUTE | 0x22 | opaque | opaque |
| ERROR | 0x7F | — (response only; `flags.error` expected set, not enforced by codec) | `code u8, detail[]` — ≥1 B; `code` ∈ `error_codes` else `OutOfRange`; detail ≤ 63 B |

Scope byte convention (SET_PARAM/GET_PARAM): `0xFF` = module, else channel index —
carried, not validated (referential checks out of scope).

### 2.3 Status Block (§3.3) — 7 bytes

| Offset | Field | Type | Rules |
|---|---|---|---|
| 0 | `state` | u8 | ∈ `node_states` (0–4) else `OutOfRange` |
| 1 | `active_channel` | u8 | any |
| 2 | `bypass` | u8 | ∈ {0,1} |
| 3 | `fault_code` | u8 | any (module-defined) |
| 4 | `uptime_s` | u16 | any |
| 6 | `event_pending` | u8 | any |

## 3. Descriptor (§4)

**Blob**: `u8 type, u8 len, u8[len] value` repeated; total ≤ `DESC_MAX_BYTES` (2048) —
checked before any record is read (`BlobTooLarge`). Strings: UTF-8, length-delimited,
no terminator, validated well-formed (`InvalidUtf8`).

**Identity for caching**: `(MODEL_ID, descriptor_crc)` where `descriptor_crc =
crc16_ccitt_false(blob, len)` (FR-034).

### 3.1 Record types (§4.1)

`len` column: exact, or minimum for records ending in a string/opaque tail.

| Type | Name | Layout (in order) | len | Req | Rep | Value rules |
|---|---|---|---|---|---|---|
| 0x01 | PROTOCOL | `major u8, minor u8` | =2 | ✔ | – | — |
| 0x02 | MODULE_TYPE | `type u8` | =1 | ✔ | – | ∈ `module_types` |
| 0x03 | NAME | `string` | 0..24 | ✔ | – | `max_len` 24 |
| 0x04 | MANUFACTURER | `string` | 0..24 | ✔ | – | `max_len` 24 |
| 0x05 | MODEL_ID | `vendor_model u16, hw_rev u16, fw_rev u16` | =6 | ✔ | – | — |
| 0x06 | SERIAL | `string` | 0..16 | – | – | `max_len` 16 |
| 0x10 | CHANNEL | `index u8, name string` | ≥1 | ✔ (≥1) | ✔ | — |
| 0x11 | SWITCHING | `flags u8, settle_ms u16` | =3 | ✔ | – | bits 2–7 of flags reserved: preserved |
| 0x20 | PARAM | `param_id u8, scope u8, kind u8, default u16, name string` | ≥5 | ✔ (≥1) | ✔ | `kind` 0..5; `default ≤ 4095` |
| 0x21 | PARAM_ENUM | `param_id u8, index u8, label string` | ≥2 | – | ✔ | — |
| 0x30 | AUDIO | `io_flags u8, input_mode u8, in_max_mVrms u16, out_max_mVrms u16` | =6 | ✔ | – | `input_mode` ∈ {0,1} |
| 0x40 | POWER_LV | `p15 u16, n15 u16, p9 u16, p5 u16` (mA) | =8 | ✔ | – | — |
| 0x41 | POWER_TUBE | `power_class u8, tubes u8, sections u8, heater_nom_mA u16, heater_max_mA u16, bplus_nom_V u16, bplus_exp_mA u8, bplus_max_mA u8` | =11 | – | – | `power_class` 1..4 |
| 0x7E | VENDOR | `vendor_id u16, opaque[]` | ≥2 | – | ✔ | opaque preserved |
| other | (unknown) | opaque | any | – | – | skipped by `len`; counted |

Fixed-length records with the wrong `len` → `MalformedRecord`. Strings longer than the
record allows (or than `max_len`) → `StringTooLong`.

### 3.2 Parse state machine (`RecordCursor` + `validate_descriptor`)

```
[Start] --len>2048--> BlobTooLarge
[Start] --ok--> [AtRecord(offset)]
[AtRecord] --offset==len--> [End] --required missing--> MissingRequired(type)
                                  --all present--> Ok(Report{skipped_unknown, counts})
[AtRecord] --remaining<2--> Truncated
[AtRecord] --offset+2+len>total--> Truncated
[AtRecord] --known type--> typed checks (len, ranges, utf8, max_len, duplicate) → next
[AtRecord] --unknown type--> skipped_unknown++ → next
```

`Report`: `{Status status; uint8_t type; uint16_t offset; uint16_t skipped_unknown;
uint16_t channel_count; uint16_t param_count;}`.

## 4. Status (error kinds) — shared vocabulary for both implementations

| Status | Meaning | Raised by |
|---|---|---|
| `Ok` | success | all |
| `Truncated` | fewer bytes than a length field or fixed layout requires | header, payload, descriptor |
| `LengthMismatch` | more bytes than the layout consumes / `payload_len` disagrees | payload, message |
| `OutOfRange` | field value outside protocol-defined set/range | payload, records |
| `UnknownOpcode` | opcode not in `OPCODE_INFO` (incl. reserved 0x60–0x6F) | payload dispatch |
| `MissingRequired` | required TLV absent (or repeated-required count 0) | descriptor |
| `DuplicateRecord` | non-repeated TLV seen twice | descriptor (parse and write) |
| `StringTooLong` | string exceeds `max_len` or record bound | descriptor (parse and write) |
| `BlobTooLarge` | descriptor > 2048 | descriptor (parse and write) |
| `BufferTooSmall` | caller output buffer insufficient (no partial write) | all encoders |
| `InvalidUtf8` | string not well-formed UTF-8 | descriptor (parse and write) |
| `MalformedRecord` | fixed-length record with wrong `len`, or `len` 0 where fields are required | descriptor |
| `ReservedViolation` | encoder asked to set reserved flag bits / reserved node id | header encode |

Python raises `L3Error(status: str, detail: str)` with `status` equal to the C++ enum
name; canonical output renders rejections as `ERR <Status>` in both.

## 5. Golden Vector

One JSON file per vector — schema `contracts/golden-vector.schema.json`:

```json
{
  "name": "msg_set_param_req",
  "kind": "message",              // "message" | "status" | "descriptor"
  "spec_ref": "protocol-l3 §3.1 SET_PARAM",
  "fields": { "opcode": "SET_PARAM", "node_id": 16, "seq": 3, "flags": 0,
              "param_id": 1, "scope": 255, "value": 4095 },
  "canonical": "op=SET_PARAM node=0x10 seq=3 flags=0x00 param_id=1 scope=0xFF value=4095",
  "bytes": "12 10 03 00 04 01 FF FF 0F"
}
```

Invariants: `bytes` is what the reference implementation produced from `fields`;
`canonical` is the rendering of `fields`; files are immutable (CLAUDE.md rule 9);
`genvectors.py --check` proves the set is reproducible. Descriptor vectors carry
`fields.records: [...]` and the complete sample includes every record type, one unknown
type (0x55), one VENDOR record, ≥2 CHANNEL, ≥3 PARAM (one enum with PARAM_ENUM rows),
and POWER_TUBE.

## 6. Differential Corpus

Generated in `tools/diffcheck.py` from `random.Random(0xB0071E)` (seed already used by the
CRC check): ≥10,000 valid messages covering every opcode and both directions with
boundary values (0, max, max+1 excluded here); ≥1,000 descriptors with random record
mixes including unknown types and vendor records; an invalid corpus (truncations at
every offset of each vector, length-field corruption, out-of-range values, duplicate
required records, over-cap blobs, invalid UTF-8) where both sides must emit the same
`ERR <Status>`. Every case is replayable from `(seed, index)`.

## 7. Fuzz Target and Mutation Report

- **Fuzz Target**: `{name, entry function, seed corpus dir, budget s}` × 4 (R-05).
  A finding = libFuzzer artefact file + reproducer command; the harness exit code is the
  gate.
- **Mutation Report**: `{diff_ref, files_in_scope, mutants_total, killed, survived,
  not_covered, kill_rate, threshold, survivors: [{file, line, mutator}]}` printed by
  `mutate.sh`; exit 1 when `kill_rate < threshold` or (`--require` and tool missing).
