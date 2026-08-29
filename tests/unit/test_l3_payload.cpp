// L3 payload codecs (spec 001 US2, T031). protocol-l3 §3.1, §3.3; rulings in
// docs/OPEN-QUESTIONS.md. [vectors] consumes the golden vectors through the generated
// header and the host-only canonical-text codec; [rules] mirrors tools/refimpl/test_l3.py.
#include "canonical.hpp"
#include "catch_amalgamated.hpp"
#include "heap_guard.hpp"
#include "l3/l3_payload.hpp"
#include "omgp_protocol.h"
#include "omgp_vectors.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace omgp::l3;

namespace {
std::vector<uint8_t> hx(const char* s) {
    std::vector<uint8_t> out;
    unsigned v;
    while (*s) {
        if (*s == ' ') {
            ++s;
            continue;
        }
        REQUIRE(std::sscanf(s, "%2x", &v) == 1);
        out.push_back(static_cast<uint8_t>(v));
        s += 2;
    }
    return out;
}
template <typename T, size_t N> constexpr size_t count_of(const T (&)[N]) {
    return N;
}
} // namespace

TEST_CASE("every message and status vector round-trips through the C++ codec", "[vectors]") {
    size_t seen = 0;
    for (size_t i = 0; i < omgp::vectors::COUNT; ++i) {
        const auto& v = omgp::vectors::ALL[i];
        const std::string kind = v.kind;
        if (kind != "message" && kind != "status")
            continue;
        ++seen;
        INFO(v.name << " (" << v.spec_ref << ")");
        std::string rendered = kind == "message" ? omgp::canon::render_message(v.bytes, v.len)
                                                 : omgp::canon::render_status(v.bytes, v.len);
        REQUIRE(rendered == v.canonical);
        std::vector<uint8_t> encoded;
        std::string err;
        bool ok = kind == "message" ? omgp::canon::encode_message(v.canonical, encoded, err)
                                    : omgp::canon::encode_status(v.canonical, encoded, err);
        INFO(err);
        REQUIRE(ok);
        REQUIRE(encoded == std::vector<uint8_t>(v.bytes, v.bytes + v.len));
    }
    REQUIRE(seen >= 1); // vectors are committed before this test is expected green (T035)
}

TEST_CASE("SET_PARAM carries absolute values and refuses out-of-range", "[rules]") {
    uint8_t buf[8];
    size_t n;
    SetParamReq r{1, 0xFF, 4095};
    Status st;
    HEAP_FREE_SCOPE({ st = encode_set_param(r, buf, sizeof buf, n); });
    REQUIRE(st == Status::Ok);
    REQUIRE(hx("01 FF FF 0F") == std::vector<uint8_t>(buf, buf + n));
    REQUIRE(encode_set_param(SetParamReq{1, 0xFF, 4096}, buf, sizeof buf, n) == Status::OutOfRange);
    SetParamReq d;
    auto w = hx("01 FF 00 10");
    REQUIRE(decode_set_param(w.data(), w.size(), d) == Status::OutOfRange);
    w = hx("01 FF FF");
    REQUIRE(decode_set_param(w.data(), w.size(), d) == Status::Truncated);
    w = hx("01 FF FF 0F 00");
    REQUIRE(decode_set_param(w.data(), w.size(), d) == Status::LengthMismatch);
    w = hx("01 FF FF 0F");
    REQUIRE(decode_set_param(w.data(), w.size(), d) == Status::Ok);
    REQUIRE((d.param_id == 1 && d.scope == 0xFF && d.value == 4095));
}

TEST_CASE("IDENTIFY response layout and module-type range", "[rules]") {
    uint8_t buf[8];
    size_t n;
    IdentifyResp r{1, 0, omgp::MT_TUBE_PREAMP, 612, 0x4A3F};
    REQUIRE(encode_identify_resp(r, buf, sizeof buf, n) == Status::Ok);
    REQUIRE(hx("01 00 03 64 02 3F 4A") == std::vector<uint8_t>(buf, buf + n));
    IdentifyResp d;
    auto w = hx("01 00 63 64 02 3F 4A");
    REQUIRE(decode_identify_resp(w.data(), w.size(), d) == Status::OutOfRange);
    r.desc_len = 2049;
    REQUIRE(encode_identify_resp(r, buf, sizeof buf, n) == Status::OutOfRange);
}

