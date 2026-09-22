// BP_SLOT_MAP response codec (spec 003 T005-T007, R-01). protocol-l3 §3.1;
// specs/003-host-core-engine/contracts/bp-slot-map.md; mirrors tools/refimpl/test_l3.py's
// bp_slot_map cases.
//
// T006 (#677) completion pass: the cases below marked "#677 AC" were named by that issue's
// acceptance criteria but absent from the file PR #773 landed — the exact encoded sizes, the
// asymmetric bit-convention case, no-partial-write on BufferTooSmall, and Truncated /
// LengthMismatch at slot_counts other than 8. Every bound is the generated symbol
// (CLAUDE.md rule 4); malformed-input cases are handed buffers sized exactly to their claimed
// `len`, so ASan flags any read past the end (rule 7 — demonstrated under the sanitizer by
// these tests, not proved by construction).
#include "catch_amalgamated.hpp"
#include "heap_guard.hpp"
#include "l3/l3_payload.hpp"
#include "omgp_protocol.h"

#include <vector>

using namespace omgp::l3;

namespace {
// The ruled cap and the shapes derived from it — never the literals 232/233/29 (rule 4).
constexpr uint8_t kMaxSlots = static_cast<uint8_t>(omgp::LIMIT_bp_slot_map_max_slots);
constexpr uint8_t kOverSlots = static_cast<uint8_t>(omgp::LIMIT_bp_slot_map_max_slots + 1);
constexpr size_t kMaxBitmap = (static_cast<size_t>(kMaxSlots) + 7) / 8;
constexpr size_t kOverBitmap = (static_cast<size_t>(kOverSlots) + 7) / 8;
constexpr size_t kMaxEncoded = 1 + 2 * kMaxBitmap;
constexpr size_t kOverEncoded = 1 + 2 * kOverBitmap;

// The slot_counts every boundary case sweeps: empty, the ceil-vs-floor discriminator, the
// exact-multiple-of-8 case where the two agree, and the ruled cap.
constexpr uint8_t kBoundarySlots[] = {0, 1, 8, kMaxSlots};

// #677 AC: the cap is exactly the largest slot_count whose payload still fits, asserted at
// compile time off the generated constants (the runtime case below also pins the other
// direction — that no slack is left on the table).
static_assert(kMaxEncoded <= omgp::LIMIT_max_l3_payload,
              "LIMIT_bp_slot_map_max_slots' worst-case payload must fit LIMIT_max_l3_payload");

std::vector<uint8_t> bitmap(size_t len, uint8_t fill) {
    return std::vector<uint8_t>(len, fill);
}
size_t bitmap_len(uint8_t slot_count) {
    return (static_cast<size_t>(slot_count) + 7) / 8;
}
Bytes view(const std::vector<uint8_t>& v) {
    return Bytes{v.data(), static_cast<uint8_t>(v.size())};
}
// The documented convention (contracts/bp-slot-map.md): slot i is bit (i % 8) of byte i / 8,
// LSB first within the byte.
void set_slot(std::vector<uint8_t>& bm, size_t i) {
    bm[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
}
bool slot_set(const Bytes& bm, size_t i) {
    return ((bm.data[i / 8] >> (i % 8)) & 1u) != 0;
}
// Encode `r` with no allocation, returning the status; `n` carries `written` back out.
Status encode_heap_free(const BpSlotMapResp& r, uint8_t* out, size_t cap, size_t& n) {
    Status st = Status::Ok;
    HEAP_FREE_SCOPE({ st = encode_bp_slot_map_resp(r, out, cap, n); });
    return st;
}
Status decode_heap_free(const uint8_t* in, size_t len, BpSlotMapResp& out) {
    Status st = Status::Ok;
    HEAP_FREE_SCOPE({ st = decode_bp_slot_map_resp(in, len, out); });
    return st;
}
} // namespace

TEST_CASE("BP_SLOT_MAP round-trips at slot_count 0, 1, 8, and the cap", "[bp_slot_map]") {
    for (const uint8_t slot_count : kBoundarySlots) {
        INFO("slot_count=" << static_cast<int>(slot_count));
        const size_t blen = bitmap_len(slot_count);
        auto occ = bitmap(blen, 0xA5);
        auto chg = bitmap(blen, 0x5A);
        const BpSlotMapResp r{slot_count, view(occ), view(chg)};
        uint8_t buf[kMaxEncoded] = {};
        size_t n = 0;
        Status st = encode_heap_free(r, buf, sizeof buf, n);
        REQUIRE(st == Status::Ok);
        REQUIRE(n == 1 + 2 * blen);
        REQUIRE(buf[0] == slot_count);

        BpSlotMapResp d;
        st = decode_heap_free(buf, n, d);
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

// #677 AC: sizes as exact numerals, not the formula restated. 1 -> 3 and 8 -> 3 are the pair
// that discriminates ceil(slot_count/8) from plain slot_count/8 (quickstart §2): at 8, and at
// the cap, the two agree, so a multiple-of-8 case alone would pin nothing.
TEST_CASE("BP_SLOT_MAP: encoded size is exactly 1 + 2*ceil(slot_count/8)", "[bp_slot_map]") {
    struct Case {
        uint8_t slot_count;
        size_t encoded;
    };
    const Case cases[] = {{0, 1}, {1, 3}, {8, 3}, {9, 5}, {kMaxSlots, kMaxEncoded}};
    for (const auto& k : cases) {
        INFO("slot_count=" << static_cast<int>(k.slot_count));
        const size_t blen = bitmap_len(k.slot_count);
        auto occ = bitmap(blen, 0);
        auto chg = bitmap(blen, 0);
        const BpSlotMapResp r{k.slot_count, view(occ), view(chg)};
        uint8_t buf[kMaxEncoded] = {};
        size_t n = 99;
        const Status st = encode_heap_free(r, buf, sizeof buf, n);
        REQUIRE(st == Status::Ok);
        REQUIRE(n == k.encoded);
    }
}

// #677 AC: pin the bit convention with an asymmetric case. All-zeros/all-ones and the 0xA5/0x5A
// fills above would survive both an MSB-first bit order and a swap of the two bitmaps.
TEST_CASE("BP_SLOT_MAP: slot i is bit (i%8) of byte i/8, LSB first, occupied before changed",
          "[bp_slot_map]") {
    // 16 slots = two bitmap bytes. occupied = slots 0 and 9; changed = slot 1 only.
    auto occ = bitmap(2, 0);
    set_slot(occ, 0);
    set_slot(occ, 9);
    auto chg = bitmap(2, 0);
    set_slot(chg, 1);

    const BpSlotMapResp r{16, view(occ), view(chg)};
    uint8_t buf[kMaxEncoded] = {};
    size_t n = 0;
    const Status st = encode_heap_free(r, buf, sizeof buf, n);
    REQUIRE(st == Status::Ok);
    // 0x01 not 0x80 rules out MSB-first; slot 9 landing in the *second* byte as 0x02 rules out
    // a single flat index; {0x02, 0x00} differing from occupied rules out a bitmap swap.
    const std::vector<uint8_t> expect{16, 0x01, 0x02, 0x02, 0x00};
    REQUIRE(std::vector<uint8_t>(buf, buf + n) == expect);

    BpSlotMapResp d;
    REQUIRE(decode_heap_free(expect.data(), expect.size(), d) == Status::Ok);
    REQUIRE(d.slot_count == 16);
    for (size_t i = 0; i < 16; ++i) {
        INFO("slot=" << i);
        REQUIRE(slot_set(d.occupied, i) == (i == 0 || i == 9));
        REQUIRE(slot_set(d.changed, i) == (i == 1));
    }
}

TEST_CASE("BP_SLOT_MAP: slot_count over the cap is OutOfRange, encode and decode",
          "[bp_slot_map]") {
    auto occ = bitmap(kOverBitmap, 0);
    auto chg = bitmap(kOverBitmap, 0);
    const BpSlotMapResp r{kOverSlots, view(occ), view(chg)};
    uint8_t buf[kOverEncoded] = {};
    size_t n = 0;
    REQUIRE(encode_heap_free(r, buf, sizeof buf, n) == Status::OutOfRange);

    // A full-shape wire message declaring slot_count = over, with the (over-long, not
    // actually deliverable) bytes that shape would need: decode must refuse it too.
    buf[0] = kOverSlots;
    for (size_t i = 0; i < kOverBitmap; ++i)
        buf[1 + i] = buf[1 + kOverBitmap + i] = 0;
    BpSlotMapResp d;
    REQUIRE(decode_heap_free(buf, kOverEncoded, d) == Status::OutOfRange);
}

TEST_CASE("BP_SLOT_MAP: an over-cap slot_count is OutOfRange even off a short, wire-legal "
          "payload — not masked by a Truncated length check (red team, PR #773 round 1)",
          "[bp_slot_map]") {
    // A payload this short could never legally carry slot_count = 233's own claimed shape
    // (it would need 61 bytes, more than LIMIT_max_l3_payload itself), so decoding it must
    // not be able to answer Truncated instead: the cap violation is checked off the single
    // slot_count byte, before any length check, so it is reachable on every real L3 message
    // a decoder can actually be handed — not just a codec-level probe with an oversized
    // buffer no wire payload could ever deliver.
    const uint8_t just_the_header[] = {kOverSlots};
    BpSlotMapResp d;
    REQUIRE(decode_heap_free(just_the_header, sizeof just_the_header, d) == Status::OutOfRange);

    // Also at the actual wire-legal ceiling: a full LIMIT_max_l3_payload-byte message.
    uint8_t at_max_payload[omgp::LIMIT_max_l3_payload] = {};
    at_max_payload[0] = kOverSlots;
    REQUIRE(decode_heap_free(at_max_payload, sizeof at_max_payload, d) == Status::OutOfRange);
}

TEST_CASE("BP_SLOT_MAP: reserved padding bits beyond slot_count are passed through, not "
          "rejected (golden rule 7)",
          "[bp_slot_map]") {
    // slot_count=4 needs one bitmap byte; bits 4-7 of that byte are reserved padding (docs
    // protocol-l3.md, contracts/bp-slot-map.md). A backplane setting them is unusual but not
    // a shape violation: decode must accept it (the host is expected to ignore them, not the
    // decoder to enforce it) and round-trip the byte verbatim.
    const uint8_t wire[] = {4, 0xF0, 0x00};
    BpSlotMapResp d;
    REQUIRE(decode_heap_free(wire, sizeof wire, d) == Status::Ok);
    REQUIRE((d.slot_count == 4 && d.occupied.len == 1 && d.occupied.data[0] == 0xF0));
    uint8_t buf[8];
    size_t n = 0;
    REQUIRE(encode_heap_free(d, buf, sizeof buf, n) == Status::Ok);
    REQUIRE(std::vector<uint8_t>(buf, buf + n) == std::vector<uint8_t>(wire, wire + sizeof wire));
}

TEST_CASE("BP_SLOT_MAP: encoder rejects a bitmap whose length disagrees with slot_count",
          "[bp_slot_map]") {
    uint8_t buf[16];
    size_t n = 0;
    uint8_t occ1[1] = {0}, chg1[1] = {0};
    // slot_count 8 needs 1-byte bitmaps; feeding 2 bytes for one of them is a shape error,
    // not a valid encoding of a different slot_count.
    uint8_t occ2[2] = {0, 0};
    const BpSlotMapResp long_occ{8, Bytes{occ2, 2}, Bytes{chg1, 1}};
    const BpSlotMapResp long_chg{8, Bytes{occ1, 1}, Bytes{occ2, 2}};
    REQUIRE(encode_heap_free(long_occ, buf, sizeof buf, n) == Status::OutOfRange);
    REQUIRE(encode_heap_free(long_chg, buf, sizeof buf, n) == Status::OutOfRange);
}

// #677 AC: BufferTooSmall leaves the output untouched and does not advance `written`. The
// "one byte less" edge alone is covered by test_l3_payload.cpp's every-encoder table; what is
// pinned here is the *no partial write* half — a sentinel-filled buffer asserted byte-for-byte
// unchanged — across every short capacity, not just total-1.
TEST_CASE("BP_SLOT_MAP: BufferTooSmall writes nothing at any capacity below the encoded size",
          "[bp_slot_map]") {
    for (const uint8_t slot_count : kBoundarySlots) {
        const size_t blen = bitmap_len(slot_count);
        const size_t total = 1 + 2 * blen;
        auto occ = bitmap(blen, 0xFF);
        auto chg = bitmap(blen, 0xFF);
        const BpSlotMapResp r{slot_count, view(occ), view(chg)};
        const std::vector<uint8_t> sentinel(total, 0xCD);
        for (size_t cap = 0; cap < total; ++cap) {
            INFO("slot_count=" << static_cast<int>(slot_count) << " cap=" << cap);
            std::vector<uint8_t> out = sentinel;
            size_t n = 99;
            const Status st = encode_heap_free(r, out.data(), cap, n);
            REQUIRE(st == Status::BufferTooSmall);
            REQUIRE(n == 0);
            REQUIRE(out == sentinel);
        }
    }
}

TEST_CASE("BP_SLOT_MAP: decoder Truncated/LengthMismatch per the shared file-header rules",
          "[bp_slot_map]") {
    BpSlotMapResp d;
    // Not even the slot_count byte.
    REQUIRE(decode_heap_free(nullptr, 0, d) == Status::Truncated);
    // slot_count = 8 needs 1 + 2*1 = 3 bytes total; 2 is short.
    const uint8_t two[] = {8, 0xFF};
    REQUIRE(decode_heap_free(two, sizeof two, d) == Status::Truncated);
    // 4 is one more than the 3 the shape requires.
    const uint8_t four[] = {8, 0xFF, 0xFF, 0x00};
    REQUIRE(decode_heap_free(four, sizeof four, d) == Status::LengthMismatch);
    // slot_count = 0 needs exactly 1 byte, zero bitmap bytes.
    const uint8_t zero_slots[] = {0};
    REQUIRE(decode_heap_free(zero_slots, sizeof zero_slots, d) == Status::Ok);
    REQUIRE((d.slot_count == 0 && d.occupied.len == 0 && d.changed.len == 0));
}

// #677 AC: the same two length rules across the slot_counts the case above leaves out — 1 and
// the cap especially, since 8 is the one value where ceil and truncating division agree. Each
// buffer is a heap vector sized exactly to the `len` passed, so an over-read is an ASan fault
// rather than a silent read into slack.
TEST_CASE("BP_SLOT_MAP: Truncated one byte short, LengthMismatch one byte long, at every "
          "boundary slot_count",
          "[bp_slot_map]") {
    for (const uint8_t slot_count : kBoundarySlots) {
        INFO("slot_count=" << static_cast<int>(slot_count));
        const size_t total = 1 + 2 * bitmap_len(slot_count);
        BpSlotMapResp d;

        // One byte short. slot_count = 0 has no short form above the empty payload, which the
        // len == 0 case already covers, so it is exercised as LengthMismatch only.
        if (total > 1) {
            std::vector<uint8_t> shortbuf(total - 1, 0);
            shortbuf[0] = slot_count;
            REQUIRE(decode_heap_free(shortbuf.data(), shortbuf.size(), d) == Status::Truncated);
        }

        // One byte long.
        std::vector<uint8_t> longbuf(total + 1, 0);
        longbuf[0] = slot_count;
        REQUIRE(decode_heap_free(longbuf.data(), longbuf.size(), d) == Status::LengthMismatch);
    }
}

TEST_CASE("BP_SLOT_MAP: no allocation on the encode/decode path", "[bp_slot_map][heap]") {
    constexpr uint8_t kLen = static_cast<uint8_t>(kMaxBitmap);
    uint8_t occ[kMaxBitmap] = {}, chg[kMaxBitmap] = {};
    const BpSlotMapResp r{kMaxSlots, Bytes{occ, kLen}, Bytes{chg, kLen}};
    uint8_t buf[kMaxEncoded];
    size_t n = 0;
    HEAP_FREE_SCOPE({
        REQUIRE(encode_bp_slot_map_resp(r, buf, sizeof buf, n) == Status::Ok);
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
    REQUIRE((mn == 1 && mx == kMaxEncoded && !opaque));
}

// AC4 (#676): the worst-case encoded response at the ruled cap must fit LIMIT_max_l3_payload —
// computed from the *generated* constants, not restated as a literal, so a future change to
// either limit is caught here rather than silently drifting from the YAML's own comment.
TEST_CASE("BP_SLOT_MAP: worst-case size at LIMIT_bp_slot_map_max_slots fits LIMIT_max_l3_payload",
          "[bp_slot_map]") {
    REQUIRE(kMaxEncoded <= omgp::LIMIT_max_l3_payload);
    // And it's the actual ceiling, not slack left on the table: one more slot must not fit.
    REQUIRE(kOverEncoded > omgp::LIMIT_max_l3_payload);
}
