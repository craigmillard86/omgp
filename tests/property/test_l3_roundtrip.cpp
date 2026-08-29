// Property tests (spec 001 T032): seeded random valid messages encode → decode → encode
// byte-identically; arbitrary bytes never crash and always yield a Status. Runs under
// ASan/UBSan via ctest. protocol-l3 §3.
#include "catch_amalgamated.hpp"
#include "heap_guard.hpp"
#include "l3/l3_descriptor.hpp"
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

// --- descriptors (spec 001 T044): write → validate → cursor → rewrite identical; random blobs ---

namespace {
struct DescGen {
    std::mt19937 rng;
    uint8_t pool[2048];
    explicit DescGen(uint32_t seed) : rng(seed) {
        for (auto& b : pool)
            b = static_cast<uint8_t>(std::uniform_int_distribution<unsigned>('a', 'z')(rng));
    }
    uint8_t u8(unsigned lo, unsigned hi) {
        return static_cast<uint8_t>(std::uniform_int_distribution<unsigned>(lo, hi)(rng));
    }
    uint16_t u16(unsigned lo, unsigned hi) {
        return static_cast<uint16_t>(std::uniform_int_distribution<unsigned>(lo, hi)(rng));
    }
    Str s(unsigned max_len) {
        return Str{pool + u8(0, 100), u8(0, max_len)};
    }
};

// Builds a random valid descriptor into buf; returns its size.
size_t random_descriptor(DescGen& g, uint8_t* buf, size_t cap) {
    DescriptorWriter w(buf, cap);
    REQUIRE(w.add_protocol(ProtocolRec{g.u8(0, 255), g.u8(0, 255)}) == Status::Ok);
    REQUIRE(w.add_module_type(ModuleTypeRec{omgp::MODULE_TYPE_CODES[g.u8(0, 12)]}) == Status::Ok);
    REQUIRE(w.add_name(g.s(24)) == Status::Ok);
    REQUIRE(w.add_manufacturer(g.s(24)) == Status::Ok);
    REQUIRE(w.add_model_id(ModelIdRec{g.u16(0, 65535), g.u16(0, 65535), g.u16(0, 65535)}) ==
            Status::Ok);
    if (g.u8(0, 1))
        REQUIRE(w.add_serial(g.s(16)) == Status::Ok);
    const int channels = g.u8(1, 6);
    for (int i = 0; i < channels; ++i)
        REQUIRE(w.add_channel(ChannelRec{static_cast<uint8_t>(i), g.s(20)}) == Status::Ok);
    REQUIRE(w.add_switching(SwitchingRec{g.u8(0, 3), g.u16(0, 65535)}) == Status::Ok);
    const int params = g.u8(1, 12);
    for (int i = 0; i < params; ++i) {
        REQUIRE(w.add_param(ParamRec{
                    static_cast<uint8_t>(i), g.u8(0, 255), omgp::PARAM_KIND_CODES[g.u8(0, 5)],
                    g.u16(0, omgp::LIMIT_param_value_max), g.s(20)}) == Status::Ok);
        if (g.u8(0, 3) == 0)
            REQUIRE(w.add_param_enum(ParamEnumRec{static_cast<uint8_t>(i), g.u8(0, 7), g.s(12)}) ==
                    Status::Ok);
    }
    REQUIRE(w.add_audio(AudioRec{g.u8(0, 255), g.u8(0, 1), g.u16(0, 65535), g.u16(0, 65535)}) ==
            Status::Ok);
    REQUIRE(w.add_power_lv(PowerLvRec{g.u16(0, 65535), g.u16(0, 65535), g.u16(0, 65535),
                                      g.u16(0, 65535)}) == Status::Ok);
    if (g.u8(0, 1))
        REQUIRE(w.add_power_tube(PowerTubeRec{g.u8(1, 4), g.u8(0, 255), g.u8(0, 255),
                                              g.u16(0, 65535), g.u16(0, 65535), g.u16(0, 65535),
                                              g.u8(0, 255), g.u8(0, 255)}) == Status::Ok);
    if (g.u8(0, 1)) {
        const uint8_t n = g.u8(0, 40);
        uint8_t raw[42] = {g.u8(0, 255), g.u8(0, 255)};
        for (int i = 0; i < n; ++i)
            raw[2 + i] = g.u8(0, 255);
        REQUIRE(w.add_raw(omgp::TLV_VENDOR, raw, static_cast<uint8_t>(2 + n)) == Status::Ok);
    }
    if (g.u8(0, 1)) {
        const uint8_t n = g.u8(0, 30);
        REQUIRE(w.add_raw(g.u8(0x50, 0x5F), g.pool, n) == Status::Ok);
    }
    DescriptorReport r;
    REQUIRE(w.finish(r) == Status::Ok);
    return w.size();
}
} // namespace

TEST_CASE("random valid descriptors survive write-validate-cursor-rewrite",
          "[property][descriptor]") {
    DescGen g(kSeed ^ 0xD35C);
    uint8_t a[2048], b[2048];
    for (int i = 0; i < 2000; ++i) {
        const size_t na = random_descriptor(g, a, sizeof a);
        DescriptorReport r;
        Status st;
        HEAP_FREE_SCOPE({ st = validate_descriptor(a, na, r); });
        REQUIRE(st == Status::Ok);
        // Re-emit every record verbatim through the raw path: identical bytes.
        DescriptorWriter w(b, sizeof b);
        RecordCursor c(a, na);
        RecordView v;
        while (!c.at_end() && c.next(v) == Status::Ok)
            REQUIRE(w.add_raw(v.type, v.value, v.len) == Status::Ok);
        REQUIRE(w.finish(r) == Status::Ok);
        REQUIRE(w.size() == na);
        REQUIRE(std::memcmp(a, b, na) == 0);
    }
}

TEST_CASE("random blobs never crash the descriptor validator or cursor", "[property][descriptor]") {
    std::mt19937 rng(kSeed ^ 0xB10B);
    std::uniform_int_distribution<unsigned> byte(0, 255), len(0, 2100);
    uint8_t buf[2100];
    unsigned rejected = 0;
    for (int i = 0; i < 2000; ++i) {
        const size_t n = len(rng);
        for (size_t k = 0; k < n; ++k)
            buf[k] = static_cast<uint8_t>(byte(rng));
        // Half the time make the first bytes a plausible record so the cursor goes deeper.
        if (n > 4 && (i & 1)) {
            buf[0] = omgp::TLV_PROTOCOL;
            buf[1] = 2;
        }
        DescriptorReport r;
        Status st;
        HEAP_FREE_SCOPE({ st = validate_descriptor(buf, n, r); });
        if (st != Status::Ok)
            ++rejected;
        RecordCursor c(buf, n);
        RecordView v;
        Status cs = Status::Ok;
        HEAP_FREE_SCOPE({
            while (!c.at_end() && cs == Status::Ok)
                cs = c.next(v);
        });
    }
    REQUIRE(rejected > 1900); // random bytes essentially never form a complete descriptor
}
