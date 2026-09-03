// tools/l3_helper.cpp's verb table (tools/l3_helper_dispatch.{hpp,cpp}), driven exactly as
// l3_helper's stdin loop drives it: one "<VERB> <arg>" line in, one response line out.
// contracts/canonical-text.md, contracts/frame-vectors.md "l3_helper verbs".
//
// test_canonical_frame.cpp already proves encode_frame_line/fdec_line/fstream_lines/
// fenc_response/fstream_response are individually correct; test_l3_payload.cpp/
// test_l3_descriptor.cpp do the same for encode_message/render_message/encode_descriptor/
// render_descriptor/validate_line. None of those tests go through dispatch_line() itself, so a
// verb-string typo or a verb wired to the wrong handler in l3_helper.cpp's dispatch would ship
// green through every other test in this repo and through diffcheck (which only ever sees
// correct verb strings) — this file is the one place that would catch it, by checking each
// verb's dispatch_line() result against the same handler called directly.
#include "canonical.hpp"
#include "catch_amalgamated.hpp"
#include "l3_helper_dispatch.hpp"
#include "link/crc16.hpp"
#include "omgp_vectors.h"

#include <cstdio>
#include <string>
#include <vector>

using omgp::canon::dispatch_line;

TEST_CASE("dispatch_line: ENC/DEC round-trip a message vector, matching the handlers directly",
          "[dispatch]") {
    const omgp::vectors::Vector* v = nullptr;
    for (size_t i = 0; i < omgp::vectors::COUNT; ++i) {
        if (std::string(omgp::vectors::ALL[i].kind) == "message") {
            v = &omgp::vectors::ALL[i];
            break;
        }
    }
    REQUIRE(v != nullptr);

    std::vector<uint8_t> expect_bytes;
    std::string err;
    REQUIRE(omgp::canon::encode_message(v->canonical, expect_bytes, err));
    REQUIRE(dispatch_line(std::string("ENC ") + v->canonical) ==
            omgp::canon::hex_lower(expect_bytes.data(), expect_bytes.size()));

    const std::string expect_line = omgp::canon::render_message(v->bytes, v->len);
    REQUIRE(dispatch_line("DEC " + omgp::canon::hex_lower(v->bytes, v->len)) == expect_line);
}

TEST_CASE("dispatch_line: DENC/DDEC/DVAL route to the descriptor handlers, not the message ones",
          "[dispatch]") {
    const omgp::vectors::Vector* v = nullptr;
    for (size_t i = 0; i < omgp::vectors::COUNT; ++i) {
        if (std::string(omgp::vectors::ALL[i].kind) == "descriptor") {
            v = &omgp::vectors::ALL[i];
            break;
        }
    }
    REQUIRE(v != nullptr);

    std::vector<uint8_t> expect_bytes;
    std::string err;
    REQUIRE(omgp::canon::encode_descriptor(v->canonical, expect_bytes, err));
    REQUIRE(dispatch_line(std::string("DENC ") + v->canonical) ==
            omgp::canon::hex_lower(expect_bytes.data(), expect_bytes.size()));

    const std::string hex = omgp::canon::hex_lower(v->bytes, v->len);
    REQUIRE(dispatch_line("DDEC " + hex) == omgp::canon::render_descriptor(v->bytes, v->len));
    REQUIRE(dispatch_line("DVAL " + hex) == omgp::canon::validate_line(v->bytes, v->len));
}

TEST_CASE("dispatch_line: FENC/FDEC/FSTREAM route to the frame handlers, matching them directly",
          "[dispatch]") {
    const omgp::vectors::Vector* v = nullptr;
    for (size_t i = 0; i < omgp::vectors::COUNT; ++i) {
        if (std::string(omgp::vectors::ALL[i].kind) == "frame") {
            v = &omgp::vectors::ALL[i];
            break;
        }
    }
    REQUIRE(v != nullptr);
    const std::string hex = omgp::canon::hex_lower(v->bytes, v->len);

    REQUIRE(dispatch_line(std::string("FENC ") + v->canonical) ==
            omgp::canon::fenc_response(v->canonical));
    REQUIRE(dispatch_line("FDEC " + hex) == omgp::canon::fdec_line(v->bytes, v->len));
    REQUIRE(dispatch_line("FSTREAM " + hex) == omgp::canon::fstream_response(hex));

    // The discriminating half: prove FENC/FDEC/FSTREAM are wired to distinct handlers, not
    // three verb strings that all happen to reach the same code path. FENC takes canonical
    // text and fails on hex; FDEC/FSTREAM take hex and fail on canonical text.
    REQUIRE(dispatch_line(std::string("FENC ") + hex) == "ERR BadRequest");
    REQUIRE(dispatch_line(std::string("FDEC ") + v->canonical) == "ERR BadRequest");
    REQUIRE(dispatch_line(std::string("FSTREAM ") + v->canonical) == "ERR BadRequest\nEND 0");
}

TEST_CASE("dispatch_line: CRC matches crc16_ccitt_false rendered as 0x%04X", "[dispatch]") {
    const std::vector<uint8_t> bytes = {0x01, 0x02, 0x03, 0x04};
    char expect[8];
    std::snprintf(expect, sizeof expect, "0x%04X",
                  omgp::crc16_ccitt_false(bytes.data(), bytes.size()));
    REQUIRE(dispatch_line("CRC 01020304") == expect);
}

TEST_CASE("dispatch_line: an unrecognised verb, and QUIT (the caller's job to intercept), are "
          "both ERR BadRequest",
          "[dispatch]") {
    REQUIRE(dispatch_line("NOSUCHVERB 00") == "ERR BadRequest");
    REQUIRE(dispatch_line("QUIT") == "ERR BadRequest");
    REQUIRE(dispatch_line("") == "ERR BadRequest");
}
