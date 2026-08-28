// libFuzzer target: whole-message round trip through the host-only canonical codec.
// decode → render → parse → encode must reproduce the input bytes exactly whenever the
// input decodes (spec 001 US2 scenario 5) — except for the documented asymmetry: the
// decoder preserves reserved flag bits and accepts any node id (FR-012, forward
// compatibility) while the encoder refuses reserved flag bits and reserved node ids in
// requests (FR-010). For those inputs the only acceptable outcome is exactly
// "ERR ReservedViolation"; anything else traps. Found by this target on its first run:
// bytes 10 18 07 04 01 00 (flags 0x04) rendered fine and then failed to re-encode.
#include "canonical.hpp"
#include "l3/l3_header.hpp"
#include "omgp_protocol.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string rendered = omgp::canon::render_message(data, size);
    if (rendered.rfind("ERR ", 0) == 0)
        return 0;

    omgp::l3::Header h;
    omgp::l3::Bytes p;
    if (omgp::l3::decode_message(data, size, h, p) != omgp::l3::Status::Ok)
        __builtin_trap(); // rendered, so it must decode
    const uint8_t reserved_bits = static_cast<uint8_t>(~(omgp::FLAG_response | omgp::FLAG_error));
    const bool response = (h.flags & omgp::FLAG_response) != 0;
    const bool encoder_must_refuse =
        (h.flags & reserved_bits) != 0 || (!response && h.node_id >= omgp::ADDR_reserved_min);

    std::vector<uint8_t> again;
    std::string err;
    const bool ok = omgp::canon::encode_message(rendered, again, err);
    if (encoder_must_refuse) {
        if (ok || err != "ERR ReservedViolation")
            __builtin_trap(); // the asymmetry is exact: refuse, and with that Status only
        return 0;
    }
    if (!ok)
        __builtin_trap(); // rendered text must parse and encode
    if (again.size() != size)
        __builtin_trap();
    for (size_t i = 0; i < size; ++i)
        if (again[i] != data[i])
            __builtin_trap();
    if (omgp::canon::render_message(again.data(), again.size()) != rendered)
        __builtin_trap();
    return 0;
}
