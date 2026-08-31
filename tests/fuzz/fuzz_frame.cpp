// libFuzzer target: trunk L2 frame deframing (trunk §4). Any input must never crash,
// hang or over-read (constitution Principle III; CLAUDE.md rule 7); any delivered frame
// must re-encode and re-parse to equal fields (round-trip stability). Built only under
// the `fuzz` preset (spec 002 US1, T017).
#include "link/frame.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace omgp::link;

namespace {

struct Delivered {
    FrameFields f;
    std::vector<uint8_t> payload;
};

// Feeds `data[0..size)` one byte at a time into a single fresh Deframer and collects
// every delivered frame, copying the payload out (it points into the Deframer's own
// accumulator and is only valid until the next feed()).
std::vector<Delivered> feed_all(const uint8_t* data, size_t size, size_t chunk) {
    Deframer d;
    FrameView v{};
    std::vector<Delivered> out;
    for (size_t start = 0; start < size; start += chunk) {
        const size_t end = start + chunk < size ? start + chunk : size;
        // feed() is defined per byte; grouping the same call sequence into chunks of
        // varying size must never change what is delivered (no bulk-feed path exists —
        // this is the only way a caller can present the stream, data-model.md §3).
        for (size_t i = start; i < end; ++i)
            if (d.feed(data[i], v)) {
                Delivered del;
                del.f = v.f;
                del.payload.assign(v.f.payload, v.f.payload + v.f.len);
                out.push_back(std::move(del));
            }
    }
    return out;
}

bool same_fields(const FrameFields& a, const std::vector<uint8_t>& ap, const FrameFields& b,
                 const std::vector<uint8_t>& bp) {
    return a.dst == b.dst && a.src == b.src && a.response == b.response && a.retry == b.retry &&
           a.seq == b.seq && a.len == b.len && ap == bp;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Pass 1: byte-at-a-time (chunk size 1, the literal per-byte path).
    const std::vector<Delivered> pass1 = feed_all(data, size, 1);
    // Pass 2: the identical byte sequence, grouped into chunks of a fuzzer-derived size —
    // must deliver byte-identical frames, since feed() has no notion of chunk boundaries.
    const size_t chunk = 2 + (size % 13);
    const std::vector<Delivered> pass2 = feed_all(data, size, chunk);

    if (pass1.size() != pass2.size())
        __builtin_trap();
    for (size_t i = 0; i < pass1.size(); ++i)
        if (!same_fields(pass1[i].f, pass1[i].payload, pass2[i].f, pass2[i].payload))
            __builtin_trap();

    // Any delivered frame re-encodes and re-parses to equal fields.
    for (const auto& del : pass1) {
        FrameFields f = del.f;
        f.payload = del.payload.empty() ? nullptr : del.payload.data();
        uint8_t out[kMaxWire];
        size_t written = 0;
        if (encode_frame(f, out, sizeof out, written) != Status::Ok)
            __builtin_trap(); // a delivered frame's fields must always be re-encodable

        Deframer redo;
        FrameView v2{};
        int redelivered = 0;
        Delivered got{};
        for (size_t i = 0; i < written; ++i)
            if (redo.feed(out[i], v2)) {
                ++redelivered;
                got.f = v2.f;
                got.payload.assign(v2.f.payload, v2.f.payload + v2.f.len);
            }
        if (redelivered != 1)
            __builtin_trap();
        if (!same_fields(got.f, got.payload, f, del.payload))
            __builtin_trap();
    }
    return 0;
}
