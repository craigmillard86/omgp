// Trunk L2 frame codec (spec 002 US1, T014). trunk §4; contracts/link-cpp.md "Frame
// codec"; contracts/frame-vectors.md. Written from the C++ contract and the Python
// reference test names (tools/refimpl/test_link.py), not from link/frame.{hpp,cpp} —
// this file is expected to fail to compile until T022 lands link/frame.hpp.
#include "catch_amalgamated.hpp"
#include "heap_guard.hpp"
#include "link/crc16.hpp"
#include "link/frame.hpp"
#include "link/link_types.hpp"
#include "omgp_protocol.h"
#include "omgp_vectors.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace omgp::link;

namespace {

// Minimal parser for the canonical frame line (contracts/frame-vectors.md):
//   "frame dst=0x01 src=0x00 flags=0x00 seq=3 payload=0101000000"
// Kept local to this test file (never shared with tools/canonical.cpp, which does not
// gain frame support until T023) so the vector's fields are read from an independent
// text rendering, not derived from the codec under test.
struct ParsedFrame {
    uint8_t dst, src, flags, seq;
    std::vector<uint8_t> payload;
};

std::string field_after(const std::string& line, const char* key) {
    const size_t pos = line.find(key);
    REQUIRE(pos != std::string::npos);
    const size_t start = pos + std::strlen(key);
    size_t end = line.find(' ', start);
    if (end == std::string::npos)
        end = line.size();
    return line.substr(start, end - start);
}

uint8_t hex_u8(const std::string& s) {
    return static_cast<uint8_t>(std::strtoul(s.c_str(), nullptr, 16));
}

ParsedFrame parse_canonical_frame(const std::string& line) {
    ParsedFrame p{};
    p.dst = hex_u8(field_after(line, "dst=0x"));
    p.src = hex_u8(field_after(line, "src=0x"));
    p.flags = hex_u8(field_after(line, "flags=0x"));
    p.seq = static_cast<uint8_t>(std::strtoul(field_after(line, "seq=").c_str(), nullptr, 10));
    const std::string hex = field_after(line, "payload=");
    for (size_t i = 0; i + 2 <= hex.size(); i += 2)
        p.payload.push_back(hex_u8(hex.substr(i, 2)));
    return p;
}

// Independent byte-stuffing transform (trunk §4), used only to build hand-specified wire
// fixtures for tests that need to place a raw ctrl byte the real encoder can never
// produce (e.g. reserved bits set) — never used to verify encode_frame's own stuffing,
// which the golden vectors and tests/property/test_link_stuffing.cpp cover.
std::vector<uint8_t> stuff_bytes(const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out;
    for (uint8_t b : body) {
        if (b == omgp::TRUNK_flag_byte) {
            out.push_back(omgp::TRUNK_escape_byte);
            out.push_back(static_cast<uint8_t>(omgp::TRUNK_flag_byte ^ omgp::TRUNK_escape_xor));
        } else if (b == omgp::TRUNK_escape_byte) {
            out.push_back(omgp::TRUNK_escape_byte);
            out.push_back(static_cast<uint8_t>(omgp::TRUNK_escape_byte ^ omgp::TRUNK_escape_xor));
        } else {
            out.push_back(b);
        }
    }
    return out;
}

// dst=0x02 src=0x00 response=1 retry=0 seq=7 payload="hi"; crc over unstuffed dst..payload
// (hand-computed, tools/refimpl/test_link.py MARKER_FULL — shared fixture, independent of
// this codec).
const uint8_t kMarkerFull[] = {0x7e, 0x02, 0x00, 0x71, 0x02, 0x68, 0x69, 0xfd, 0xa0, 0x7e};
// dst=0x03 src=0x00 response=0 retry=1 seq=1 payload="Q"
const uint8_t kMarkerBFull[] = {0x7e, 0x03, 0x00, 0x12, 0x01, 0x51, 0x38, 0xab, 0x7e};

} // namespace

// --- golden vectors (contracts/frame-vectors.md) -----------------------------------

