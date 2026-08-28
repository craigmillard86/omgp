// Host-only helper for tools/diffcheck.py: one request per stdin line, one result per
// stdout line, flushed per line. Protocol: contracts/canonical-text.md.
//   ENC <canonical message>   -> hex | ERR <Status>
//   DEC <hex>                 -> canonical message | ERR <Status>
//   DENC/DDEC/DVAL            -> descriptor verbs (feature 001 US3)
//   CRC <hex>                 -> 0x%04X (crc16_ccitt_false, link/crc16.hpp)
//   QUIT                      -> exit 0
#include "../link/crc16.hpp"
#include "canonical.hpp"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

int main() {
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t sp = line.find(' ');
        const std::string verb = line.substr(0, sp);
        const std::string arg = sp == std::string::npos ? "" : line.substr(sp + 1);
        std::string out;
        std::vector<uint8_t> bytes;
        if (verb == "QUIT")
            return 0;
        if (verb == "ENC") {
            std::string err;
            out = omgp::canon::encode_message(arg, bytes, err)
                      ? omgp::canon::hex_lower(bytes.data(), bytes.size())
                      : err;
        } else if (verb == "DEC") {
            out = omgp::canon::parse_hex(arg, bytes)
                      ? omgp::canon::render_message(bytes.data(), bytes.size())
                      : "ERR BadRequest";
        } else if (verb == "DENC") {
            std::string err;
            out = omgp::canon::encode_descriptor(arg, bytes, err)
                      ? omgp::canon::hex_lower(bytes.data(), bytes.size())
                      : err;
        } else if (verb == "DDEC") {
            out = omgp::canon::parse_hex(arg, bytes)
                      ? omgp::canon::render_descriptor(bytes.data(), bytes.size())
                      : "ERR BadRequest";
        } else if (verb == "DVAL") {
            out = omgp::canon::parse_hex(arg, bytes)
                      ? omgp::canon::validate_line(bytes.data(), bytes.size())
                      : "ERR BadRequest";
        } else if (verb == "CRC") {
            if (omgp::canon::parse_hex(arg, bytes)) {
                char b[8];
                std::snprintf(b, sizeof b, "0x%04X",
                              omgp::crc16_ccitt_false(bytes.data(), bytes.size()));
                out = b;
            } else {
                out = "ERR BadRequest";
            }
        } else {
            out = "ERR BadRequest";
        }
        std::cout << out << '\n' << std::flush;
    }
    return 0;
}
