// L3 common header codec (spec 001 US2, T030). protocol-l3 §3.
#include "catch_amalgamated.hpp"
#include "heap_guard.hpp"
#include "l3/l3_header.hpp"
#include "omgp_protocol.h"

#include <cstring>

using namespace omgp::l3;

namespace {
const uint8_t kSetParam[] = {0x12, 0x10, 0x03, 0x00, 0x04, 0x01, 0xFF, 0xFF, 0x0F};
}

TEST_CASE("header encodes to five bytes in wire order and never allocates", "[header]") {
    Header h{omgp::OP_SET_PARAM, 0x10, 3, 0, 4};
    uint8_t buf[5];
    size_t n = 0;
    Status st;
    HEAP_FREE_SCOPE({ st = encode_header(h, buf, sizeof buf, n); });
    REQUIRE(st == Status::Ok);
    REQUIRE(n == 5);
    REQUIRE(std::memcmp(buf, kSetParam, 5) == 0);
}

TEST_CASE("BufferTooSmall writes nothing", "[header]") {
    Header h{omgp::OP_PING, 1, 0, 0, 0};
    uint8_t buf[5];
    std::memset(buf, 0xA5, sizeof buf);
    size_t n = 99;
    REQUIRE(encode_header(h, buf, 4, n) == Status::BufferTooSmall);
    for (uint8_t b : buf)
        REQUIRE(b == 0xA5);
    REQUIRE(n == 0);
}

TEST_CASE("reserved flag bits: refused on encode, preserved on decode", "[header]") {
    uint8_t buf[5];
    size_t n;
    REQUIRE(encode_header(Header{omgp::OP_PING, 1, 0, 0x04, 0}, buf, 5, n) ==
            Status::ReservedViolation);
    const uint8_t wire[] = {0x01, 0x01, 0x00, 0x04, 0x00};
    Header h;
    REQUIRE(decode_header(wire, 5, h) == Status::Ok);
    REQUIRE(h.flags == 0x04);
}

TEST_CASE("reserved node ids are refused for requests only", "[header]") {
    uint8_t buf[5];
    size_t n;
    REQUIRE(encode_header(Header{omgp::OP_PING, omgp::ADDR_reserved_min, 0, 0, 0}, buf, 5, n) ==
            Status::ReservedViolation);
    REQUIRE(encode_header(Header{omgp::OP_PING, omgp::ADDR_reserved_min, 0, omgp::FLAG_response, 0},
                          buf, 5, n) == Status::Ok);
    const uint8_t wire[] = {0x01, 0x80, 0x00, 0x00, 0x00};
    Header h;
    REQUIRE(decode_header(wire, 5, h) == Status::Ok);
    REQUIRE(h.node_id == 0x80);
}

TEST_CASE("payload_len is bounded by LIMIT_max_l3_payload", "[header]") {
    uint8_t buf[5];
    size_t n;
    REQUIRE(encode_header(Header{omgp::OP_BP_POWER, 2, 0, 0, 64}, buf, 5, n) == Status::Ok);
    REQUIRE(encode_header(Header{omgp::OP_BP_POWER, 2, 0, 0, 65}, buf, 5, n) == Status::OutOfRange);
    const uint8_t wire[] = {0x21, 0x02, 0x00, 0x00, 0x41};
    Header h;
    REQUIRE(decode_header(wire, 5, h) == Status::OutOfRange);
    REQUIRE(decode_header(wire, 4, h) == Status::Truncated);
}

TEST_CASE("decode_message delimits the payload exactly", "[header]") {
    Header h;
    Bytes p;
    HEAP_FREE_SCOPE({ REQUIRE(decode_message(kSetParam, sizeof kSetParam, h, p) == Status::Ok); });
    REQUIRE(h.payload_len == 4);
    REQUIRE(p.len == 4);
    REQUIRE(p.data == kSetParam + 5);
    REQUIRE(decode_message(kSetParam, 7, h, p) == Status::Truncated);
    uint8_t longer[10];
    std::memcpy(longer, kSetParam, 9);
    longer[9] = 0;
    REQUIRE(decode_message(longer, 10, h, p) == Status::LengthMismatch);
}