TEST_CASE("every frame_* vector encodes to its bytes and deframes to its fields", "[vectors]") {
    size_t seen = 0;
    for (size_t i = 0; i < omgp::vectors::COUNT; ++i) {
        const auto& v = omgp::vectors::ALL[i];
        if (std::string(v.kind) != "frame")
            continue;
        ++seen;
        INFO(v.name << " (" << v.spec_ref << ")");
        const ParsedFrame parsed = parse_canonical_frame(v.canonical);

        FrameFields f{parsed.dst,
                      parsed.src,
                      (parsed.flags & 0x01) != 0,
                      (parsed.flags & 0x02) != 0,
                      parsed.seq,
                      static_cast<uint8_t>(parsed.payload.size()),
                      parsed.payload.empty() ? nullptr : parsed.payload.data()};

        uint8_t out[kMaxWire];
        size_t written = 0;
        Status st;
        HEAP_FREE_SCOPE({ st = encode_frame(f, out, sizeof out, written); });
        REQUIRE(st == Status::Ok);
        REQUIRE(written == static_cast<size_t>(v.len));
        REQUIRE(std::memcmp(out, v.bytes, written) == 0);

        Deframer d;
        FrameView view{};
        int delivered = 0;
        HEAP_FREE_SCOPE({
            for (size_t k = 0; k < v.len; ++k)
                if (d.feed(v.bytes[k], view))
                    ++delivered;
        });
        REQUIRE(delivered == 1);
        REQUIRE(d.stats().delivered == 1);
        REQUIRE(view.f.dst == parsed.dst);
        REQUIRE(view.f.src == parsed.src);
        REQUIRE(view.f.response == ((parsed.flags & 0x01) != 0));
        REQUIRE(view.f.retry == ((parsed.flags & 0x02) != 0));
        REQUIRE(view.f.seq == parsed.seq);
        REQUIRE(view.f.len == parsed.payload.size());
        if (!parsed.payload.empty())
            REQUIRE(std::memcmp(view.f.payload, parsed.payload.data(), parsed.payload.size()) == 0);
    }
    REQUIRE(seen == 5); // frame_ping_req, frame_response, frame_retry, frame_max_payload,
                        // frame_worst_stuffing (contracts/frame-vectors.md)
}

TEST_CASE("frame_worst_stuffing's wire length is exactly 140 bytes", "[timing:max_payload]") {
    // SC-008 as corrected 2026-08-31 (docs/OPEN-QUESTIONS.md): every escapable byte in
    // this vector escapes, but an encoder-emitted ctrl/len byte can never itself need
    // escaping and CRC parity bars 141 — 140 is the achieved structural ceiling, `kMaxWire`
    // (142) only a sizing bound. A directed length assertion, not only the byte round-trip
    // covered above, so a drifted generator cannot round-trip green on a wrong length.
    bool found = false;
    for (size_t i = 0; i < omgp::vectors::COUNT; ++i) {
        const auto& v = omgp::vectors::ALL[i];
        if (std::string(v.name) != "frame_worst_stuffing")
            continue;
        found = true;
        REQUIRE(v.len == 140);
    }
    REQUIRE(found);
}

// --- encode_frame refusals (FR-005: nothing written) --------------------------------

TEST_CASE("encode_frame refuses payload longer than LIMIT_max_l3_payload, writes nothing",
          "[frame]") {
    uint8_t payload[omgp::LIMIT_max_l3_payload + 1] = {};
    FrameFields f{0x01, 0x00, false, false, 0, static_cast<uint8_t>(sizeof payload), payload};
    uint8_t out[kMaxWire];
    std::memset(out, 0xA5, sizeof out);
    size_t written = 99;
    REQUIRE(encode_frame(f, out, sizeof out, written) == Status::PayloadTooLong);
    REQUIRE(written == 0);
    for (uint8_t b : out)
        REQUIRE(b == 0xA5);
}

TEST_CASE("encode_frame refuses dst == 0xFF (reserved broadcast), writes nothing", "[frame]") {
    FrameFields f{0xFF, 0x00, false, false, 0, 0, nullptr};
    uint8_t out[kMaxWire];
    std::memset(out, 0xA5, sizeof out);
    size_t written = 99;
    REQUIRE(encode_frame(f, out, sizeof out, written) == Status::ReservedAddress);
    REQUIRE(written == 0);
    for (uint8_t b : out)
        REQUIRE(b == 0xA5);
}

// --- BufferTooSmall: the pre-check is against the worst-case BOUND, not the achieved
// length (contracts/link-cpp.md) -----------------------------------------------------