TEST_CASE("READ_DESC request/response including the 61-byte tail bound", "[rules]") {
    uint8_t buf[64];
    size_t n;
    REQUIRE(encode_read_desc_req(ReadDescReq{1987, 61}, buf, sizeof buf, n) == Status::Ok);
    REQUIRE(hx("C3 07 3D") == std::vector<uint8_t>(buf, buf + n));
    REQUIRE(encode_read_desc_req(ReadDescReq{2048, 1}, buf, sizeof buf, n) == Status::OutOfRange);
    uint8_t tail[62];
    for (size_t i = 0; i < sizeof tail; ++i)
        tail[i] = static_cast<uint8_t>(i);
    REQUIRE(encode_read_desc_resp(ReadDescResp{1987, Bytes{tail, 61}}, buf, sizeof buf, n) ==
            Status::Ok);
    REQUIRE(n == 64);
    REQUIRE(encode_read_desc_resp(ReadDescResp{0, Bytes{tail, 62}}, buf, sizeof buf, n) ==
            Status::OutOfRange);
    ReadDescResp d;
    auto w = hx("00 00 05 01 02");
    REQUIRE(decode_read_desc_resp(w.data(), w.size(), d) == Status::Truncated);
    w = hx("00 00 01 01 02");
    REQUIRE(decode_read_desc_resp(w.data(), w.size(), d) == Status::LengthMismatch);
}

TEST_CASE("status block, events, errors, bypass", "[rules]") {
    uint8_t buf[64];
    size_t n;
    REQUIRE(encode_status_block(StatusBlock{omgp::STATE_READY, 2, 0, 0, 77, 1}, buf, sizeof buf,
                                n) == Status::Ok);
    REQUIRE(hx("01 02 00 00 4D 00 01") == std::vector<uint8_t>(buf, buf + n));
    StatusBlock sb;
    auto w = hx("05 02 00 00 4D 00 01");
    REQUIRE(decode_status_block(w.data(), w.size(), sb) == Status::OutOfRange);
    w = hx("01 02 02 00 4D 00 01");
    REQUIRE(decode_status_block(w.data(), w.size(), sb) == Status::OutOfRange);

    GetEventResp ev;
    w = hx("00 00");
    REQUIRE(decode_get_event_resp(w.data(), w.size(), ev) == Status::Ok);
    REQUIRE((ev.event_type == omgp::EVT_NONE && ev.remaining_count == 0 && ev.detail.len == 0));
    w = hx("F0 00 DE AD BE EF");
    REQUIRE(decode_get_event_resp(w.data(), w.size(), ev) == Status::Ok);
    REQUIRE(ev.detail.len == 4);
    w = hx("63 00");
    REQUIRE(decode_get_event_resp(w.data(), w.size(), ev) == Status::OutOfRange);
    w = hx("00");
    REQUIRE(decode_get_event_resp(w.data(), w.size(), ev) == Status::Truncated);
    uint8_t big[63] = {0};
    REQUIRE(encode_get_event_resp(GetEventResp{omgp::EVT_NONE, 0, Bytes{big, 63}}, buf, sizeof buf,
                                  n) == Status::OutOfRange);

    ErrorResp er;
    w = hx("02 04 05");
    REQUIRE(decode_error_resp(w.data(), w.size(), er) == Status::Ok);
    REQUIRE((er.code == omgp::ERR_BAD_PAYLOAD && er.detail.len == 2));
    w = hx("63");
    REQUIRE(decode_error_resp(w.data(), w.size(), er) == Status::OutOfRange);
    REQUIRE(decode_error_resp(w.data(), 0, er) == Status::Truncated);

    SetBypassReq bp;
    w = hx("02");
    REQUIRE(decode_set_bypass(w.data(), w.size(), bp) == Status::OutOfRange);
    REQUIRE(encode_set_bypass(SetBypassReq{2}, buf, sizeof buf, n) == Status::OutOfRange);
}

TEST_CASE("opaque backplane payloads pass through verbatim", "[rules]") {
    uint8_t buf[64];
    size_t n;
    auto w = hx("01 FF 00");
    REQUIRE(encode_opaque(OpaquePayload{Bytes{w.data(), 3}}, buf, sizeof buf, n) == Status::Ok);
    REQUIRE(std::vector<uint8_t>(buf, buf + n) == w);
    OpaquePayload d;
    REQUIRE(decode_opaque(w.data(), 0, d) == Status::Ok);
    REQUIRE(d.bytes.len == 0);
    uint8_t big[65] = {0};
    REQUIRE(encode_opaque(OpaquePayload{Bytes{big, 65}}, buf, sizeof buf, n) == Status::OutOfRange);
    uint8_t out[3];
    REQUIRE(encode_opaque(OpaquePayload{Bytes{big, 4}}, out, sizeof out, n) ==
            Status::BufferTooSmall);
}

