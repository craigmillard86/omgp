// Host-only canonical-text frame verbs (spec 002 US1, T023). contracts/frame-vectors.md
// "Canonical frame line" and "l3_helper verbs". Exercises exactly the functions l3_helper.cpp
// calls for FENC/FDEC/FSTREAM (tools/canonical.hpp), the same split test_l3_payload.cpp uses
// for ENC/DEC. Doesn't re-test encode_frame/Deframer correctness itself (tests/unit/
// test_link_frame.cpp already covers every Discard reason and the golden vectors byte-for-byte)
// — only the rendering/parsing/dispatch this task adds on top of that codec.
#include "canonical.hpp"
#include "catch_amalgamated.hpp"
#include "link/link_types.hpp"
#include "omgp_protocol.h"
#include "omgp_vectors.h"

#include <cstring>
#include <string>
#include <vector>

using namespace omgp::link;

// --- golden vectors: FENC and FDEC round-trip every frame_* vector exactly -----------------

TEST_CASE("FENC-equivalent: encode_frame_line reproduces every frame_* vector's wire bytes",
          "[vectors]") {
    size_t seen = 0;
    for (size_t i = 0; i < omgp::vectors::COUNT; ++i) {
        const auto& v = omgp::vectors::ALL[i];
        if (std::string(v.kind) != "frame")
            continue;
        ++seen;
        INFO(v.name << " (" << v.spec_ref << ")");
        std::vector<uint8_t> out;
        std::string error;
        const bool ok = omgp::canon::encode_frame_line(v.canonical, out, error);
        REQUIRE(ok);
        REQUIRE(out.size() == static_cast<size_t>(v.len));
        REQUIRE(std::memcmp(out.data(), v.bytes, out.size()) == 0);
    }
    REQUIRE(seen == 5);
}

TEST_CASE("FDEC-equivalent: fdec_line reproduces every frame_* vector's canonical line",
          "[vectors]") {
    size_t seen = 0;
    for (size_t i = 0; i < omgp::vectors::COUNT; ++i) {
        const auto& v = omgp::vectors::ALL[i];
        if (std::string(v.kind) != "frame")
            continue;
        ++seen;
        INFO(v.name << " (" << v.spec_ref << ")");
        const std::string line = omgp::canon::fdec_line(v.bytes, v.len);
        REQUIRE(line == "OK " + std::string(v.canonical));
    }
    REQUIRE(seen == 5);
}

// --- render_frame / parse_frame_line in isolation --------------------------------------------

TEST_CASE("render_frame renders response and retry as the two low flag bits", "[frame]") {
    uint8_t payload[] = {0xAB, 0xCD};
    FrameFields f{0x01, 0x02, true, false, 9, 2, payload};
    REQUIRE(omgp::canon::render_frame(f) ==
            "frame dst=0x01 src=0x02 flags=0x01 seq=9 payload=abcd");
    f.response = false;
    f.retry = true;
    REQUIRE(omgp::canon::render_frame(f) ==
            "frame dst=0x01 src=0x02 flags=0x02 seq=9 payload=abcd");
    f.response = true;
    f.retry = true;
    REQUIRE(omgp::canon::render_frame(f) ==
            "frame dst=0x01 src=0x02 flags=0x03 seq=9 payload=abcd");
}

TEST_CASE("render_frame renders an empty payload as an empty hex field", "[frame]") {
    FrameFields f{0x00, 0x00, false, false, 0, 0, nullptr};
    REQUIRE(omgp::canon::render_frame(f) == "frame dst=0x00 src=0x00 flags=0x00 seq=0 payload=");
}

TEST_CASE("parse_frame_line rejects malformed canonical text before it reaches the codec",
          "[frame]") {
    struct Case {
        const char* text;
        const char* why;
    };
    const Case cases[] = {
        {"frame dst=0x01 src=0x00 flags=0x00 seq=0", "missing payload field"},
        {"frame dst=0x01 src=0x00 flags=0x00 payload=00", "missing seq field"},
        {"frame dst=0x01 src=0x00 flags=0x00 seq=16 payload=00", "seq out of 0..15 range"},
        {"frame dst=0x01 src=0x00 flags=0x00 seq=0 payload=0", "odd-length payload hex"},
        {"frame dst=0x01 src=0x00 flags=0x00 seq=0 payload=zz", "non-hex payload"},
        {"not-a-frame dst=0x01 src=0x00 flags=0x00 seq=0 payload=", "wrong line prefix"},
    };
    for (const auto& c : cases) {
        INFO(c.why);
        FrameFields f{};
        std::vector<uint8_t> payload;
        std::string error;
        REQUIRE_FALSE(omgp::canon::parse_frame_line(c.text, f, payload, error));
        REQUIRE(error == "ERR BadRequest");
    }
}