TEST_CASE("BufferTooSmall checks the worst-case bound, not the achieved stuffed length",
          "[frame]") {
    uint8_t payload[omgp::LIMIT_max_l3_payload];
    for (size_t i = 0; i < sizeof payload; ++i)
        payload[i] = static_cast<uint8_t>(i);
    FrameFields f{0x01, 0x00, false, false, 0, static_cast<uint8_t>(sizeof payload), payload};

    uint8_t exact[kMaxWire]; // kMaxWire == 2 + 2*(kHeaderLen + 64 + kCrcLen) == 142
    size_t written = 0;
    REQUIRE(encode_frame(f, exact, kMaxWire, written) == Status::Ok);
    REQUIRE(written > 0);

    uint8_t one_short[kMaxWire - 1];
    std::memset(one_short, 0xA5, sizeof one_short);
    written = 99;
    REQUIRE(encode_frame(f, one_short, kMaxWire - 1, written) == Status::BufferTooSmall);
    REQUIRE(written == 0);
    for (uint8_t b : one_short)
        REQUIRE(b == 0xA5);
}

// --- Deframer discard counters, one per Discard reason (tools/refimpl/test_link.py) --

TEST_CASE("Deframer counts BadCrc and resyncs on the next frame", "[frame]") {
    // kMarkerFull with its last CRC byte flipped (0xa0 -> 0xa1).
    const uint8_t corrupt[] = {0x7e, 0x02, 0x00, 0x71, 0x02, 0x68, 0x69, 0xfd, 0xa1, 0x7e};
    Deframer d;
    FrameView view{};
    int delivered = 0;
    for (uint8_t b : corrupt)
        if (d.feed(b, view))
            ++delivered;
    for (uint8_t b : kMarkerFull)
        if (d.feed(b, view))
            ++delivered;
    REQUIRE(delivered == 1);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadCrc)] == 1);
    REQUIRE(d.stats().delivered == 1);
}

TEST_CASE("Deframer counts BadLength when len is outside 0..64 and resyncs", "[frame]") {
    // dst=0x01 src=0x00 ctrl=0x00 len=0xC8 (200, outside range) then two arbitrary bytes.
    const uint8_t bad[] = {0x7e, 0x01, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x7e};
    Deframer d;
    FrameView view{};
    int delivered = 0;
    for (uint8_t b : bad)
        if (d.feed(b, view))
            ++delivered;
    for (uint8_t b : kMarkerFull)
        if (d.feed(b, view))
            ++delivered;
    REQUIRE(delivered == 1);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadLength)] == 1);
}

TEST_CASE("Deframer counts BadLength when declared len disagrees with the accumulated body",
          "[frame]") {
    // dst=0x01 src=0x00 ctrl=0x00 len=0x05 (declares 5) but only 4 more bytes precede FLAG.
    const uint8_t bad[] = {0x7e, 0x01, 0x00, 0x00, 0x05, 0xaa, 0xbb, 0xcc, 0xdd, 0x7e};
    Deframer d;
    FrameView view{};
    int delivered = 0;
    for (uint8_t b : bad)
        if (d.feed(b, view))
            ++delivered;
    for (uint8_t b : kMarkerFull)
        if (d.feed(b, view))
            ++delivered;
    REQUIRE(delivered == 1);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadLength)] == 1);
}

TEST_CASE("Deframer counts BadEscape on an invalid escape byte and resyncs", "[frame]") {
    // dst=4 payload=0x7e, with its escape's second byte corrupted: 7d 5e -> 7d 00.
    const uint8_t bad[] = {0x7e, 0x04, 0x00, 0x20, 0x01, 0x7d, 0x00, 0xa4, 0xb2, 0x7e};
    Deframer d;
    FrameView view{};
    int delivered = 0;
    for (uint8_t b : bad)
        if (d.feed(b, view))
            ++delivered;
    for (uint8_t b : kMarkerFull)
        if (d.feed(b, view))
            ++delivered;
    REQUIRE(delivered == 1);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadEscape)] == 1);
}

TEST_CASE("an escape byte as the last byte before FLAG is BadEscape and resyncs", "[frame]") {
    const uint8_t bad[] = {0x7e, 0x01, 0x00, 0x00, 0x00, 0x7d};
    Deframer d;
    FrameView view{};
    int delivered = 0;
    for (uint8_t b : bad)
        if (d.feed(b, view))
            ++delivered;
    for (uint8_t b : kMarkerFull)
        if (d.feed(b, view))
            ++delivered;
    REQUIRE(delivered == 1);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadEscape)] == 1);
}

