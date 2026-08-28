# Contract: `omgp_l3` C++ API

Library target `omgp_l3` (`l3/`), namespace `omgp::l3`. Embedded-path: C++17,
`-fno-exceptions -fno-rtti`, no heap, no OS, no time. All functions are `noexcept`
by construction (no exceptions compiled) and re-entrant (no globals mutated).
Every header cites the protocol section it implements (`// protocol-l3 §3`).

## Types (`l3_types.hpp`)

```cpp
enum class Status : uint8_t {
  Ok = 0, Truncated, LengthMismatch, OutOfRange, UnknownOpcode, MissingRequired,
  DuplicateRecord, StringTooLong, BlobTooLarge, BufferTooSmall, InvalidUtf8,
  MalformedRecord, ReservedViolation
};
const char* status_name(Status);                    // "Ok", "Truncated", ... (for tools/tests)

struct Header { uint8_t opcode, node_id, seq, flags, payload_len; };   // §3

struct Bytes { const uint8_t* data; uint8_t len; };                    // view, ≤ 64
struct Str   { const uint8_t* data; uint8_t len; };                    // UTF-8 view

// §3.1 payloads (request/response) — POD, fixed size, no constructors
struct IdentifyResp { uint8_t major, minor, module_type; uint16_t desc_len, desc_crc; };
struct ReadDescReq  { uint16_t offset; uint8_t max_len; };
struct ReadDescResp { uint16_t offset; Bytes bytes; };
struct SelectChannelReq { uint8_t channel; };
struct SetBypassReq { uint8_t bypass; };
struct SetParamReq  { uint8_t param_id, scope; uint16_t value; };
struct GetParamReq  { uint8_t param_id, scope; };
struct GetParamResp { uint8_t param_id, scope; uint16_t value; };
struct StatusBlock  { uint8_t state, active_channel, bypass, fault_code; uint16_t uptime_s; uint8_t event_pending; }; // §3.3
struct GetEventResp { uint8_t event_type, remaining_count; Bytes detail; };
struct OpaquePayload { Bytes bytes; };              // BP_SLOT_MAP / BP_POWER / BP_ROUTE (FR-009)
struct ErrorResp    { uint8_t code; Bytes detail; };

struct DescriptorReport { Status status; uint8_t type; uint16_t offset;
                          uint16_t skipped_unknown, channel_count, param_count; };
```

## Header codec (`l3_header.hpp`)

```cpp
Status encode_header(const Header&, uint8_t* out, size_t cap, size_t& written);
// ReservedViolation if flags & 0xFC, or request (flags.response==0) node_id ≥ 0x80
// OutOfRange if payload_len > LIMIT_max_l3_payload; BufferTooSmall if cap < 5 (writes nothing)

Status decode_header(const uint8_t* in, size_t len, Header& out);
// Truncated if len < 5; OutOfRange if payload_len > 64; reserved flag bits preserved

Status decode_message(const uint8_t* in, size_t len, Header& hdr, Bytes& payload);
// as decode_header, then Truncated if len < 5+payload_len, LengthMismatch if len > 5+payload_len
```

## Payload codecs (`l3_payload.hpp`)

One `encode_*` / `decode_*` pair per row of data-model §2.2, all with the shape:

```cpp
Status encode_set_param(const SetParamReq&, uint8_t* out, size_t cap, size_t& written);
Status decode_set_param(const uint8_t* in, size_t len, SetParamReq& out);
// ... identify_resp, read_desc_req/resp, select_channel, set_bypass, get_param_req/resp,
//     status_block, get_event_resp, opaque (bp_*), error_resp
```

Rules common to all: encode → `BufferTooSmall` with no partial write; encode →
`OutOfRange` for values outside data-model rules (never clamps); decode → `Truncated`
if `len` < fixed size, `LengthMismatch` if `len` > layout consumes (variable-tail
payloads consume everything, so never `LengthMismatch`), `OutOfRange` per rules.
Empty payloads (PING, IDENTIFY req, GET_STATUS req, GET_EVENT req, accepted responses)
need no function: `payload_len == 0` is the contract.

Dispatch helper (used by tests, helper, fuzz):

```cpp
enum class Dir : uint8_t { Request, Response };
Status payload_bounds(uint8_t opcode, Dir, uint8_t& min_len, uint8_t& max_len, bool& opaque);
// UnknownOpcode for anything not in OPCODE_INFO (includes reserved 0x60–0x6F)
```

## Descriptor codec (`l3_descriptor.hpp`) — §4

```cpp
struct RecordView { uint8_t type; uint8_t len; const uint8_t* value; uint16_t offset; };

class RecordCursor {                     // zero-copy, O(1) state, no allocation
 public:
  RecordCursor(const uint8_t* blob, size_t len);      // does NOT check the 2048 cap
  Status next(RecordView& out);          // Ok / Truncated; returns false-equivalent at end
  bool at_end() const;
};

Status validate_descriptor(const uint8_t* blob, size_t len, DescriptorReport& report);
// BlobTooLarge before reading; then per data-model §3.2; required set from generated TLV_INFO

// typed decoders — one per known record type; MalformedRecord/OutOfRange/StringTooLong/InvalidUtf8
Status decode_protocol(const RecordView&, ProtocolRec&);      // ... module_type, name, manufacturer,
                                                              // model_id, serial, channel, switching,
                                                              // param, param_enum, audio, power_lv,
                                                              // power_tube, vendor
class DescriptorWriter {                 // caller buffer; enforces cap, max_len, duplicates, utf8
 public:
  DescriptorWriter(uint8_t* buf, size_t cap);         // cap ≤ 2048 enforced at construction
  Status add_protocol(const ProtocolRec&);            // ... one add_* per known type
  Status add_raw(uint8_t type, const uint8_t* value, uint8_t len);   // unknown/vendor passthrough
  size_t size() const;
  Status finish(DescriptorReport&);                   // runs validate_descriptor on the result
};

uint16_t descriptor_crc(const uint8_t* blob, size_t len);   // crc16_ccitt_false(blob) — FR-034
```

Typed record structs mirror data-model §3.1 with `Str` views for strings and
`Bytes` for opaque tails; none owns memory.

## Compile-time constants

All from `build/gen/omgp_protocol.h` (namespace `omgp`): `OP_*`, `ERR_*`, `TLV_*`,
`EVT_*`, `MT_*`, `STATE_*`, `KIND_*`, `FLAG_*`, `LIMIT_*`, `ADDR_*`, `TRUNK_*`,
`MBUS_*`, `OPCODE_INFO[]`, `TLV_INFO[]`, `PAYLOAD_INFO[]`, `DESC_MAX_BYTES`. `l3/` must
contain no literal that duplicates one of these (FR-005; `check_embedded.py`).

## Guarantees (labelled)

- No heap: *by construction* (no allocating constructs) and *demonstrated* natively by
  the `--wrap=malloc` link check.
- No read beyond input: *by construction* — every read is preceded by a bound check
  against `len`; *demonstrated* by ASan under fuzzing (SC-004).
- No exceptions/RTTI: *by construction* (`-fno-exceptions -fno-rtti` makes their use a
  compile error).
