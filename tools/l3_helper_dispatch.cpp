// See l3_helper_dispatch.hpp. Protocol: contracts/canonical-text.md, contracts/frame-vectors.md.
//   ENC <canonical message>   -> hex | ERR <Status>
//   DEC <hex>                 -> canonical message | ERR <Status>
//   DENC/DDEC/DVAL            -> descriptor verbs (feature 001 US3)
//   FENC <canonical frame>    -> OK <hex wire bytes> | ERR <Status>       (spec 002 T023)
//   FDEC <hex wire bytes>     -> OK <canonical frame> | ERR <Discard>     (spec 002 T023)
//   FSTREAM <hex stream>      -> canonical frame* then END <discards>     (spec 002 T023)
//   CRC <hex>                 -> 0x%04X (crc16_ccitt_false, link/crc16.hpp)
#include "l3_helper_dispatch.hpp"

#include "../link/crc16.hpp"
#include "canonical.hpp"

#include <cstdio>
#include <vector>

namespace omgp {
namespace canon {

std::string dispatch_line(const std::string& line) {
    const size_t sp = line.find(' ');
    const std::string verb = line.substr(0, sp);
    const std::string arg = sp == std::string::npos ? "" : line.substr(sp + 1);
    std::vector<uint8_t> bytes;
    if (verb == "ENC") {
        std::string err;
        return encode_message(arg, bytes, err) ? hex_lower(bytes.data(), bytes.size()) : err;
    }
    if (verb == "DEC") {
        return parse_hex(arg, bytes) ? render_message(bytes.data(), bytes.size())
                                      : "ERR BadRequest";
    }
    if (verb == "DENC") {
        std::string err;
        return encode_descriptor(arg, bytes, err) ? hex_lower(bytes.data(), bytes.size()) : err;
    }
    if (verb == "DDEC") {
        return parse_hex(arg, bytes) ? render_descriptor(bytes.data(), bytes.size())
                                      : "ERR BadRequest";
    }
    if (verb == "DVAL") {
        return parse_hex(arg, bytes) ? validate_line(bytes.data(), bytes.size()) : "ERR BadRequest";
    }
    if (verb == "FENC") {
        return fenc_response(arg);
    }
    if (verb == "FDEC") {
        return parse_hex(arg, bytes) ? fdec_line(bytes.data(), bytes.size()) : "ERR BadRequest";
    }
    if (verb == "FSTREAM") {
        return fstream_response(arg);
    }
    if (verb == "CRC") {
        if (!parse_hex(arg, bytes))
            return "ERR BadRequest";
        char b[8];
        std::snprintf(b, sizeof b, "0x%04X", crc16_ccitt_false(bytes.data(), bytes.size()));
        return b;
    }
    return "ERR BadRequest";
}

} // namespace canon
} // namespace omgp
