// Trunk L2 byte-stuffing and CRC property tests (spec 002 US1, T015). trunk §4;
// contracts/link-cpp.md. Runs under ASan/UBSan via ctest. Written from the C++ contract,
// not from link/frame.{hpp,cpp} — this file is expected to fail to compile until T022
// lands link/frame.hpp.
#include "catch_amalgamated.hpp"
#include "link/crc16.hpp"
#include "link/frame.hpp"
#include "link/link_types.hpp"
#include "omgp_protocol.h"

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

using namespace omgp::link;

namespace {
constexpr uint32_t kSeed = 0xB0071E; // same seed family as tests/property/test_l3_roundtrip.cpp
constexpr uint8_t kFlag = omgp::TRUNK_flag_byte;
constexpr uint8_t kEsc = omgp::TRUNK_escape_byte;

uint8_t make_ctrl(bool response, bool retry, uint8_t seq) {
    const uint8_t flags = static_cast<uint8_t>((response ? 0x01 : 0x00) | (retry ? 0x02 : 0x00));
    const uint8_t seq_bits = static_cast<uint8_t>((seq & 0x0F) << 4);
    return static_cast<uint8_t>(flags | seq_bits);
}
} // namespace

// --- stuff -> unstuff identity, exercised through the real codec (encode_frame ->
// Deframer), including 7E/7D-dense payloads (FR-001) --------------------------------

TEST_CASE("random frames, including 7E/7D-dense payloads, round-trip through "
          "encode_frame -> Deframer",
          "[property]") {
    std::mt19937 rng(kSeed);
    auto rand_byte = [&]() { return static_cast<uint8_t>(rng() & 0xFF); };

    for (int i = 0; i < 3000; ++i) {
        const uint8_t dst = static_cast<uint8_t>(rng() % 0xFF); // excludes 0xFF (reserved)
        const uint8_t src = rand_byte();
        const bool response = (rng() & 1) != 0;
        const bool retry = (rng() & 1) != 0;
        const uint8_t seq = static_cast<uint8_t>(rng() % 16);
        const uint8_t len = static_cast<uint8_t>(rng() % (omgp::LIMIT_max_l3_payload + 1));
        std::vector<uint8_t> payload(len);
        for (auto& b : payload)
            // Every third frame: payload dense in exactly the two bytes that force
            // stuffing (0x7E/0x7D), the worst case for the stuff/unstuff transform;
            // otherwise fully random (still likely to hit 7E/7D by chance at len up to 64).
            b = (i % 3 == 0) ? (rand_byte() & 1 ? kFlag : kEsc) : rand_byte();

        const uint8_t* p = payload.empty() ? nullptr : payload.data();
        const FrameFields f{dst, src, response, retry, seq, len, p};
        uint8_t out[kMaxWire];
        size_t written = 0;
        REQUIRE(encode_frame(f, out, sizeof out, written) == Status::Ok);

        Deframer d;
        FrameView view{};
        int delivered = 0;
        FrameFields got{};
        std::vector<uint8_t> got_payload;
        for (size_t k = 0; k < written; ++k) {
            if (d.feed(out[k], view)) {
                ++delivered;
                got = view.f;
                got_payload.assign(view.f.payload, view.f.payload + view.f.len);
            }
        }
        REQUIRE(delivered == 1);
        REQUIRE(d.stats().delivered == 1);
        for (uint32_t disc : d.stats().discarded)
            REQUIRE(disc == 0);
        REQUIRE(got.dst == dst);
        REQUIRE(got.src == src);
        REQUIRE(got.response == response);
        REQUIRE(got.retry == retry);
        REQUIRE(got.seq == seq);
        REQUIRE(got.len == len);
        REQUIRE(got_payload == payload);
    }
}

// --- published CCITT-FALSE check value, through encode_frame's own CRC field --------

