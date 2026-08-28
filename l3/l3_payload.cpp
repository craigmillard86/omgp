// OMGP L3 codec — opcode payloads. protocol-l3 §3.1, §3.3, §3.4. Multi-byte integers
// little-endian (§4 convention). All range sets come from the generated code tables.
#include "l3_payload.hpp"

#include "omgp_protocol.h"

namespace omgp {
namespace l3 {

namespace {

constexpr size_t kMaxPayload = LIMIT_max_l3_payload;

inline void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline uint16_t get16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// Fixed layouts: shorter is Truncated, longer is LengthMismatch.
inline Status fixed_len(size_t len, size_t n) {
    if (len < n)
        return Status::Truncated;
    if (len > n)
        return Status::LengthMismatch;
    return Status::Ok;
}

template <size_t N> inline bool in_codes(const uint8_t (&table)[N], uint8_t v) {
    for (size_t i = 0; i < N; ++i)
        if (table[i] == v)
            return true;
    return false;
}

inline bool valid_event(uint8_t t) { // §3.4 incl. USER_DEFINED 0xF0-0xFF
    return in_codes(EVENT_CODES, t) || (t >= EVT_USER_DEFINED_MIN && t <= EVT_USER_DEFINED_MAX);
}

inline Status copy_tail(const uint8_t* src, size_t n, uint8_t* dst) {
    for (size_t i = 0; i < n; ++i)
        dst[i] = src[i];
    return Status::Ok;
}

} // namespace

// --- IDENTIFY response: u8 major, u8 minor, u8 module_type, u16 desc_len, u16 desc_crc ---

namespace {
constexpr size_t kIdentifyLen = 7;
inline Status check_identify(const IdentifyResp& r) {
    if (!in_codes(MODULE_TYPE_CODES, r.module_type))
        return Status::OutOfRange;
    if (r.desc_len > LIMIT_max_descriptor_bytes)
        return Status::OutOfRange;
    return Status::Ok;
}
} // namespace

Status encode_identify_resp(const IdentifyResp& r, uint8_t* out, size_t cap, size_t& written) {
    written = 0;
    const Status st = check_identify(r);
    if (st != Status::Ok)
        return st;
    if (cap < kIdentifyLen)
        return Status::BufferTooSmall;
    out[0] = r.major;
    out[1] = r.minor;
    out[2] = r.module_type;
    put16(out + 3, r.desc_len);
    put16(out + 5, r.desc_crc);
    written = kIdentifyLen;
    return Status::Ok;
}

Status decode_identify_resp(const uint8_t* in, size_t len, IdentifyResp& out) {
    const Status st = fixed_len(len, kIdentifyLen);
    if (st != Status::Ok)
        return st;
    out.major = in[0];
    out.minor = in[1];
    out.module_type = in[2];
    out.desc_len = get16(in + 3);
    out.desc_crc = get16(in + 5);
    return check_identify(out);
}

// --- READ_DESC: request u16 offset, u8 max_len; response u16 offset, u8 len, u8[len] ---

namespace {
constexpr size_t kReadDescReqLen = 3;
constexpr size_t kReadDescRespHead = 3;
constexpr size_t kReadDescTailMax = kMaxPayload - kReadDescRespHead;
} // namespace

Status encode_read_desc_req(const ReadDescReq& r, uint8_t* out, size_t cap, size_t& written) {
    written = 0;
    if (r.offset >= LIMIT_max_descriptor_bytes)
        return Status::OutOfRange;
    if (cap < kReadDescReqLen)
        return Status::BufferTooSmall;
    put16(out, r.offset);
    out[2] = r.max_len;
    written = kReadDescReqLen;
    return Status::Ok;
}

Status decode_read_desc_req(const uint8_t* in, size_t len, ReadDescReq& out) {
    const Status st = fixed_len(len, kReadDescReqLen);
    if (st != Status::Ok)
        return st;
    out.offset = get16(in);
    out.max_len = in[2];
    if (out.offset >= LIMIT_max_descriptor_bytes)
        return Status::OutOfRange;
    return Status::Ok;
}

Status encode_read_desc_resp(const ReadDescResp& r, uint8_t* out, size_t cap, size_t& written) {
    written = 0;
    if (r.offset >= LIMIT_max_descriptor_bytes)
        return Status::OutOfRange;
    if (r.bytes.len > kReadDescTailMax)
        return Status::OutOfRange;
    const size_t total = kReadDescRespHead + r.bytes.len;
    if (cap < total)
        return Status::BufferTooSmall;
    put16(out, r.offset);
    out[2] = r.bytes.len;
    copy_tail(r.bytes.data, r.bytes.len, out + kReadDescRespHead);
    written = total;
    return Status::Ok;
}

Status decode_read_desc_resp(const uint8_t* in, size_t len, ReadDescResp& out) {
    if (len < kReadDescRespHead)
        return Status::Truncated;
    const uint8_t n = in[2];
    const size_t tail = len - kReadDescRespHead;
    if (tail < n)
        return Status::Truncated;
    if (tail > n)
        return Status::LengthMismatch;
    out.offset = get16(in);
    if (out.offset >= LIMIT_max_descriptor_bytes)
        return Status::OutOfRange;
    if (n > kReadDescTailMax)
        return Status::OutOfRange;
    out.bytes.data = in + kReadDescRespHead;
    out.bytes.len = n;
    return Status::Ok;
}

// --- one-byte requests -------------------------------------------------------------------

Status encode_select_channel(const SelectChannelReq& r, uint8_t* out, size_t cap, size_t& written) {
    written = 0;
    if (cap < 1)
        return Status::BufferTooSmall;
    out[0] = r.channel;
    written = 1;
    return Status::Ok;
}

Status decode_select_channel(const uint8_t* in, size_t len, SelectChannelReq& out) {
    const Status st = fixed_len(len, 1);
    if (st != Status::Ok)
        return st;
    out.channel = in[0];
    return Status::Ok;
}

Status encode_set_bypass(const SetBypassReq& r, uint8_t* out, size_t cap, size_t& written) {
    written = 0;
    if (r.bypass > 1) // §3.1: payload = 0/1
        return Status::OutOfRange;
    if (cap < 1)
        return Status::BufferTooSmall;
    out[0] = r.bypass;
    written = 1;
    return Status::Ok;
}

Status decode_set_bypass(const uint8_t* in, size_t len, SetBypassReq& out) {
    const Status st = fixed_len(len, 1);
    if (st != Status::Ok)
        return st;
    out.bypass = in[0];
    return out.bypass > 1 ? Status::OutOfRange : Status::Ok;
}

// --- parameters: u8 param_id, u8 scope (0xFF module, else channel), u16 value 0-4095 ---

namespace {
constexpr size_t kParamLen = 4;
constexpr size_t kGetParamReqLen = 2;
} // namespace

Status encode_set_param(const SetParamReq& r, uint8_t* out, size_t cap, size_t& written) {
    written = 0;
    if (r.value > LIMIT_param_value_max)
        return Status::OutOfRange;
    if (cap < kParamLen)
        return Status::BufferTooSmall;
    out[0] = r.param_id;
    out[1] = r.scope;
    put16(out + 2, r.value);
    written = kParamLen;
    return Status::Ok;
}

Status decode_set_param(const uint8_t* in, size_t len, SetParamReq& out) {
    const Status st = fixed_len(len, kParamLen);
    if (st != Status::Ok)
        return st;
    out.param_id = in[0];
    out.scope = in[1];
    out.value = get16(in + 2);
    return out.value > LIMIT_param_value_max ? Status::OutOfRange : Status::Ok;
}

Status encode_get_param_req(const GetParamReq& r, uint8_t* out, size_t cap, size_t& written) {
    written = 0;
    if (cap < kGetParamReqLen)
        return Status::BufferTooSmall;
    out[0] = r.param_id;
    out[1] = r.scope;
    written = kGetParamReqLen;
    return Status::Ok;
}

Status decode_get_param_req(const uint8_t* in, size_t len, GetParamReq& out) {
    const Status st = fixed_len(len, kGetParamReqLen);
    if (st != Status::Ok)
        return st;
    out.param_id = in[0];
    out.scope = in[1];
    return Status::Ok;
}

Status encode_get_param_resp(const GetParamResp& r, uint8_t* out, size_t cap, size_t& written) {
    written = 0;
    if (r.value > LIMIT_param_value_max)
        return Status::OutOfRange;
    if (cap < kParamLen)
        return Status::BufferTooSmall;
    out[0] = r.param_id;
    out[1] = r.scope;
    put16(out + 2, r.value);
    written = kParamLen;
    return Status::Ok;
}

Status decode_get_param_resp(const uint8_t* in, size_t len, GetParamResp& out) {
    const Status st = fixed_len(len, kParamLen);
    if (st != Status::Ok)
        return st;
    out.param_id = in[0];
    out.scope = in[1];
    out.value = get16(in + 2);
    return out.value > LIMIT_param_value_max ? Status::OutOfRange : Status::Ok;
}

// --- §3.3 status block: u8 state, u8 active_channel, u8 bypass, u8 fault_code, u16 uptime_s,
//     u8 event_pending ---

namespace {
constexpr size_t kStatusLen = 7;
inline Status check_status(const StatusBlock& s) {
    if (!in_codes(NODE_STATE_CODES, s.state))
        return Status::OutOfRange;
    if (s.bypass > 1)
        return Status::OutOfRange;
    return Status::Ok;
}
} // namespace

Status encode_status_block(const StatusBlock& s, uint8_t* out, size_t cap, size_t& written) {
    written = 0;
    const Status st = check_status(s);
    if (st != Status::Ok)
        return st;
    if (cap < kStatusLen)
        return Status::BufferTooSmall;
    out[0] = s.state;
    out[1] = s.active_channel;
    out[2] = s.bypass;
    out[3] = s.fault_code;
    put16(out + 4, s.uptime_s);
    out[6] = s.event_pending;
    written = kStatusLen;
    return Status::Ok;
}

Status decode_status_block(const uint8_t* in, size_t len, StatusBlock& out) {
    const Status st = fixed_len(len, kStatusLen);
    if (st != Status::Ok)
        return st;
    out.state = in[0];
    out.active_channel = in[1];
    out.bypass = in[2];
    out.fault_code = in[3];
    out.uptime_s = get16(in + 4);
    out.event_pending = in[6];
    return check_status(out);
}

// --- GET_EVENT response: u8 event_type, u8 remaining_count, u8[] detail (§3.4) ---

namespace {
constexpr size_t kEventHead = 2;
constexpr size_t kEventDetailMax = kMaxPayload - kEventHead;
} // namespace

Status encode_get_event_resp(const GetEventResp& r, uint8_t* out, size_t cap, size_t& written) {
    written = 0;
    if (!valid_event(r.event_type))
        return Status::OutOfRange;
    if (r.detail.len > kEventDetailMax)
        return Status::OutOfRange;
    const size_t total = kEventHead + r.detail.len;
    if (cap < total)
        return Status::BufferTooSmall;
    out[0] = r.event_type;
    out[1] = r.remaining_count;
    copy_tail(r.detail.data, r.detail.len, out + kEventHead);
    written = total;
    return Status::Ok;
}

Status decode_get_event_resp(const uint8_t* in, size_t len, GetEventResp& out) {
    if (len < kEventHead)
        return Status::Truncated;
    if (len - kEventHead > kEventDetailMax)
        return Status::OutOfRange;
    out.event_type = in[0];
    out.remaining_count = in[1];
    out.detail.data = in + kEventHead;
    out.detail.len = static_cast<uint8_t>(len - kEventHead);
    return valid_event(out.event_type) ? Status::Ok : Status::OutOfRange;
}

// --- opaque backplane payloads ---------------------------------------------------------------

Status encode_opaque(const OpaquePayload& r, uint8_t* out, size_t cap, size_t& written) {
    written = 0;
    if (r.bytes.len > kMaxPayload)
        return Status::OutOfRange;
    if (cap < r.bytes.len)
        return Status::BufferTooSmall;
    copy_tail(r.bytes.data, r.bytes.len, out);
    written = r.bytes.len;
    return Status::Ok;
}

Status decode_opaque(const uint8_t* in, size_t len, OpaquePayload& out) {
    if (len > kMaxPayload)
        return Status::OutOfRange;
    out.bytes.data = in;
    out.bytes.len = static_cast<uint8_t>(len);
    return Status::Ok;
}

// --- ERROR response: u8 code, u8[] detail ----------------------------------------------------

namespace {
constexpr size_t kErrorHead = 1;
constexpr size_t kErrorDetailMax = kMaxPayload - kErrorHead;
} // namespace

Status encode_error_resp(const ErrorResp& r, uint8_t* out, size_t cap, size_t& written) {
    written = 0;
    if (!in_codes(ERROR_CODES, r.code))
        return Status::OutOfRange;
    if (r.detail.len > kErrorDetailMax)
        return Status::OutOfRange;
    const size_t total = kErrorHead + r.detail.len;
    if (cap < total)
        return Status::BufferTooSmall;
    out[0] = r.code;
    copy_tail(r.detail.data, r.detail.len, out + kErrorHead);
    written = total;
    return Status::Ok;
}

Status decode_error_resp(const uint8_t* in, size_t len, ErrorResp& out) {
    if (len < kErrorHead)
        return Status::Truncated;
    if (len - kErrorHead > kErrorDetailMax)
        return Status::OutOfRange;
    out.code = in[0];
    out.detail.data = in + kErrorHead;
    out.detail.len = static_cast<uint8_t>(len - kErrorHead);
    return in_codes(ERROR_CODES, out.code) ? Status::Ok : Status::OutOfRange;
}

// --- dispatch ------------------------------------------------------------------------------

Status payload_bounds(uint8_t opcode, Dir dir, uint8_t& min_len, uint8_t& max_len, bool& opaque) {
    const PayloadInfo* info = nullptr;
    for (const auto& p : PAYLOAD_INFO)
        if (p.code == opcode)
            info = &p;
    if (info == nullptr)
        return Status::UnknownOpcode;
    if (dir == Dir::Request)
        for (const auto& o : OPCODE_INFO)
            if (o.code == opcode && o.target == Target::ResponseOnly)
                return Status::UnknownOpcode;
    opaque = info->opaque;
    if (dir == Dir::Request) {
        min_len = info->req_min;
        max_len = info->req_max;
    } else {
        min_len = info->resp_min;
        max_len = info->resp_max;
    }
    return Status::Ok;
}

} // namespace l3
} // namespace omgp
