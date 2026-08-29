// libFuzzer target: L3 common header + message delimiting (protocol-l3 §3).
// Any input must decode or be rejected with a Status; never crash, hang or over-read
// (constitution Principle III; spec 001 FR-026). Built only under the `fuzz` preset.
#include "l3/l3_header.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    omgp::l3::Header h;
    omgp::l3::Bytes p;
    if (omgp::l3::decode_message(data, size, h, p) == omgp::l3::Status::Ok) {
        // Re-encode the header and require the bytes we decoded (decode is total on the
        // header; reserved flag bits survive, so encode may legitimately refuse them).
        uint8_t out[omgp::l3::HEADER_LEN];
        size_t n = 0;
        const omgp::l3::Status st = omgp::l3::encode_header(h, out, sizeof out, n);
        if (st == omgp::l3::Status::Ok)
            for (size_t i = 0; i < n; ++i)
                if (out[i] != data[i])
                    __builtin_trap();
    }
    (void)omgp::l3::decode_header(data, size, h);
    return 0;
}