TEST_CASE("a 71-byte unstuffed body is TooLong and resyncs", "[frame]") {
    STATIC_REQUIRE(omgp::LIMIT_max_l3_payload == 64);
    uint8_t bad[1 + 71 + 1];
    bad[0] = 0x7e;
    for (int i = 0; i < 71; ++i)
        bad[1 + i] = 0x01;
    bad[72] = 0x7e;
    Deframer d;
    FrameView view{};
    int delivered = 0;
    for (uint8_t b : bad)
        if (d.feed(b, view))
            ++delivered;
    for (uint8_t b : kMarkerFull)
        if (d.feed(b, view))
            ++delivered;
    REQUIRE(delivered == 1);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::TooLong)] == 1);
}

TEST_CASE("a TooLong abort survives an escaped 71st byte (PR #99 regression)", "[frame]") {
    // The 71st unstuffed byte arrives via a valid escape (7D 5E, unstuffing to FLAG). The
    // TooLong abort to Hunting must win over the Escaped->InFrame transition, or the rest
    // of the stream (including kMarkerFull's own opening FLAG-less remainder) would be
    // parsed as frame content with no FLAG ever opening a frame.
    uint8_t prefix[1 + 70 + 2];
    prefix[0] = 0x7e;
    for (int i = 0; i < 70; ++i)
        prefix[1 + i] = 0x01;
    prefix[71] = 0x7d;
    prefix[72] = 0x5e;
    Deframer d;
    FrameView view{};
    int delivered = 0;
    for (uint8_t b : prefix)
        if (d.feed(b, view))
            ++delivered;
    // kMarkerFull minus its opening FLAG: must NOT be delivered (no FLAG opened it).
    for (size_t i = 1; i < sizeof kMarkerFull; ++i)
        if (d.feed(kMarkerFull[i], view))
            ++delivered;
    REQUIRE(delivered == 0);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::TooLong)] == 1);
    REQUIRE(d.stats().delivered == 0);
    // The genuine next frame, opened by a real FLAG, is still delivered intact.
    for (uint8_t b : kMarkerFull)
        if (d.feed(b, view))
            ++delivered;
    REQUIRE(delivered == 1);
}

TEST_CASE("Deframer counts BadLength on a runt frame shorter than header+CRC", "[frame]") {
    // FLAG, 3 body bytes (fewer than kHeaderLen+kCrcLen == 6), FLAG: too short to ever be a
    // frame, and distinct from the two other BadLength tests above, which both accumulate
    // exactly 6 or more bytes and so exercise the *declared-length-mismatch* check, never
    // this shorter-than-any-header-plus-CRC check.
    const uint8_t bad[] = {0x7e, 0x01, 0x02, 0x03, 0x7e};
    Deframer d;
    FrameView view{};
    int delivered = 0;
    for (uint8_t b : bad)
        if (d.feed(b, view))
            ++delivered;
    REQUIRE(delivered == 0);
    REQUIRE(d.stats().delivered == 0);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadLength)] == 1);
    for (uint8_t b : kMarkerFull)
        if (d.feed(b, view))
            ++delivered;
    REQUIRE(delivered == 1);
}

TEST_CASE("Deframer::reset() returns to Hunting with an empty accumulator", "[frame]") {
    // Every other test that recovers from a corrupted stream does so by feeding a
    // subsequent frame WITH its own opening FLAG — but on_flag() always resynchronises
    // state_/len_ unconditionally (trunk §4 FR-003), so that pattern can never observe
    // whether reset() itself did the same. Probe directly instead: get into InFrame with a
    // nonzero accumulator, reset(), then feed one non-FLAG byte (dropped iff state_ ==
    // Hunting) followed by one lone FLAG (sees n == 0 iff len_ == 0). Anything left over
    // from before reset() turns that FLAG into a non-silent discard or delivery.
    Deframer d;
    FrameView view{};
    d.feed(0x7e, view);
    d.feed(0x01, view);
    d.feed(0x02, view);
    d.feed(0x03, view);

    d.reset();

    REQUIRE_FALSE(d.feed(0x09, view));
    REQUIRE_FALSE(d.feed(0x7e, view));

    REQUIRE(d.stats().delivered == 0);
    for (size_t i = 0; i < static_cast<size_t>(Discard::COUNT); ++i)
        REQUIRE(d.stats().discarded[i] == 0);
}

