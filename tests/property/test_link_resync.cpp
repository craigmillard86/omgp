// Trunk L2 resync property test (spec 002 US1, T016). trunk §4 ("discarded silently;
// resynchronisation is on the next FLAG"); data-model.md §11 (torture corpus element,
// mirrored here in C++ — the eight corruption classes tools/refimpl/torture.py defines).
// Runs under ASan/UBSan via ctest. Written from the C++ contract, not from
// link/frame.{hpp,cpp} — this file is expected to fail to compile until T022 lands
// link/frame.hpp.
#include "catch_amalgamated.hpp"
#include "link/frame.hpp"
#include "link/link_types.hpp"
#include "omgp_protocol.h"

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

using namespace omgp::link;

namespace {
constexpr uint32_t kSeed = 0xB0071E;
constexpr uint8_t kFlag = omgp::TRUNK_flag_byte;
constexpr uint8_t kEsc = omgp::TRUNK_escape_byte;
constexpr uint8_t kXor = omgp::TRUNK_escape_xor;

struct RandFrame {
    uint8_t dst, src;
    bool response, retry;
    uint8_t seq;
    std::vector<uint8_t> payload;
};

RandFrame random_valid_frame(std::mt19937& rng) {
    RandFrame f;
    f.dst = static_cast<uint8_t>(rng() % 0xFF); // excludes 0xFF (reserved, trunk §5)
    f.src = static_cast<uint8_t>(rng() & 0xFF);
    f.response = (rng() & 1) != 0;
    f.retry = (rng() & 1) != 0;
    f.seq = static_cast<uint8_t>(rng() % 16);
    const uint8_t len = static_cast<uint8_t>(rng() % (omgp::LIMIT_max_l3_payload + 1));
    f.payload.resize(len);
    for (auto& b : f.payload)
        b = static_cast<uint8_t>(rng() & 0xFF);
    return f;
}

std::vector<uint8_t> encode(const RandFrame& f) {
    const uint8_t len = static_cast<uint8_t>(f.payload.size());
    const uint8_t* p = f.payload.empty() ? nullptr : f.payload.data();
    const FrameFields ff{f.dst, f.src, f.response, f.retry, f.seq, len, p};
    uint8_t out[kMaxWire];
    size_t written = 0;
    REQUIRE(encode_frame(ff, out, sizeof out, written) == Status::Ok);
    return std::vector<uint8_t>(out, out + written);
}

// The eight corruption classes of data-model.md §11 / tools/refimpl/torture.py,
// applied to a copy of a valid frame's unstuffed body (dst..crc, FLAGs stripped).
enum class Corruption { Flip, Drop, Insert, Truncate, Flag, BadEscape, Garbage, Overlength };
constexpr Corruption kAllClasses[] = {
    Corruption::Flip, Corruption::Drop,      Corruption::Insert,  Corruption::Truncate,
    Corruption::Flag, Corruption::BadEscape, Corruption::Garbage, Corruption::Overlength,
};

uint8_t random_byte_excluding(std::mt19937& rng, std::initializer_list<uint8_t> excluded) {
    uint8_t b;
    bool bad;
    do {
        b = static_cast<uint8_t>(rng() & 0xFF);
        bad = false;
        for (uint8_t e : excluded)
            if (b == e)
                bad = true;
    } while (bad);
    return b;
}

bool corrupt(Corruption c, std::mt19937& rng, std::vector<uint8_t>& body) {
    switch (c) {
    case Corruption::Flip: {
        if (body.empty())
            return false;
        const size_t i = rng() % body.size();
        const uint8_t bit = static_cast<uint8_t>(1u << (rng() % 8));
        body[i] ^= bit;
        return true;
    }
    case Corruption::Drop: {
        if (body.empty())
            return false;
        body.erase(body.begin() + static_cast<long>(rng() % body.size()));
        return true;
    }
    case Corruption::Insert: {
        const size_t i = rng() % (body.size() + 1);
        body.insert(body.begin() + static_cast<long>(i), random_byte_excluding(rng, {kFlag}));
        return true;
    }
    case Corruption::Truncate: {
        if (body.size() < 2)
            return false;
        const size_t cut = 1 + rng() % (body.size() - 1);
        body.resize(cut);
        return true;
    }
    case Corruption::Flag: {
        if (body.empty())
            return false;
        const size_t i = rng() % (body.size() + 1);
        body.insert(body.begin() + static_cast<long>(i), kFlag);
        return true;
    }
    case Corruption::BadEscape: {
        std::vector<size_t> positions;
        for (size_t i = 0; i + 1 < body.size(); ++i)
            if (body[i] == kEsc)
                positions.push_back(i);
        const uint8_t good_a = static_cast<uint8_t>(kFlag ^ kXor);
        const uint8_t good_b = static_cast<uint8_t>(kEsc ^ kXor);
        if (!positions.empty()) {
            const size_t p = positions[rng() % positions.size()];
            body[p + 1] = random_byte_excluding(rng, {good_a, good_b});
        } else {
            const size_t i = rng() % (body.size() + 1);
            const uint8_t bad = random_byte_excluding(rng, {good_a, good_b});
            body.insert(body.begin() + static_cast<long>(i), bad);
            body.insert(body.begin() + static_cast<long>(i), kEsc);
        }
        return true;
    }
    case Corruption::Garbage: {
        const size_t n = 1 + rng() % omgp::LIMIT_max_l3_payload;
        body.resize(n);
        for (auto& b : body)
            b = random_byte_excluding(rng, {kFlag});
        return true;
    }
    case Corruption::Overlength: {
        // kMaxUnstuffed (70) plus extra plain bytes, avoiding FLAG/ESC entirely for a
        // deterministic TooLong abort; the escaped-boundary variant (the 71st byte
        // arriving via a valid escape, PR #99's bug class) is covered directly by
        // tests/unit/test_link_frame.cpp.
        const size_t extra = 1 + rng() % omgp::LIMIT_max_l3_payload;
        body.resize(kMaxUnstuffed + extra);
        for (auto& b : body)
            b = random_byte_excluding(rng, {kFlag, kEsc});
        return true;
    }
    }
    return false;
}

// Builds one wire-ready corrupted element (both FLAGs included) for corruption class `c`.
// Self-checks against an isolated Deframer (data-model.md §11 / R-08): if the corruption
// still parses as a delivered frame (CRC-lucky), it is discarded and regenerated from the
// same seeded generator — so the returned element never round-trips as a valid frame.
std::vector<uint8_t> build_corrupted_element(Corruption c, std::mt19937& rng) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        const RandFrame base = random_valid_frame(rng);
        const std::vector<uint8_t> wire = encode(base);
        std::vector<uint8_t> body(wire.begin() + 1, wire.end() - 1); // strip both FLAGs
        if (!corrupt(c, rng, body))
            continue;
        std::vector<uint8_t> stream;
        stream.push_back(kFlag);
        stream.insert(stream.end(), body.begin(), body.end());
        stream.push_back(kFlag);

