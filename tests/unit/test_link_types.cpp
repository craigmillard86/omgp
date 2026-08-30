// trunk §4/§5/§6/§9: pins the shared types and derived constants every later link/
// component (frame codec, Master, Responder, HealthTracker) builds on.
// Contract: specs/002-trunk-link-layer/contracts/link-cpp.md "Types"
#include "catch_amalgamated.hpp"
#include "link/link_types.hpp"
#include "omgp_protocol.h"

#include <string>

TEST_CASE("kMaxWire is 142, derived from LIMIT_max_l3_payload", "[timing:max_payload]") {
    STATIC_REQUIRE(omgp::link::kMaxWire == 2 + 2 * (4 + omgp::LIMIT_max_l3_payload + 2));
    STATIC_REQUIRE(omgp::link::kMaxWire == 142);
}

TEST_CASE("byte_time_us at the reference bit rate is 10us", "[timing:bit_rate]") {
    REQUIRE(omgp::link::byte_time_us(omgp::TRUNK_bit_rate) == 10);
}

TEST_CASE("byte_time_us at the fallback bit rate is 86us", "[timing:bit_rate_fallback]") {
    REQUIRE(omgp::link::byte_time_us(omgp::TRUNK_bit_rate_fallback) == 86);
}

TEST_CASE("every link::Status has a distinct, non-\"?\" name", "[link]") {
    using omgp::link::Status;
    Status all[6];
    all[0] = Status::Ok;
    all[1] = Status::PayloadTooLong;
    all[2] = Status::ReservedAddress;
    all[3] = Status::BufferTooSmall;
    all[4] = Status::Busy;
    all[5] = Status::NotIdle;
    for (Status s : all) {
        std::string name = omgp::link::status_name(s);
        REQUIRE(name != "?");
        for (Status other : all) {
            if (other == s)
                continue;
            REQUIRE(name != std::string(omgp::link::status_name(other)));
        }
    }
}

TEST_CASE("HealthState uses the trunk document's own four words", "[link]") {
    using omgp::link::HealthState;
    STATIC_REQUIRE(static_cast<int>(HealthState::UNENROLLED) == 0);
    STATIC_REQUIRE(static_cast<int>(HealthState::ENROLLED) == 1);
    STATIC_REQUIRE(static_cast<int>(HealthState::SUSPECT) == 2);
    STATIC_REQUIRE(static_cast<int>(HealthState::OFFLINE) == 3);
}
