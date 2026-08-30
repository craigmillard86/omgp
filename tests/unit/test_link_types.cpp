// trunk §4/§5/§6/§9: pins the shared types and derived constants every later link/
// component (frame codec, Master, Responder, HealthTracker) builds on.
// Contract: specs/002-trunk-link-layer/contracts/link-cpp.md "Types"
#include "catch_amalgamated.hpp"
#include "link/link_types.hpp"
#include "omgp_protocol.h"

#include <string>

TEST_CASE("kMaxWire is 142, derived from LIMIT_max_l3_payload", "[timing:max_payload]") {
    STATIC_REQUIRE(omgp::link::kHeaderLen == 4);
    STATIC_REQUIRE(omgp::link::kCrcLen == 2);
    STATIC_REQUIRE(omgp::link::kMaxUnstuffed ==
                   omgp::link::kHeaderLen + omgp::LIMIT_max_l3_payload + omgp::link::kCrcLen);
    STATIC_REQUIRE(omgp::link::kMaxWire == 2 + 2 * omgp::link::kMaxUnstuffed);
    STATIC_REQUIRE(omgp::link::kMaxWire == 142);
}

TEST_CASE("kAddrCount is 16, derived from ADDR_host..ADDR_backplane_max", "[link]") {
    STATIC_REQUIRE(omgp::link::kAddrCount == static_cast<size_t>(omgp::ADDR_backplane_max) -
                                                 static_cast<size_t>(omgp::ADDR_host) + 1);
    STATIC_REQUIRE(omgp::link::kAddrCount == 16);
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

TEST_CASE("Discard has exactly the four documented reasons, in data-model order", "[link]") {
    using omgp::link::Discard;
    STATIC_REQUIRE(static_cast<size_t>(Discard::COUNT) == 4);
    STATIC_REQUIRE(static_cast<int>(Discard::BadCrc) == 0);
    STATIC_REQUIRE(static_cast<int>(Discard::BadLength) == 1);
    STATIC_REQUIRE(static_cast<int>(Discard::BadEscape) == 2);
    STATIC_REQUIRE(static_cast<int>(Discard::TooLong) == 3);
}

TEST_CASE("DeframerStats.discarded is sized to Discard::COUNT and zero-initializes", "[link]") {
    using omgp::link::Discard;
    omgp::link::DeframerStats stats{};
    STATIC_REQUIRE(sizeof(stats.discarded) / sizeof(stats.discarded[0]) ==
                   static_cast<size_t>(Discard::COUNT));
    REQUIRE(stats.delivered == 0);
    for (auto d : stats.discarded)
        REQUIRE(d == 0);
}

TEST_CASE("Notice has the seven documented notification kinds, in data-model order", "[link]") {
    using omgp::link::Notice;
    STATIC_REQUIRE(static_cast<int>(Notice::ENROLLED) == 0);
    STATIC_REQUIRE(static_cast<int>(Notice::SUSPECT) == 1);
    STATIC_REQUIRE(static_cast<int>(Notice::OFFLINE) == 2);
    STATIC_REQUIRE(static_cast<int>(Notice::RECOVERED) == 3);
    STATIC_REQUIRE(static_cast<int>(Notice::BUS_FAULT) == 4);
    STATIC_REQUIRE(static_cast<int>(Notice::ALERT) == 5);
    STATIC_REQUIRE(static_cast<int>(Notice::BUS_RECOVERED) == 6);
}

TEST_CASE("FrameFields/FrameView expose the trunk §4 fields with the documented types", "[link]") {
    const uint8_t payload[1] = {0x42};
    omgp::link::FrameFields f{/*dst=*/0x01,    /*src=*/0x00, /*response=*/false,
                              /*retry=*/false, /*seq=*/0x03, /*len=*/1,          payload};
    omgp::link::FrameView view{f};
    REQUIRE(view.f.dst == 0x01);
    REQUIRE(view.f.src == 0x00);
    REQUIRE(view.f.response == false);
    REQUIRE(view.f.retry == false);
    REQUIRE(view.f.seq == 0x03);
    REQUIRE(view.f.len == 1);
    REQUIRE(view.f.payload == payload);
}

TEST_CASE("AddrStats and BusStats have the FR-011a fields, aggregate-initialized to zero",
          "[link]") {
    omgp::link::AddrStats a{};
    omgp::link::BusStats b{};
    REQUIRE(a.transactions == 0);
    REQUIRE(a.retries == 0);
    REQUIRE(a.timeouts == 0);
    REQUIRE(a.crc_failures == 0);
    REQUIRE(a.discards == 0);
    REQUIRE(a.replays_served == 0);
    REQUIRE(a.late_responses == 0);
    REQUIRE(b.rate_changes == 0);
    REQUIRE(b.bus_faults == 0);
}
