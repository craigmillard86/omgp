// libFuzzer target: every opcode payload decoder (protocol-l3 §3.1, §3.3).
// Input shape: byte 0 = opcode, byte 1 = direction (bit0), rest = payload. The opcode
// selects the typed decoder via payload_bounds; every decoder is also run on the raw
// payload regardless, so no decoder can be reached only through dispatch.
#include "l3/l3_payload.hpp"
#include "omgp_protocol.h"

#include <cstddef>
#include <cstdint>

using namespace omgp::l3;

namespace {
template <typename T, typename Dec, typename Enc>
void round(const uint8_t* p, size_t n, Dec dec, Enc enc) {
    T v;
    if (dec(p, n, v) != Status::Ok)
        return;
    uint8_t out[omgp::LIMIT_max_l3_payload];
    size_t written = 0;
    if (enc(v, out, sizeof out, written) != Status::Ok)
        __builtin_trap(); // a decoded value must re-encode
    if (written != n)
        __builtin_trap();
    for (size_t i = 0; i < n; ++i)
        if (out[i] != p[i])
            __builtin_trap(); // byte-identical round trip
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2)
        return 0;
    const uint8_t opcode = data[0];
    const Dir dir = (data[1] & 1) ? Dir::Response : Dir::Request;
    const uint8_t* p = data + 2;
    const size_t n = size - 2;
    uint8_t mn, mx;
    bool opaque;
    (void)payload_bounds(opcode, dir, mn, mx, opaque);

    round<IdentifyResp>(p, n, decode_identify_resp, encode_identify_resp);
    round<ReadDescReq>(p, n, decode_read_desc_req, encode_read_desc_req);
    round<ReadDescResp>(p, n, decode_read_desc_resp, encode_read_desc_resp);
    round<SelectChannelReq>(p, n, decode_select_channel, encode_select_channel);
    round<SetBypassReq>(p, n, decode_set_bypass, encode_set_bypass);
    round<SetParamReq>(p, n, decode_set_param, encode_set_param);
    round<GetParamReq>(p, n, decode_get_param_req, encode_get_param_req);
    round<GetParamResp>(p, n, decode_get_param_resp, encode_get_param_resp);
    round<StatusBlock>(p, n, decode_status_block, encode_status_block);
    round<GetEventResp>(p, n, decode_get_event_resp, encode_get_event_resp);
    round<OpaquePayload>(p, n, decode_opaque, encode_opaque);
    round<ErrorResp>(p, n, decode_error_resp, encode_error_resp);
    return 0;
}
