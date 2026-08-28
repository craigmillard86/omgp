// Property tests (spec 001 T032): seeded random valid messages encode → decode → encode
// byte-identically; arbitrary bytes never crash and always yield a Status. Runs under
// ASan/UBSan via ctest. protocol-l3 §3.
#include "catch_amalgamated.hpp"
#include "heap_guard.hpp"
#include "l3/l3_header.hpp"
#include "l3/l3_payload.hpp"
#include "omgp_protocol.h"

#include <cstring>
#include <random>

using namespace omgp::l3;

namespace {
constexpr uint32_t kSeed = 0xB0071E; // same seed family as tools/diffcheck.py
constexpr int kValidIterations = 5000;
constexpr int kRandomIterations = 5000;

template <typename Encode, typename Decode> void round_trip(Encode encode, Decode decode) {
    uint8_t a[64], b[64];
    size_t na = 0, nb = 0;
    REQUIRE(encode(a, sizeof a, na) == Status::Ok);
    REQUIRE(decode(a, na) == Status::Ok);
    REQUIRE(encode(b, sizeof b, nb) == Status::Ok); // encode of the decoded value
    REQUIRE(na == nb);
    REQUIRE(std::memcmp(a, b, na) == 0);
}
} // namespace

TEST_CASE("random valid payloads are stable under encode-decode-encode", "[property]") {
    std::mt19937 rng(kSeed);
    auto u8 = [&](unsigned lo, unsigned hi) {
        return static_cast<uint8_t>(std::uniform_int_distribution<unsigned>(lo, hi)(rng));
    };
    auto u16 = [&](unsigned lo, unsigned hi) {
        return static_cast<uint16_t>(std::uniform_int_distribution<unsigned>(lo, hi)(rng));
    };
    uint8_t tail[64];
    for (int i = 0; i < kValidIterations; ++i) {
        for (auto& t : tail)
            t = u8(0, 255);
        switch (i % 8) {
        case 0: {
            SetParamReq r{u8(0, 255), u8(0, 255), u16(0, omgp::LIMIT_param_value_max)}, d;
            round_trip(
                [&](uint8_t* o, size_t c, size_t& n) { return encode_set_param(r, o, c, n); },
                [&](const uint8_t* p, size_t n) {
                    Status s = decode_set_param(p, n, d);
                    r = d;
                    return s;
                });
            break;
        }
        case 1: {
            GetParamResp r{u8(0, 255), u8(0, 255), u16(0, omgp::LIMIT_param_value_max)}, d;
            round_trip(
                [&](uint8_t* o, size_t c, size_t& n) { return encode_get_param_resp(r, o, c, n); },
                [&](const uint8_t* p, size_t n) {
                    Status s = decode_get_param_resp(p, n, d);
                    r = d;
                    return s;
                });
            break;
        }
        case 2: {
            StatusBlock r{u8(0, 4), u8(0, 255), u8(0, 1), u8(0, 255), u16(0, 65535), u8(0, 255)}, d;
            round_trip(
                [&](uint8_t* o, size_t c, size_t& n) { return encode_status_block(r, o, c, n); },
                [&](const uint8_t* p, size_t n) {
                    Status s = decode_status_block(p, n, d);
                    r = d;
                    return s;
                });
            break;
        }
        case 3: {
            IdentifyResp r{u8(0, 255), u8(0, 255), omgp::MODULE_TYPE_CODES[u8(0, 12)],
                           u16(0, omgp::LIMIT_max_descriptor_bytes), u16(0, 65535)},
                d;
            round_trip(
                [&](uint8_t* o, size_t c, size_t& n) { return encode_identify_resp(r, o, c, n); },
                [&](const uint8_t* p, size_t n) {
                    Status s = decode_identify_resp(p, n, d);
                    r = d;
                    return s;
                });
            break;
        }
        case 4: {
            uint8_t len = u8(0, 61);
            ReadDescResp r{u16(0, omgp::LIMIT_max_descriptor_bytes - 1), Bytes{tail, len}}, d;
            round_trip(
                [&](uint8_t* o, size_t c, size_t& n) { return encode_read_desc_resp(r, o, c, n); },
                [&](const uint8_t* p, size_t n) {
                    Status s = decode_read_desc_resp(p, n, d);
                    r = d;
                    return s;
                });
            break;
        }
        case 5: {
            uint8_t len = u8(0, 62);
            GetEventResp r{omgp::EVENT_CODES[u8(0, 4)], u8(0, 255), Bytes{tail, len}}, d;
            round_trip(
                [&](uint8_t* o, size_t c, size_t& n) { return encode_get_event_resp(r, o, c, n); },
                [&](const uint8_t* p, size_t n) {
                    Status s = decode_get_event_resp(p, n, d);
                    r = d;
                    return s;
                });
            break;
        }
        case 6: {
            uint8_t len = u8(0, 63);
            ErrorResp r{omgp::ERROR_CODES[u8(0, 5)], Bytes{tail, len}}, d;
            round_trip(
                [&](uint8_t* o, size_t c, size_t& n) { return encode_error_resp(r, o, c, n); },
                [&](const uint8_t* p, size_t n) {
                    Status s = decode_error_resp(p, n, d);
                    r = d;
                    return s;
                });
            break;
        }
        default: {
            uint8_t len = u8(0, 64);
            OpaquePayload r{Bytes{tail, len}}, d;
            round_trip([&](uint8_t* o, size_t c, size_t& n) { return encode_opaque(r, o, c, n); },
                       [&](const uint8_t* p, size_t n) {
                           Status s = decode_opaque(p, n, d);
                           r = d;
                           return s;
                       });
            break;
        }
        }
    }
}

TEST_CASE("arbitrary bytes never crash the decoders and never allocate", "[property]") {
    std::mt19937 rng(kSeed ^ 0x5A5A);
    std::uniform_int_distribution<unsigned> byte(0, 255), len(0, 80);
    uint8_t buf[80];
    unsigned ok = 0, rejected = 0;
    for (int i = 0; i < kRandomIterations; ++i) {
        const size_t n = len(rng);
        for (size_t k = 0; k < n; ++k)
            buf[k] = static_cast<uint8_t>(byte(rng));
        Header h;
        Bytes p;
        Status st;
        HEAP_FREE_SCOPE({ st = decode_message(buf, n, h, p); });
        if (st != Status::Ok) {
            ++rejected;
            continue;
        }
        ++ok;
        // Every typed decoder must accept or reject the payload without reading past it.
        SetParamReq a;
        IdentifyResp b;
        ReadDescResp c;
        StatusBlock d;
        GetEventResp e;
        ErrorResp f;
        OpaquePayload g;
        HEAP_FREE_SCOPE({
            (void)decode_set_param(p.data, p.len, a);
            (void)decode_identify_resp(p.data, p.len, b);
            (void)decode_read_desc_resp(p.data, p.len, c);
            (void)decode_status_block(p.data, p.len, d);
            (void)decode_get_event_resp(p.data, p.len, e);
            (void)decode_error_resp(p.data, p.len, f);
            (void)decode_opaque(p.data, p.len, g);
        });
    }
    REQUIRE(ok + rejected == kRandomIterations);
    REQUIRE(rejected > 0);
}
