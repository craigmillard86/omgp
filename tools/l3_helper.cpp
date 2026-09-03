// Host-only helper for tools/diffcheck.py: one request per stdin line, one result per
// stdout line (FSTREAM: several, ending in one END line), flushed per line. Protocol:
// contracts/canonical-text.md, contracts/frame-vectors.md.
//   ENC <canonical message>   -> hex | ERR <Status>
//   DEC <hex>                 -> canonical message | ERR <Status>
//   DENC/DDEC/DVAL            -> descriptor verbs (feature 001 US3)
//   FENC <canonical frame>    -> OK <hex wire bytes> | ERR <Status> | ERR BadRequest
//                                                                        (spec 002 T023)
//   FDEC <hex wire bytes>     -> OK <canonical frame> | ERR <Discard> | ERR BadRequest
//                                                                        (spec 002 T023)
//   FSTREAM <hex stream>      -> (OK <canonical frame>)* then END <discards>; malformed
//                                hex is ERR BadRequest then END 0          (spec 002 T023)
//   CRC <hex>                 -> 0x%04X (crc16_ccitt_false, link/crc16.hpp)
//   QUIT                      -> exit 0
// The verb table itself lives in l3_helper_dispatch.{hpp,cpp} so it is unit testable
// (tests/unit/test_l3_helper_dispatch.cpp) without spawning this binary; this file is only
// the stdin/stdout loop around it.
#include "l3_helper_dispatch.hpp"

#include <iostream>
#include <string>

int main() {
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.substr(0, line.find(' ')) == "QUIT")
            return 0;
        std::cout << omgp::canon::dispatch_line(line) << '\n' << std::flush;
    }
    return 0;
}
