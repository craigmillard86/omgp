# Contract: generated constants (`tools/codegen.py`)

## CLI

```
python3 tools/codegen.py [--yaml FILE] [--out DIR] [--vectors DIR] [--check]
  --yaml     source (default protocol/omgp-protocol.yaml)
  --out      output dir (default build/gen)
  --vectors  also render omgp_vectors.h from DIR/*.json (default tests/vectors when present)
  --check    render to a temp dir and exit 1 with a unified diff if --out differs; write nothing
  --check-docs  verify docs/protocol-l3.md tables against the YAML (see Drift guard); write nothing
exit 0 ok | 1 drift (--check / --check-docs) | 2 YAML validation error (message names the conflict)
```

Prints exactly one line on success: `codegen: wrote <out>/omgp_protocol.h, omgp_protocol.py[, omgp_vectors.h]`
(existing pipeline/CI parse nothing from it; the drift guard uses `git diff` + `--check`).

## Outputs

### `omgp_protocol.h` (namespace `omgp`, `#pragma once`, `<cstdint>` only)

```cpp
// GENERATED from protocol/omgp-protocol.yaml -- DO NOT EDIT
inline constexpr uint8_t  PROTOCOL_MAJOR = 1;  inline constexpr uint8_t PROTOCOL_MINOR = 0;
inline constexpr uint8_t  OP_PING = 0x01; ...                    // opcodes, error codes, tlv, events,
inline constexpr uint32_t LIMIT_max_l3_payload = 64; ...         // module types, states, kinds, flags,
inline constexpr const char* TRUNK_crc = "crc16_ccitt_false";    // addressing, trunk/bus (ints as uint32_t,
                                                                 // strings as const char*)
enum class Target : uint8_t { Any, Module, Backplane, ResponseOnly };
struct OpcodeInfo { uint8_t code; Target target; bool idempotent; };
inline constexpr OpcodeInfo OPCODE_INFO[] = { {0x01, Target::Any, true}, ... };   // sorted by code
struct TlvInfo { uint8_t type; bool required; bool repeated; uint8_t max_len; };  // max_len 0 = none
inline constexpr TlvInfo TLV_INFO[] = { {0x01, true, false, 0}, ... };            // sorted by type
struct PayloadInfo { uint8_t code; uint8_t req_min, req_max, resp_min, resp_max; bool opaque; };
inline constexpr PayloadInfo PAYLOAD_INFO[] = { ... };                            // sorted by code
inline constexpr uint16_t DESC_MAX_BYTES = LIMIT_max_descriptor_bytes;   // alias — not a second YAML value
inline constexpr const char* DESC_CRC = "crc16_ccitt_false";
```

### `omgp_protocol.py`

Same names as module-level ints/strs; tables as tuples of dicts:
`OPCODE_INFO = ({"code": 0x01, "target": "any", "idempotent": True}, ...)`,
`TLV_INFO`, `PAYLOAD_INFO`; plus reverse maps `OPCODE_NAME = {0x01: "PING", ...}`,
`TLV_NAME`, `ERROR_NAME`, `EVENT_NAME`, `MODULE_TYPE_NAME` (Python only; C++ tests get
names via `status_name`/vectors).

### `omgp_vectors.h` (namespace `omgp::vectors`)

```cpp
struct Vector { const char* name; const char* kind; const char* spec_ref; const char* canonical;
                const uint8_t* bytes; uint16_t len; };
inline constexpr uint8_t V_msg_set_param_req[] = {0x12, 0x10, ...};
inline constexpr Vector ALL[] = { {"msg_set_param_req", "message", "...", "op=SET_PARAM ...", V_msg_set_param_req, 9}, ... };
inline constexpr size_t COUNT = ...;
```

Sorted by `name`. Used by Catch2 tests (decode → canonical equality; canonical → encode
→ bytes equality) and by `fuzz-smoke.sh` for seed corpora (via the JSON directly).

## Determinism contract (spec FR-002, SC-002)

- All mappings iterated in sorted key order; tables sorted by numeric code/type.
- No timestamps, paths, hostnames, tool versions in output.
- `\n` newlines, UTF-8, trailing newline, no trailing whitespace.
- **Test**: `sha256(run1) == sha256(run2)`; `sha256(run(shuffled_yaml)) == sha256(run1)`
  where `shuffled_yaml` is the same document with every mapping's key order reversed.
- **Drift guard** (CI codegen step): `python3 tools/codegen.py --check-docs` must exit
  0. It parses the `docs/protocol-l3.md` tables (§3.1 opcodes, error-code line, §3.4
  events, §4.1 record types) and the YAML, and fails naming any opcode/error/TLV/event/
  module type present in one but not the other, or present in both with different
  codes. The tables remain human-authored prose (rule 1: "update the affected docs
  table in the same commit"); this makes forgetting to do so mechanical. `--check` is a
  developer tool for verifying a `--out` directory and is not used by CI.

## Validation (spec FR-004) — exit 2 with message

Duplicate opcode code / TLV type / error code / event / module type / state / kind;
value > 0xFF where a byte is implied; `max_len` > 255; unknown `target`; reserved
opcode range overlapping an allocated opcode; `l3_payloads` referencing an opcode not
in `opcodes`; field widths in `l3_payloads` not in `{u8, u16, bytes}`.
