// OMGP L3 codec — common header. protocol-l3 §3: u8 opcode, node_id, seq, flags
// (bit0 response, bit1 error, bits 2-7 reserved), payload_len (<= LIMIT_max_l3_payload).
#include "l3_header.hpp"

#include "omgp_protocol.h"

namespace omgp {
namespace l3 {

namespace {
// §3: bits 2-7 reserved. Derived from the generated flags, never a literal mask.
constexpr uint8_t kReservedFlagMask = static_cast<uint8_t>(~(FLAG_response | FLAG_error));
} // namespace

Status encode_header(const Header& h, uint8_t* out, size_t cap, size_t& written) {
    written = 0;
    if (h.flags & kReservedFlagMask)
        return Status::ReservedViolation;
    // §2: reserved node ids are never addressed by v1 requests; responses echo whatever
    // node answered, so they are not policed here.
    if (!(h.flags & FLAG_response) && h.node_id >= ADDR_reserved_min)
        return Status::ReservedViolation;
    if (h.payload_len > LIMIT_max_l3_payload)
        return Status::OutOfRange;
    if (cap < HEADER_LEN)
        return Status::BufferTooSmall;
    out[0] = h.opcode;
    out[1] = h.node_id;
    out[2] = h.seq;
    out[3] = h.flags;
    out[4] = h.payload_len;
    written = HEADER_LEN;
    return Status::Ok;
}

Status decode_header(const uint8_t* in, size_t len, Header& out) {
    if (len < HEADER_LEN)
        return Status::Truncated;
    out.opcode = in[0];
    out.node_id = in[1];
    out.seq = in[2];
    out.flags = in[3];
    out.payload_len = in[4];
    if (out.payload_len > LIMIT_max_l3_payload)
        return Status::OutOfRange;
    return Status::Ok;
}

Status decode_message(const uint8_t* in, size_t len, Header& hdr, Bytes& payload) {
    const Status st = decode_header(in, len, hdr);
    if (st != Status::Ok)
        return st;
    const size_t rest = len - HEADER_LEN;
    if (rest < hdr.payload_len)
        return Status::Truncated;
    if (rest > hdr.payload_len)
        return Status::LengthMismatch;
    payload.data = in + HEADER_LEN;
    payload.len = hdr.payload_len;
    return Status::Ok;
}

} // namespace l3
} // namespace omgp
