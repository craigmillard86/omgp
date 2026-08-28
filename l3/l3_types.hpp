// OMGP L3 codec — shared types. protocol-l3 §3 (message model), §3.3 (status block),
// §4 (descriptor). Embedded path: C++17, no exceptions, no RTTI, no heap, no OS.
// Contract: specs/001-protocol-foundation/contracts/l3-codec-cpp.md
#pragma once

#include <cstddef>
#include <cstdint>

namespace omgp {
namespace l3 {

// Result of every codec operation. Errors by return value; Ok == 0.
enum class Status : uint8_t {
    Ok = 0,
    Truncated,         // fewer bytes than a length field or fixed layout requires
    LengthMismatch,    // more bytes than the layout consumes / payload_len disagrees
    OutOfRange,        // field value outside the protocol-defined set or range
    UnknownOpcode,     // opcode not in OPCODE_INFO (includes reserved 0x60-0x6F)
    MissingRequired,   // required TLV absent, or repeated-required count is zero
    DuplicateRecord,   // non-repeated TLV seen twice
    StringTooLong,     // string exceeds max_len or its record bound
    BlobTooLarge,      // descriptor longer than LIMIT_max_descriptor_bytes
    BufferTooSmall,    // caller output buffer insufficient (nothing written)
    InvalidUtf8,       // string is not well-formed UTF-8 (§4: "Strings UTF-8")
    MalformedRecord,   // fixed-length record with the wrong len, or len 0 where fields exist
    ReservedViolation, // encoder asked to set reserved flag bits or a reserved node id
};

// Enumerator name as a string literal ("Ok", "Truncated", ...). Used by tools and tests;
// never allocates.
const char* status_name(Status s);

// §3 common header, five bytes on the wire in this order.
struct Header {
    uint8_t opcode;
    uint8_t node_id;
    uint8_t seq;
    uint8_t flags;
    uint8_t payload_len;
};

// Non-owning views into caller memory. len fits uint8_t: payloads are <= 64 bytes and
// TLV values <= 255 bytes.
struct Bytes {
    const uint8_t* data;
    uint8_t len;
};
struct Str {
    const uint8_t* data; // UTF-8, length-delimited, no terminator (§4)
    uint8_t len;
};

// §3.1 payloads (request/response). POD, fixed size, no constructors.
struct IdentifyResp {
    uint8_t major;
    uint8_t minor;
    uint8_t module_type;
    uint16_t desc_len;
    uint16_t desc_crc;
};
struct ReadDescReq {
    uint16_t offset;
    uint8_t max_len;
};
struct ReadDescResp {
    uint16_t offset;
    Bytes bytes;
};
struct SelectChannelReq {
    uint8_t channel;
};
struct SetBypassReq {
    uint8_t bypass;
};
struct SetParamReq {
    uint8_t param_id;
    uint8_t scope;
    uint16_t value;
};
struct GetParamReq {
    uint8_t param_id;
    uint8_t scope;
};
struct GetParamResp {
    uint8_t param_id;
    uint8_t scope;
    uint16_t value;
};
// §3.3 status block, seven bytes on the wire.
struct StatusBlock {
    uint8_t state;
    uint8_t active_channel;
    uint8_t bypass;
    uint8_t fault_code;
    uint16_t uptime_s;
    uint8_t event_pending;
};
struct GetEventResp {
    uint8_t event_type;
    uint8_t remaining_count;
    Bytes detail;
};
// BP_SLOT_MAP / BP_POWER / BP_ROUTE: format not yet defined, passed through verbatim
// (spec 001 FR-009).
struct OpaquePayload {
    Bytes bytes;
};
struct ErrorResp {
    uint8_t code;
    Bytes detail;
};

// Direction of a payload, for dispatch by opcode (§3: bit0 of flags).
enum class Dir : uint8_t { Request = 0, Response = 1 };

// Result of validate_descriptor / DescriptorWriter::finish (§4).
struct DescriptorReport {
    Status status;
    uint8_t type;             // offending record type when status != Ok (0 otherwise)
    uint16_t offset;          // byte offset of the offending record (0 otherwise)
    uint16_t skipped_unknown; // records of unknown type skipped by length
    uint16_t channel_count;
    uint16_t param_count;
};

} // namespace l3
} // namespace omgp