// --- encode_frame_line (FENC) refusals: codec status, not BadRequest ------------------------

TEST_CASE("encode_frame_line surfaces encode_frame's own refusal spellings", "[frame]") {
    std::vector<uint8_t> out;
    std::string error;
    // dst == 0xFF: reserved broadcast address (trunk §5).
    REQUIRE_FALSE(omgp::canon::encode_frame_line(
        "frame dst=0xFF src=0x00 flags=0x00 seq=0 payload=", out, error));
    REQUIRE(error == "ERR ReservedAddress");

    // payload longer than LIMIT_max_l3_payload (64 bytes): 65 bytes of "00".
    std::string long_payload;
    for (int i = 0; i < 65; ++i)
        long_payload += "00";
    REQUIRE_FALSE(omgp::canon::encode_frame_line(
        "frame dst=0x01 src=0x00 flags=0x00 seq=0 payload=" + long_payload, out, error));
    REQUIRE(error == "ERR PayloadTooLong");
}

// --- fdec_line (FDEC): the discriminating check (CLAUDE.md rule 11) -------------------------

TEST_CASE("fdec_line: a flipped CRC byte on frame_ping_req is ERR BadCrc, not OK", "[frame]") {
    const uint8_t* good = nullptr;
    uint16_t good_len = 0;
    for (size_t i = 0; i < omgp::vectors::COUNT; ++i) {
        const auto& v = omgp::vectors::ALL[i];
        if (std::string(v.name) == "frame_ping_req") {
            good = v.bytes;
            good_len = v.len;
        }
    }
    REQUIRE(good != nullptr);

    // Baseline: the untouched vector bytes decode cleanly (proves the corrupted case below
    // is discriminating against a real CRC check, not against some unrelated parse failure).
    const std::string expect_ok = "OK frame dst=0x01 src=0x00 flags=0x00 seq=0 payload=0101000000";
    REQUIRE(omgp::canon::fdec_line(good, good_len) == expect_ok);

    // Flip one bit of the CRC's low byte (second-to-last byte, before the closing FLAG).
    std::vector<uint8_t> corrupt(good, good + good_len);
    REQUIRE(good_len >= 2);
    corrupt[corrupt.size() - 2] ^= 0x01;
    REQUIRE(omgp::canon::fdec_line(corrupt.data(), corrupt.size()) == "ERR BadCrc");
}

TEST_CASE("fdec_line names every Discard reason with the contract's exact spelling", "[frame]") {
    // Runt frame: fewer than kHeaderLen+kCrcLen bytes between FLAGs.
    const uint8_t runt[] = {0x7e, 0x01, 0x02, 0x03, 0x7e};
    REQUIRE(omgp::canon::fdec_line(runt, sizeof runt) == "ERR BadLength");

    // Invalid escape continuation byte.
    const uint8_t bad_escape[] = {0x7e, 0x04, 0x00, 0x20, 0x01, 0x7d, 0x00, 0xa4, 0xb2, 0x7e};
    REQUIRE(omgp::canon::fdec_line(bad_escape, sizeof bad_escape) == "ERR BadEscape");

    // 71-byte unstuffed body: one past kMaxUnstuffed.
    STATIC_REQUIRE(omgp::LIMIT_max_l3_payload == 64);
    uint8_t too_long[1 + 71];
    too_long[0] = 0x7e;
    for (int i = 0; i < 71; ++i)
        too_long[1 + i] = 0x01;
    REQUIRE(omgp::canon::fdec_line(too_long, sizeof too_long) == "ERR TooLong");

    // dst == 0xFF with a correct CRC: reserved broadcast, discarded on decode too.
    const uint8_t reserved[] = {0x7e, 0xff, 0x00, 0x00, 0x00, 0x63, 0xcf, 0x7e};
    REQUIRE(omgp::canon::fdec_line(reserved, sizeof reserved) == "ERR ReservedAddress");
}

// --- fstream_lines (FSTREAM) ------------------------------------------------------------------

