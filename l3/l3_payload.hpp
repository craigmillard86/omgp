// OMGP L3 codec — opcode payloads. protocol-l3 §3.1 (opcodes), §3.3 (status block),
// §3.4 (events); layouts ruled 2026-08-28 (docs/OPEN-QUESTIONS.md) and encoded in the
// YAML l3_payloads section. Empty payloads (PING, IDENTIFY request, GET_STATUS request,
// GET_EVENT request, and the accepted responses) need no function: payload_len == 0.
//
// Every encoder: BufferTooSmall with no partial write; OutOfRange for values outside the
// protocol (never clamps). Every decoder: Truncated if shorter than the layout,
// LengthMismatch if a fixed layout has bytes left over, OutOfRange per field rules. No
// decoder reads past `len`.
#pragma once

#include "l3_types.hpp"

namespace omgp {
namespace l3 {

Status encode_identify_resp(const IdentifyResp&, uint8_t* out, size_t cap, size_t& written);
Status decode_identify_resp(const uint8_t* in, size_t len, IdentifyResp& out);

Status encode_read_desc_req(const ReadDescReq&, uint8_t* out, size_t cap, size_t& written);
Status decode_read_desc_req(const uint8_t* in, size_t len, ReadDescReq& out);
Status encode_read_desc_resp(const ReadDescResp&, uint8_t* out, size_t cap, size_t& written);
Status decode_read_desc_resp(const uint8_t* in, size_t len, ReadDescResp& out);

Status encode_select_channel(const SelectChannelReq&, uint8_t* out, size_t cap, size_t& written);
Status decode_select_channel(const uint8_t* in, size_t len, SelectChannelReq& out);

Status encode_set_bypass(const SetBypassReq&, uint8_t* out, size_t cap, size_t& written);
Status decode_set_bypass(const uint8_t* in, size_t len, SetBypassReq& out);

// SET operations carry absolute values (§3; constitution Principle V).
Status encode_set_param(const SetParamReq&, uint8_t* out, size_t cap, size_t& written);
Status decode_set_param(const uint8_t* in, size_t len, SetParamReq& out);

Status encode_get_param_req(const GetParamReq&, uint8_t* out, size_t cap, size_t& written);
Status decode_get_param_req(const uint8_t* in, size_t len, GetParamReq& out);
Status encode_get_param_resp(const GetParamResp&, uint8_t* out, size_t cap, size_t& written);
Status decode_get_param_resp(const uint8_t* in, size_t len, GetParamResp& out);

Status encode_status_block(const StatusBlock&, uint8_t* out, size_t cap, size_t& written);
Status decode_status_block(const uint8_t* in, size_t len, StatusBlock& out);

Status encode_get_event_resp(const GetEventResp&, uint8_t* out, size_t cap, size_t& written);
Status decode_get_event_resp(const uint8_t* in, size_t len, GetEventResp& out);

// BP_SLOT_MAP / BP_POWER / BP_ROUTE: format not yet defined; bytes pass through verbatim.
Status encode_opaque(const OpaquePayload&, uint8_t* out, size_t cap, size_t& written);
Status decode_opaque(const uint8_t* in, size_t len, OpaquePayload& out);

Status encode_error_resp(const ErrorResp&, uint8_t* out, size_t cap, size_t& written);
Status decode_error_resp(const uint8_t* in, size_t len, ErrorResp& out);

// Payload size bounds for an opcode/direction from the generated PAYLOAD_INFO table.
// UnknownOpcode for anything not in the table (including the reserved 0x60-0x6F range)
// and for a request to a response-only opcode (ERROR).
Status payload_bounds(uint8_t opcode, Dir dir, uint8_t& min_len, uint8_t& max_len, bool& opaque);

} // namespace l3
} // namespace omgp
