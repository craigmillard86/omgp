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