TEST_CASE("a TooLong abort clears the accumulator, not only the state", "[frame]") {
    // The existing TooLong tests recover via a subsequent FLAG-opened frame, which (as
    // above) resynchronises len_ regardless of whether append()'s own reset worked. Probe
    // with a lone FLAG immediately after the abort: on_flag() runs unconditionally on a
    // FLAG byte and uses len_ as-is, so a stale nonzero count is visible right there.
    uint8_t body[71];
    for (auto& b : body)
        b = 0x01;
    Deframer d;
    FrameView view{};
    d.feed(0x7e, view);
    for (uint8_t b : body)
        d.feed(b, view);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::TooLong)] == 1);

    REQUIRE_FALSE(d.feed(0x7e, view));
    REQUIRE(d.stats().delivered == 0);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::TooLong)] == 1);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadLength)] == 0);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadCrc)] == 0);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadEscape)] == 0);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::ReservedAddress)] == 0);
}

TEST_CASE("BadEscape clears state and accumulator, not only the counter", "[frame]") {
    // Same masking problem as the reset()/TooLong probes above: the existing BadEscape test
    // (line ~257) recovers via a FLAG-opened kMarkerFull, which cannot tell whether
    // on_escaped_byte's own state_/len_ reset actually ran. Probe directly instead.
    const uint8_t bad[] = {0x7e, 0x04, 0x00, 0x20, 0x01, 0x7d, 0x00};
    Deframer d;
    FrameView view{};
    for (uint8_t b : bad)
        d.feed(b, view);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadEscape)] == 1);

    REQUIRE_FALSE(d.feed(0x09, view));
    REQUIRE_FALSE(d.feed(0x7e, view));

    REQUIRE(d.stats().delivered == 0);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadEscape)] == 1);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadLength)] == 0);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadCrc)] == 0);
}

TEST_CASE("a successful unescape returns to InFrame, not stuck at Escaped/Hunting", "[frame]") {
    // dst=0x02 src=0x00 ctrl=0x00 len=2 payload={0x7E (escaped as 7D 5E), 0x05 (plain, fed
    // right after the escape completes)}; CRC over the unstuffed body. If the transition
    // back to InFrame after a successful unescape doesn't happen, the plain byte 0x05 is
    // either dropped (stuck Hunting) or misread as an escape continuation (stuck Escaped),
    // and the frame fails to decode.
    std::vector<uint8_t> body{0x02, 0x00, 0x00, 0x02, 0x7e, 0x05};
    const uint16_t c = omgp::crc16_ccitt_false(body.data(), body.size());
    const uint8_t wire[] = {0x7e,
                             0x02,
                             0x00,
                             0x00,
                             0x02,
                             0x7d,
                             0x5e,
                             0x05,
                             static_cast<uint8_t>(c & 0xFF),
                             static_cast<uint8_t>((c >> 8) & 0xFF),
                             0x7e};
    Deframer d;
    FrameView view{};
    int delivered = 0;
    for (uint8_t b : wire)
        if (d.feed(b, view))
            ++delivered;
    REQUIRE(delivered == 1);
    REQUIRE(d.stats().delivered == 1);
    REQUIRE(view.f.dst == 0x02);
    REQUIRE(view.f.len == 2);
    REQUIRE(view.f.payload[0] == 0x7e);
    REQUIRE(view.f.payload[1] == 0x05);
    for (size_t i = 0; i < static_cast<size_t>(Discard::COUNT); ++i)
        REQUIRE(d.stats().discarded[i] == 0);
}

TEST_CASE("a dangling escape before FLAG still lets that FLAG open the next frame", "[frame]") {
    // Same shape as "an escape byte as the last byte before FLAG is BadEscape and resyncs"
    // above, but the recovery frame is fed WITHOUT its own opening FLAG (trunk §4 FR-003:
    // the FLAG that discards the dangling escape also opens whatever comes next) — a
    // stronger check than feeding a fully self-opening frame afterward, which would
    // resynchronise regardless of whether this transition is correct.
    const uint8_t prefix[] = {0x7e, 0x01, 0x00, 0x00, 0x00, 0x7d, 0x7e};
    Deframer d;
    FrameView view{};
    int delivered = 0;
    for (uint8_t b : prefix)
        if (d.feed(b, view))
            ++delivered;
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::BadEscape)] == 1);

    for (size_t i = 1; i < sizeof(kMarkerFull); ++i)
        if (d.feed(kMarkerFull[i], view))
            ++delivered;
    REQUIRE(delivered == 1);
    REQUIRE(d.stats().delivered == 1);
    REQUIRE(view.f.dst == 0x02);
}