// --- boundary tests from the 2026-08-29 mutation triage (docs/OPEN-QUESTIONS.md) ----------------

TEST_CASE("every encoder: exact capacity succeeds; one byte less fails with written == 0",
          "[rules]") {
    // Kills the `cap < N` → `cap <= N` and `written = 0` → constant survivors in every encoder
    // (l3_payload.cpp:64-381), plus the `== limit` value edges carried by the inputs
    // (desc_len 2048, value 4095, bypass 1, detail 62/63, opaque 64).
    uint8_t d[64];
    for (size_t i = 0; i < sizeof d; ++i)
        d[i] = static_cast<uint8_t>(i);
    using Enc = std::function<Status(uint8_t*, size_t, size_t&)>;
    struct Case {
        const char* name;
        size_t size;
        Enc enc;
    };
    const IdentifyResp id{1, 0, omgp::MT_TUBE_PREAMP, omgp::LIMIT_max_descriptor_bytes, 0};
    const ReadDescReq rq{omgp::LIMIT_max_descriptor_bytes - 1, 61};
    const ReadDescResp rr{omgp::LIMIT_max_descriptor_bytes - 1, Bytes{d, 61}};
    const SelectChannelReq sc{3};
    const SetBypassReq sb{1};
    const SetParamReq sp{1, 0xFF, omgp::LIMIT_param_value_max};
    const GetParamReq gq{1, 0xFF};
    const GetParamResp gr{1, 0xFF, omgp::LIMIT_param_value_max};
    const StatusBlock st{omgp::STATE_READY, 0, 1, 0, 0, 0};
    const GetEventResp ev{omgp::EVT_NONE, 0, Bytes{d, 62}};
    const OpaquePayload op{Bytes{d, 64}};
    const ErrorResp er{omgp::ERR_BAD_PAYLOAD, Bytes{d, 63}};
    const Case cases[] = {
        {"identify_resp", 7,
         [&](uint8_t* o, size_t c, size_t& n) { return encode_identify_resp(id, o, c, n); }},
        {"read_desc_req", 3,
         [&](uint8_t* o, size_t c, size_t& n) { return encode_read_desc_req(rq, o, c, n); }},
        {"read_desc_resp", 64,
         [&](uint8_t* o, size_t c, size_t& n) { return encode_read_desc_resp(rr, o, c, n); }},
        {"select_channel", 1,
         [&](uint8_t* o, size_t c, size_t& n) { return encode_select_channel(sc, o, c, n); }},
        {"set_bypass", 1,
         [&](uint8_t* o, size_t c, size_t& n) { return encode_set_bypass(sb, o, c, n); }},
        {"set_param", 4,
         [&](uint8_t* o, size_t c, size_t& n) { return encode_set_param(sp, o, c, n); }},
        {"get_param_req", 2,
         [&](uint8_t* o, size_t c, size_t& n) { return encode_get_param_req(gq, o, c, n); }},
        {"get_param_resp", 4,
         [&](uint8_t* o, size_t c, size_t& n) { return encode_get_param_resp(gr, o, c, n); }},
        {"status_block", 7,
         [&](uint8_t* o, size_t c, size_t& n) { return encode_status_block(st, o, c, n); }},
        {"get_event_resp", 64,
         [&](uint8_t* o, size_t c, size_t& n) { return encode_get_event_resp(ev, o, c, n); }},
        {"opaque", 64, [&](uint8_t* o, size_t c, size_t& n) { return encode_opaque(op, o, c, n); }},
        {"error_resp", 64,
         [&](uint8_t* o, size_t c, size_t& n) { return encode_error_resp(er, o, c, n); }},
    };
    for (const auto& k : cases) {
        INFO(k.name);
        uint8_t buf[64];
        size_t n = 99;
        REQUIRE(k.enc(buf, k.size, n) == Status::Ok);
        REQUIRE(n == k.size);
        n = 99;
        REQUIRE(k.enc(buf, k.size - 1, n) == Status::BufferTooSmall);
        REQUIRE(n == 0);
    }
}