        Deframer check;
        FrameView v{};
        bool delivered = false;
        for (uint8_t b : stream)
            if (check.feed(b, v))
                delivered = true;
        if (delivered)
            continue; // CRC-lucky: regenerate with the next sub-seed
        return stream;
    }
    FAIL("could not build a non-parsing corrupted element after 500 attempts");
    return {};
}

bool fields_match(const FrameFields& got, const RandFrame& exp) {
    if (got.dst != exp.dst || got.src != exp.src || got.response != exp.response ||
        got.retry != exp.retry || got.seq != exp.seq || got.len != exp.payload.size())
        return false;
    if (exp.payload.empty())
        return true;
    return std::memcmp(got.payload, exp.payload.data(), exp.payload.size()) == 0;
}

} // namespace

TEST_CASE("resync after each corruption class: exactly the intact frames are delivered, "
          "in order, and no corrupted frame is ever delivered",
          "[property]") {
    std::mt19937 rng(kSeed ^ 0x2E5C);
    constexpr int kPerClass = 150;

    for (Corruption c : kAllClasses) {
        for (int trial = 0; trial < kPerClass; ++trial) {
            std::vector<uint8_t> stream;
            std::vector<RandFrame> expected;

            // Sandwich the corrupted element between valid frames so intact frames both
            // before and after must survive resync (FR-003).
            const int before = 1 + static_cast<int>(rng() % 3);
            for (int i = 0; i < before; ++i) {
                const RandFrame f = random_valid_frame(rng);
                const auto wire = encode(f);
                stream.insert(stream.end(), wire.begin(), wire.end());
                expected.push_back(f);
            }

            const std::vector<uint8_t> corrupted = build_corrupted_element(c, rng);
            stream.insert(stream.end(), corrupted.begin(), corrupted.end());

            const int after = 1 + static_cast<int>(rng() % 3);
            for (int i = 0; i < after; ++i) {
                const RandFrame f = random_valid_frame(rng);
                const auto wire = encode(f);
                stream.insert(stream.end(), wire.begin(), wire.end());
                expected.push_back(f);
            }

            Deframer d;
            FrameView view{};
            size_t delivered_count = 0;
            for (uint8_t b : stream) {
                if (d.feed(b, view)) {
                    ++delivered_count;
                    REQUIRE(delivered_count <= expected.size());
                    REQUIRE(fields_match(view.f, expected[delivered_count - 1]));
                }
            }
            REQUIRE(delivered_count == expected.size());
            REQUIRE(d.stats().delivered == expected.size());
        }
    }
}
