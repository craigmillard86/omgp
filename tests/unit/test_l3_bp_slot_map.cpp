// BP_SLOT_MAP response codec (spec 003 T005-T007, R-01). protocol-l3 §3.1;
// specs/003-host-core-engine/contracts/bp-slot-map.md; mirrors tools/refimpl/test_l3.py's
// bp_slot_map cases.
#include "catch_amalgamated.hpp"
#include "heap_guard.hpp"
#include "l3/l3_payload.hpp"
#include "omgp_protocol.h"

#include <vector>

using namespace omgp::l3;

namespace {
std::vector<uint8_t> bitmap(size_t len, uint8_t fill) {
    return std::vector<uint8_t>(len, fill);
}
size_t bitmap_len(uint8_t slot_count) {
    return (static_cast<size_t>(slot_count) + 7) / 8;
}
} // namespace

TEST_CASE("BP_SLOT_MAP round-trips at slot_count 0, 1, 8, and the cap", "[bp_slot_map]") {
    for (const uint8_t slot_count :
         {static_cast<uint8_t>(0), static_cast<uint8_t>(1), static_cast<uint8_t>(8),
          static_cast<uint8_t>(omgp::LIMIT_bp_slot_map_max_slots)}) {
        INFO("slot_count=" << static_cast<int>(slot_count));
        const size_t blen = bitmap_len(slot_count);
        auto occ = bitmap(blen, 0xA5);
        auto chg = bitmap(blen, 0x5A);
        uint8_t buf[1 + 2 * 29] = {};
        size_t n = 0;
        Status st;
        HEAP_FREE_SCOPE({
            st = encode_bp_slot_map_resp(
                BpSlotMapResp{slot_count, Bytes{occ.data(), static_cast<uint8_t>(occ.size())},
                              Bytes{chg.data(), static_cast<uint8_t>(chg.size())}},
                buf, sizeof buf, n);
        });
        REQUIRE(st == Status::Ok);
        REQUIRE(n == 1 + 2 * blen);
        REQUIRE(buf[0] == slot_count);

        BpSlotMapResp d;
        HEAP_FREE_SCOPE({ st = decode_bp_slot_map_resp(buf, n, d); });
        REQUIRE(st == Status::Ok);
        REQUIRE(d.slot_count == slot_count);
        REQUIRE(d.occupied.len == blen);
        REQUIRE(d.changed.len == blen);
        for (size_t i = 0; i < blen; ++i) {
            REQUIRE(d.occupied.data[i] == 0xA5);
            REQUIRE(d.changed.data[i] == 0x5A);
        }
    }
}

TEST_CASE("BP_SLOT_MAP: slot_count over the cap is OutOfRange, encode and decode",
          "[bp_slot_map]") {
    constexpr uint8_t over = static_cast<uint8_t>(omgp::LIMIT_bp_slot_map_max_slots + 1);
    const size_t blen = bitmap_len(over);
    auto occ = bitmap(blen, 0);
    auto chg = bitmap(blen, 0);
    uint8_t buf[1 + 2 * 30] = {};
    size_t n = 0;
    REQUIRE(encode_bp_slot_map_resp(
                BpSlotMapResp{over, Bytes{occ.data(), static_cast<uint8_t>(occ.size())},
                              Bytes{chg.data(), static_cast<uint8_t>(chg.size())}},
                buf, sizeof buf, n) == Status::OutOfRange);

    // A wire message that only *declares* slot_count = over, with the bytes that would
    // actually follow it: decode must refuse it too, not just the encoder.
    buf[0] = over;
    for (size_t i = 0; i < blen; ++i)
        buf[1 + i] = buf[1 + blen + i] = 0;
    BpSlotMapResp d;
    REQUIRE(decode_bp_slot_map_resp(buf, 1 + 2 * blen, d) == Status::OutOfRange);
}