TEST_CASE("fstream_lines: a FLAG-only burst between two frames is END 0", "[frame]") {
    const uint8_t* ping = nullptr;
    uint16_t ping_len = 0;
    const uint8_t* resp = nullptr;
    uint16_t resp_len = 0;
    for (size_t i = 0; i < omgp::vectors::COUNT; ++i) {
        const auto& v = omgp::vectors::ALL[i];
        if (std::string(v.name) == "frame_ping_req") {
            ping = v.bytes;
            ping_len = v.len;
        } else if (std::string(v.name) == "frame_response") {
            resp = v.bytes;
            resp_len = v.len;
        }
    }
    REQUIRE(ping != nullptr);
    REQUIRE(resp != nullptr);

    // A "garbage burst" of bare FLAG bytes between two frames produces only empty (n==0)
    // frames, which on_flag() discards silently without touching any Discard counter
    // (link/frame.cpp) — the discriminating case the contract's END 0 example describes,
    // as opposed to a burst of non-FLAG bytes (which would count as one discarded frame).
    std::vector<uint8_t> stream(ping, ping + ping_len);
    stream.push_back(omgp::TRUNK_flag_byte);
    stream.push_back(omgp::TRUNK_flag_byte);
    stream.push_back(omgp::TRUNK_flag_byte);
    stream.insert(stream.end(), resp, resp + resp_len);

    const std::string out = omgp::canon::fstream_lines(stream.data(), stream.size());
    const std::string expect = "OK frame dst=0x01 src=0x00 flags=0x00 seq=0 payload=0101000000\n"
                               "OK frame dst=0x00 src=0x01 flags=0x01 seq=5 payload=0101000100\n"
                               "END 0";
    REQUIRE(out == expect);
}

TEST_CASE("fstream_lines counts a non-FLAG garbage burst as one discarded frame", "[frame]") {
    const uint8_t* ping = nullptr;
    uint16_t ping_len = 0;
    for (size_t i = 0; i < omgp::vectors::COUNT; ++i) {
        const auto& v = omgp::vectors::ALL[i];
        if (std::string(v.name) == "frame_ping_req") {
            ping = v.bytes;
            ping_len = v.len;
        }
    }
    REQUIRE(ping != nullptr);

    std::vector<uint8_t> stream(ping, ping + ping_len);
    const std::vector<uint8_t> garbage = {0x11, 0x22, 0x33}; // never a valid header+CRC
    stream.insert(stream.end(), garbage.begin(), garbage.end());
    // the 2nd frame's opening FLAG ends the garbage run
    stream.insert(stream.end(), ping, ping + ping_len);

    const std::string out = omgp::canon::fstream_lines(stream.data(), stream.size());
    const std::string expect = "OK frame dst=0x01 src=0x00 flags=0x00 seq=0 payload=0101000000\n"
                               "OK frame dst=0x01 src=0x00 flags=0x00 seq=0 payload=0101000000\n"
                               "END 1";
    REQUIRE(out == expect);
}

// --- fenc_response / fstream_response: the l3_helper FENC/FSTREAM verb bodies ---------------
// contracts/frame-vectors.md "l3_helper verbs": FENC succeeds with "OK <hex wire bytes>" (like
// FDEC/FSTREAM's own "OK "/"END " framing), never bare hex; FSTREAM always terminates in an
// END line, even when the request itself is malformed, so a "read lines until END" driver can
// never block on it.

TEST_CASE("fenc_response OK-prefixes the hex wire bytes, matching every frame_* vector",
          "[frame]") {
    size_t seen = 0;
    for (size_t i = 0; i < omgp::vectors::COUNT; ++i) {
        const auto& v = omgp::vectors::ALL[i];
        if (std::string(v.kind) != "frame")
            continue;
        ++seen;
        INFO(v.name << " (" << v.spec_ref << ")");
        REQUIRE(omgp::canon::fenc_response(v.canonical) ==
                "OK " + omgp::canon::hex_lower(v.bytes, v.len));
    }
    REQUIRE(seen == 5);
}

TEST_CASE("fenc_response surfaces a refusal unprefixed, same spelling as encode_frame_line",
          "[frame]") {
    // dst == 0xFF: reserved broadcast address (trunk §5); no "OK " on a refusal.
    REQUIRE(omgp::canon::fenc_response("frame dst=0xFF src=0x00 flags=0x00 seq=0 payload=") ==
            "ERR ReservedAddress");
    REQUIRE(omgp::canon::fenc_response("not-a-frame") == "ERR BadRequest");
}

TEST_CASE("fstream_response terminates a malformed hex request with END 0, not a bare ERR",
          "[frame]") {
    // "zz" is never valid hex: parse_hex fails before fstream_lines ever runs. A driver reading
    // lines until END must see one here too, or it blocks waiting for a terminator that never
    // arrives (the hazard this test pins).
    REQUIRE(omgp::canon::fstream_response("zz") == "ERR BadRequest\nEND 0");
}

TEST_CASE("fstream_response matches fstream_lines on well-formed hex", "[frame]") {
    const uint8_t* ping = nullptr;
    uint16_t ping_len = 0;
    for (size_t i = 0; i < omgp::vectors::COUNT; ++i) {
        const auto& v = omgp::vectors::ALL[i];
        if (std::string(v.name) == "frame_ping_req") {
            ping = v.bytes;
            ping_len = v.len;
        }
    }
    REQUIRE(ping != nullptr);
    REQUIRE(omgp::canon::fstream_response(omgp::canon::hex_lower(ping, ping_len)) ==
            omgp::canon::fstream_lines(ping, ping_len));
}