TEST_CASE("Deframer discards a frame addressed to the reserved broadcast dst 0xFF", "[frame]") {
    // trunk §5: dst 0xFF is reserved and MUST NOT be used in v1; encode_frame refuses to
    // originate it (Status::ReservedAddress). A wire frame that arrives addressed there
    // (bit corruption, or a future sender that ignores the rule) would otherwise be
    // delivered as a FrameFields that encode_frame can never re-encode, breaking the
    // codec's own round-trip invariant (caught live by fuzz_frame, docs/OPEN-QUESTIONS.md
    // 2026-08-31) — discard it like any other structurally invalid frame instead.
    // Hand-built body dst=0xFF src=0x00 ctrl=0x00 len=0x00 (no payload); CRC-16/CCITT-FALSE
    // over dst..len computed independently (0xCF63), not via the codec under test.
    const uint8_t bad[] = {0x7e, 0xff, 0x00, 0x00, 0x00, 0x63, 0xcf, 0x7e};
    Deframer d;
    FrameView view{};
    int delivered = 0;
    for (uint8_t b : bad)
        if (d.feed(b, view))
            ++delivered;
    for (uint8_t b : kMarkerFull)
        if (d.feed(b, view))
            ++delivered;
    REQUIRE(delivered == 1);
    REQUIRE(d.stats().discarded[static_cast<size_t>(Discard::ReservedAddress)] == 1);
    REQUIRE(d.stats().delivered == 1);
}

TEST_CASE("back-to-back frames sharing one FLAG are both delivered in order", "[frame]") {
    std::vector<uint8_t> stream(kMarkerFull, kMarkerFull + sizeof(kMarkerFull) - 1);
    stream.insert(stream.end(), kMarkerBFull, kMarkerBFull + sizeof kMarkerBFull);
    Deframer d;
    FrameView view{};
    std::vector<FrameFields> delivered;
    for (uint8_t b : stream)
        if (d.feed(b, view))
            delivered.push_back(view.f);
    REQUIRE(delivered.size() == 2);
    REQUIRE(delivered[0].dst == 0x02);
    REQUIRE(delivered[1].dst == 0x03);
    REQUIRE(d.stats().delivered == 2);
}

// --- reserved ctrl bits (bits 2-3): ignored on decode, never set on encode ----------

TEST_CASE("reserved ctrl bits are ignored on decode", "[frame]") {
    // ctrl = 0x0C: reserved bits 2-3 forced set, response=0, retry=0, seq=0 — a ctrl value
    // no real encoder ever produces. Built via the local `stuff_bytes` (not encode_frame,
    // which cannot express this) and the codec's own published-check-value-verified CRC
    // function, so this exercises only the Deframer's ctrl-bit handling (contracts/
    // link-cpp.md: "reserved ctrl bits are never set on encode; ignored on decode").
    std::vector<uint8_t> body{0x02, 0x00, 0x0C, 0x02, 0x68, 0x69};
    const uint16_t c = omgp::crc16_ccitt_false(body.data(), body.size());
    body.push_back(static_cast<uint8_t>(c & 0xFF));
    body.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
    std::vector<uint8_t> wire{omgp::TRUNK_flag_byte};
    const std::vector<uint8_t> stuffed = stuff_bytes(body);
    wire.insert(wire.end(), stuffed.begin(), stuffed.end());
    wire.push_back(omgp::TRUNK_flag_byte);

    Deframer d;
    FrameView view{};
    int delivered = 0;
    for (uint8_t b : wire)
        if (d.feed(b, view))
            ++delivered;
    REQUIRE(delivered == 1);
    REQUIRE(view.f.dst == 0x02);
    REQUIRE(view.f.response == false);
    REQUIRE(view.f.retry == false);
    REQUIRE(view.f.seq == 0);
}

TEST_CASE("encode_frame never sets reserved ctrl bits 2-3", "[frame]") {
    for (int response = 0; response < 2; ++response) {
        for (int retry = 0; retry < 2; ++retry) {
            for (uint8_t seq = 0; seq < 16; ++seq) {
                // dst/src chosen so neither needs stuffing: the unstuffed ctrl byte lands
                // at wire[3] unshifted.
                FrameFields f{0x01, 0x00, response != 0, retry != 0, seq, 0, nullptr};
                uint8_t out[kMaxWire];
                size_t written = 0;
                REQUIRE(encode_frame(f, out, sizeof out, written) == Status::Ok);
                REQUIRE((out[3] & 0x0C) == 0);
            }
        }
    }
}