TEST_CASE("decoders at the exact limits", "[rules]") {
    uint8_t d[65];
    for (size_t i = 0; i < sizeof d; ++i)
        d[i] = static_cast<uint8_t>(i);
    // l3_payload.cpp:117/124/148: offset 2048 is out of range, 2047 is the last valid one.
    ReadDescReq rq;
    auto w = hx("00 08 01");
    REQUIRE(decode_read_desc_req(w.data(), w.size(), rq) == Status::OutOfRange);
    w = hx("FF 07 01");
    REQUIRE(decode_read_desc_req(w.data(), w.size(), rq) == Status::Ok);
    REQUIRE(rq.offset == 2047);
    ReadDescResp rr;
    w = hx("00 08 00");
    REQUIRE(decode_read_desc_resp(w.data(), w.size(), rr) == Status::OutOfRange);
    w = hx("00 00 00"); // l3_payload.cpp:139: a three-byte response carries zero bytes
    REQUIRE(decode_read_desc_resp(w.data(), w.size(), rr) == Status::Ok);
    REQUIRE(rr.bytes.len == 0);
    uint8_t buf[64];
    size_t n;
    REQUIRE(encode_read_desc_resp(ReadDescResp{2048, Bytes{d, 0}}, buf, sizeof buf, n) ==
            Status::OutOfRange);
    // l3_payload.cpp:264: GET_PARAM response value 4095 is valid, 4096 is not.
    GetParamResp gp;
    w = hx("01 FF FF 0F");
    REQUIRE(decode_get_param_resp(w.data(), w.size(), gp) == Status::Ok);
    REQUIRE(gp.value == 4095);
    w = hx("01 FF 00 10");
    REQUIRE(decode_get_param_resp(w.data(), w.size(), gp) == Status::OutOfRange);
    // l3_payload.cpp:275: bypass 1 is valid in a status block.
    StatusBlock sb;
    w = hx("01 02 01 00 4D 00 01");
    REQUIRE(decode_status_block(w.data(), w.size(), sb) == Status::Ok);
    REQUIRE(sb.bypass == 1);
    // l3_payload.cpp:337 and :39: a 64-byte event payload (62 detail bytes) with a
    // user-defined type inside the range; 65 bytes is out of range.
    GetEventResp ev;
    d[0] = 0xF5;
    REQUIRE(decode_get_event_resp(d, 64, ev) == Status::Ok);
    REQUIRE((ev.event_type == 0xF5 && ev.detail.len == 62));
    REQUIRE(decode_get_event_resp(d, 65, ev) == Status::OutOfRange);
    // l3_payload.cpp:360: opaque payloads of exactly 64 bytes.
    OpaquePayload op;
    REQUIRE(decode_opaque(d, 64, op) == Status::Ok);
    REQUIRE(op.bytes.len == 64);
    REQUIRE(decode_opaque(d, 65, op) == Status::OutOfRange);
    // l3_payload.cpp:392: a 64-byte error payload (63 detail bytes); 65 is out of range.
    ErrorResp er;
    d[0] = omgp::ERR_BAD_PAYLOAD;
    REQUIRE(decode_error_resp(d, 64, er) == Status::Ok);
    REQUIRE(er.detail.len == 63);
    REQUIRE(decode_error_resp(d, 65, er) == Status::OutOfRange);
}

TEST_CASE("payload_bounds dispatch covers every opcode and rejects the rest", "[rules]") {
    uint8_t mn, mx;
    bool opaque;
    for (const auto& o : omgp::OPCODE_INFO) {
        const bool response_only = o.target == omgp::Target::ResponseOnly;
        REQUIRE(payload_bounds(o.code, Dir::Request, mn, mx, opaque) ==
                (response_only ? Status::UnknownOpcode : Status::Ok));
        REQUIRE(payload_bounds(o.code, Dir::Response, mn, mx, opaque) == Status::Ok);
    }
    REQUIRE(payload_bounds(omgp::OP_SET_PARAM, Dir::Request, mn, mx, opaque) == Status::Ok);
    REQUIRE((mn == 4 && mx == 4 && !opaque));
    REQUIRE(payload_bounds(omgp::OP_GET_EVENT, Dir::Response, mn, mx, opaque) == Status::Ok);
    REQUIRE((mn == 2 && mx == omgp::LIMIT_max_l3_payload));
    REQUIRE(payload_bounds(omgp::OP_BP_POWER, Dir::Request, mn, mx, opaque) == Status::Ok);
    REQUIRE(opaque);
    REQUIRE(payload_bounds(0x63, Dir::Request, mn, mx, opaque) == Status::UnknownOpcode);
    REQUIRE(payload_bounds(omgp::RESERVED_FIRMWARE_UPDATE_MIN, Dir::Request, mn, mx, opaque) ==
            Status::UnknownOpcode);
    REQUIRE(payload_bounds(0x00, Dir::Request, mn, mx, opaque) == Status::UnknownOpcode);
}
