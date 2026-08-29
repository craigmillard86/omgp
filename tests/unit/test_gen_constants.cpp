// Generated-constants contract (spec 001 US1, T021): the attribute tables the codecs
// drive dispatch and validation from. protocol-l3 §3.1 (opcodes), §4.1 (record types).
#include "catch_amalgamated.hpp"
#include "omgp_protocol.h"

#include <cstddef>
#include <string>

namespace {
template <typename T, size_t N> constexpr size_t count_of(const T (&)[N]) {
    return N;
}
} // namespace

TEST_CASE("OPCODE_INFO is sorted by code and knows ERROR is response-only", "[gen]") {
    for (size_t i = 1; i < count_of(omgp::OPCODE_INFO); ++i)
        REQUIRE(omgp::OPCODE_INFO[i - 1].code < omgp::OPCODE_INFO[i].code);
    bool found = false;
    for (const auto& e : omgp::OPCODE_INFO) {
        if (e.code == omgp::OP_ERROR) {
            found = true;
            REQUIRE(e.target == omgp::Target::ResponseOnly);
        }
        if (e.code == omgp::OP_SET_PARAM) {
            REQUIRE(e.target == omgp::Target::Module);
            REQUIRE(e.idempotent);
        }
    }
    REQUIRE(found);
}

TEST_CASE("TLV_INFO carries required/repeated/max_len from the YAML", "[gen]") {
    for (size_t i = 1; i < count_of(omgp::TLV_INFO); ++i)
        REQUIRE(omgp::TLV_INFO[i - 1].type < omgp::TLV_INFO[i].type);
    for (const auto& t : omgp::TLV_INFO) {
        if (t.type == omgp::TLV_NAME) {
            REQUIRE(t.required);
            REQUIRE_FALSE(t.repeated);
            REQUIRE(t.max_len == 24);
        }
        if (t.type == omgp::TLV_CHANNEL) {
            REQUIRE(t.required);
            REQUIRE(t.repeated);
            REQUIRE(t.max_len == 0);
        }
        if (t.type == omgp::TLV_SERIAL) {
            REQUIRE_FALSE(t.required);
            REQUIRE(t.max_len == 16);
        }
    }
}

TEST_CASE("PAYLOAD_INFO bounds follow the l3_payloads layouts", "[gen]") {
    for (size_t i = 1; i < count_of(omgp::PAYLOAD_INFO); ++i)
        REQUIRE(omgp::PAYLOAD_INFO[i - 1].code < omgp::PAYLOAD_INFO[i].code);
    for (const auto& p : omgp::PAYLOAD_INFO) {
        if (p.code == omgp::OP_SET_PARAM) {
            REQUIRE(p.req_min == 4);
            REQUIRE(p.req_max == 4);
            REQUIRE(p.resp_min == 0);
            REQUIRE(p.resp_max == 0);
            REQUIRE_FALSE(p.opaque);
        }
        if (p.code == omgp::OP_BP_POWER) {
            REQUIRE(p.opaque);
            REQUIRE(p.req_max == omgp::LIMIT_max_l3_payload);
        }
        if (p.code == omgp::OP_GET_EVENT) {
            REQUIRE(p.resp_min == 2);
            REQUIRE(p.resp_max == omgp::LIMIT_max_l3_payload);
        }
    }
}

TEST_CASE("code tables are sorted and complete", "[gen]") {
    STATIC_REQUIRE(count_of(omgp::OPCODE_CODES) == count_of(omgp::OPCODE_INFO));
    STATIC_REQUIRE(count_of(omgp::TLV_CODES) == count_of(omgp::TLV_INFO));
    STATIC_REQUIRE(count_of(omgp::ERROR_CODES) == 6);
    STATIC_REQUIRE(count_of(omgp::EVENT_CODES) ==
                   7); // NONE..PARAM_CHANGED_LOCALLY + USER_DEFINED_MIN/MAX
    STATIC_REQUIRE(count_of(omgp::MODULE_TYPE_CODES) == 13);
    STATIC_REQUIRE(count_of(omgp::NODE_STATE_CODES) == 5);
    STATIC_REQUIRE(count_of(omgp::PARAM_KIND_CODES) == 6);
    for (size_t i = 1; i < count_of(omgp::ERROR_CODES); ++i)
        REQUIRE(omgp::ERROR_CODES[i - 1] < omgp::ERROR_CODES[i]);
    REQUIRE(omgp::ERROR_CODES[0] == omgp::ERR_UNKNOWN_OPCODE);
    REQUIRE(omgp::EVENT_CODES[0] == omgp::EVT_NONE);
}

TEST_CASE("descriptor constants alias the single YAML limit", "[gen]") {
    STATIC_REQUIRE(omgp::DESC_MAX_BYTES == omgp::LIMIT_max_descriptor_bytes);
    REQUIRE(std::string(omgp::DESC_CRC) == "crc16_ccitt_false");
    REQUIRE(std::string(omgp::TRUNK_crc) == "crc16_ccitt_false");
    STATIC_REQUIRE(omgp::RESERVED_FIRMWARE_UPDATE_MIN < omgp::RESERVED_FIRMWARE_UPDATE_MAX);
    STATIC_REQUIRE(omgp::EVT_NONE == 0);
}