TEST_CASE("BP_SLOT_MAP: encoder rejects a bitmap whose length disagrees with slot_count",
          "[bp_slot_map]") {
    uint8_t buf[16];
    size_t n = 0;
    uint8_t occ1[1] = {0}, chg1[1] = {0};
    // slot_count 8 needs 1-byte bitmaps; feeding 2 bytes for one of them is a shape error,
    // not a valid encoding of a different slot_count.
    uint8_t occ2[2] = {0, 0};
    REQUIRE(encode_bp_slot_map_resp(BpSlotMapResp{8, Bytes{occ2, 2}, Bytes{chg1, 1}}, buf,
                                    sizeof buf, n) == Status::OutOfRange);
    REQUIRE(encode_bp_slot_map_resp(BpSlotMapResp{8, Bytes{occ1, 1}, Bytes{occ2, 2}}, buf,
                                    sizeof buf, n) == Status::OutOfRange);
}

TEST_CASE("BP_SLOT_MAP: decoder Truncated/LengthMismatch per the shared file-header rules",
          "[bp_slot_map]") {
    BpSlotMapResp d;
    // Not even the slot_count byte.
    REQUIRE(decode_bp_slot_map_resp(nullptr, 0, d) == Status::Truncated);
    // slot_count = 8 needs 1 + 2*1 = 3 bytes total; 2 is short.
    const uint8_t two[] = {8, 0xFF};
    REQUIRE(decode_bp_slot_map_resp(two, sizeof two, d) == Status::Truncated);
    // 4 is one more than the 3 the shape requires.
    const uint8_t four[] = {8, 0xFF, 0xFF, 0x00};
    REQUIRE(decode_bp_slot_map_resp(four, sizeof four, d) == Status::LengthMismatch);
    // slot_count = 0 needs exactly 1 byte, zero bitmap bytes.
    const uint8_t zero_slots[] = {0};
    REQUIRE(decode_bp_slot_map_resp(zero_slots, sizeof zero_slots, d) == Status::Ok);
    REQUIRE((d.slot_count == 0 && d.occupied.len == 0 && d.changed.len == 0));
}

TEST_CASE("BP_SLOT_MAP: no allocation on the encode/decode path", "[bp_slot_map][heap]") {
    uint8_t occ[29] = {}, chg[29] = {};
    uint8_t buf[1 + 2 * 29];
    size_t n = 0;
    HEAP_FREE_SCOPE({
        REQUIRE(encode_bp_slot_map_resp(BpSlotMapResp{232, Bytes{occ, 29}, Bytes{chg, 29}}, buf,
                                        sizeof buf, n) == Status::Ok);
        BpSlotMapResp d;
        REQUIRE(decode_bp_slot_map_resp(buf, n, d) == Status::Ok);
    });
}

TEST_CASE("BP_SLOT_MAP: payload_bounds matches the generated PAYLOAD_INFO row", "[bp_slot_map]") {
    uint8_t mn, mx;
    bool opaque;
    REQUIRE(payload_bounds(omgp::OP_BP_SLOT_MAP, Dir::Request, mn, mx, opaque) == Status::Ok);
    REQUIRE((mn == 0 && mx == 0 && !opaque));
    REQUIRE(payload_bounds(omgp::OP_BP_SLOT_MAP, Dir::Response, mn, mx, opaque) == Status::Ok);
    REQUIRE((mn == 1 && mx == 1 + 2 * 29 && !opaque));
}

// AC4 (#676): the worst-case encoded response at the ruled cap must fit LIMIT_max_l3_payload —
// computed from the *generated* constants, not restated as a literal, so a future change to
// either limit is caught here rather than silently drifting from the YAML's own comment.
TEST_CASE("BP_SLOT_MAP: worst-case size at LIMIT_bp_slot_map_max_slots fits LIMIT_max_l3_payload",
          "[bp_slot_map]") {
    const size_t worst_case_bitmap_len =
        (static_cast<size_t>(omgp::LIMIT_bp_slot_map_max_slots) + 7) / 8;
    const size_t worst_case_total = 1 + 2 * worst_case_bitmap_len;
    REQUIRE(worst_case_total <= omgp::LIMIT_max_l3_payload);
    // And it's the actual ceiling, not slack left on the table: one more slot must not fit.
    const size_t one_more_bitmap_len =
        (static_cast<size_t>(omgp::LIMIT_bp_slot_map_max_slots) + 1 + 7) / 8;
    REQUIRE(1 + 2 * one_more_bitmap_len > omgp::LIMIT_max_l3_payload);
}