TEST_CASE("encode_frame's CRC bytes equal crc16_ccitt_false(dst..payload); published "
          "CCITT-FALSE check value holds",
          "[property]") {
    // Published CCITT-FALSE check value (poly 0x1021, init 0xFFFF): crc("123456789")
    // == 0x29B1. Pins the constants link/crc16.hpp and this codec's CRC field depend on.
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    REQUIRE(omgp::crc16_ccitt_false(check, sizeof check) == 0x29B1);

    std::mt19937 rng(kSeed ^ 0xC1C0);
    // dst/src/payload bytes deliberately exclude FLAG/ESC so the encoded body is
    // byte-identical to the unstuffed body (isolates the CRC field from the stuffing
    // transform, which the round-trip test above already covers); ctrl/len can never
    // collide with FLAG/ESC by construction (ctrl in {0x00-0x03, 0x10-0x13, ...,
    // 0xF0-0xF3}; len in 0x00-0x40).
    auto safe_byte = [&]() {
        uint8_t b;
        do {
            b = static_cast<uint8_t>(rng() & 0xFF);
        } while (b == omgp::TRUNK_flag_byte || b == omgp::TRUNK_escape_byte || b == 0xFF);
        return b;
    };

    int checked = 0;
    for (int i = 0; i < 500; ++i) {
        const uint8_t dst = safe_byte();
        const uint8_t src = safe_byte();
        const bool response = (rng() & 1) != 0;
        const bool retry = (rng() & 1) != 0;
        const uint8_t seq = static_cast<uint8_t>(rng() % 16);
        const uint8_t len = static_cast<uint8_t>(rng() % (omgp::LIMIT_max_l3_payload + 1));
        std::vector<uint8_t> payload(len);
        for (auto& b : payload)
            b = safe_byte();

        const uint8_t ctrl = make_ctrl(response, retry, seq);
        std::vector<uint8_t> body{dst, src, ctrl, len};
        body.insert(body.end(), payload.begin(), payload.end());
        const uint16_t expected_crc = omgp::crc16_ccitt_false(body.data(), body.size());
        const uint8_t crc_lo = static_cast<uint8_t>(expected_crc & 0xFF);
        const uint8_t crc_hi = static_cast<uint8_t>((expected_crc >> 8) & 0xFF);
        if (crc_lo == kFlag || crc_lo == kEsc || crc_hi == kFlag || crc_hi == kEsc)
            continue; // CRC itself would need stuffing; skip (round-trip test above
                      // already covers stuffed CRC bytes end-to-end)
        ++checked;

        const uint8_t* p = payload.empty() ? nullptr : payload.data();
        const FrameFields f{dst, src, response, retry, seq, len, p};
        uint8_t out[kMaxWire];
        size_t written = 0;
        REQUIRE(encode_frame(f, out, sizeof out, written) == Status::Ok);

        std::vector<uint8_t> expected_unescaped = body;
        expected_unescaped.push_back(crc_lo);
        expected_unescaped.push_back(crc_hi);
        REQUIRE(written == expected_unescaped.size() + 2); // + both FLAGs
        REQUIRE(out[0] == kFlag);
        REQUIRE(out[written - 1] == kFlag);
        REQUIRE(std::memcmp(out + 1, expected_unescaped.data(), expected_unescaped.size()) == 0);
    }
    REQUIRE(checked > 400); // FLAG/ESC-valued CRCs are rare (2/65536 per byte); most iterate
}

// --- frame time = wire length x byte_time_us, at both bit rates ---------------------

TEST_CASE("frame time equals wire length times byte_time_us, at both bit rates",
          "[timing:bit_rate][timing:bit_rate_fallback]") {
    // frame_max_payload (contracts/frame-vectors.md): 72 wire bytes, CRC escapes neither
    // byte — a directed, reproducible wire length to multiply against the timing symbols.
    uint8_t payload[omgp::LIMIT_max_l3_payload];
    for (size_t i = 0; i < sizeof payload; ++i)
        payload[i] = static_cast<uint8_t>(i);
    const FrameFields f{0x01, 0x00, false, false, 0, static_cast<uint8_t>(sizeof payload), payload};
    uint8_t out[kMaxWire];
    size_t written = 0;
    REQUIRE(encode_frame(f, out, sizeof out, written) == Status::Ok);
    REQUIRE(written == 72);

    // 72 * 10us at the reference rate; 72 * 86us at the fallback rate.
    REQUIRE(written * byte_time_us(omgp::TRUNK_bit_rate) == 720);
    REQUIRE(written * byte_time_us(omgp::TRUNK_bit_rate_fallback) == 6192);
}
