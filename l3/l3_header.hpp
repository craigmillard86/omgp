// OMGP L3 codec — common header. protocol-l3 §3 (five-byte header), §2 (node ids).
// Embedded path: no heap, no exceptions; errors by Status. Buffers are caller-provided.
#pragma once

#include "l3_types.hpp"

namespace omgp {
namespace l3 {

constexpr size_t HEADER_LEN = 5;

// Writes exactly HEADER_LEN bytes or nothing (BufferTooSmall). Refuses reserved flag bits
// and, for requests, reserved node ids (§2: 0x80-0xFF MUST NOT be used in v1).
Status encode_header(const Header& h, uint8_t* out, size_t cap, size_t& written);

// Reads exactly HEADER_LEN bytes. Reserved flag bits are preserved, not rejected.
Status decode_header(const uint8_t* in, size_t len, Header& out);

// Header plus payload delimited by payload_len: Truncated if fewer bytes follow,
// LengthMismatch if more. payload views into `in`.
Status decode_message(const uint8_t* in, size_t len, Header& hdr, Bytes& payload);

} // namespace l3
} // namespace omgp
