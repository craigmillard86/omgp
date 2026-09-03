// Host-only canonical-text codec for L3 messages (C++ side of the differential test).
// Format: specs/001-protocol-foundation/contracts/canonical-text.md. Renders identical
// strings to tools/refimpl/canonical.py for identical values — string equality is the
// semantic-identity test. Uses the full language (std::string/vector): tools/, not l3/.
#pragma once

#include "l3/l3_types.hpp"
#include "link/link_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace omgp {
namespace canon {

// Decode a whole wire message (header + payload) and render it; "ERR <Status>" on failure.
std::string render_message(const uint8_t* data, size_t len);

// Parse canonical text and encode header + payload. On failure returns false and sets
// error to "ERR <Status>" (codec rejection) or "ERR BadRequest" (malformed text).
bool encode_message(const std::string& canonical, std::vector<uint8_t>& out, std::string& error);

// §3.3 status block on its own (kind "status" vectors).
std::string render_status(const uint8_t* data, size_t len);
bool encode_status(const std::string& canonical, std::vector<uint8_t>& out, std::string& error);

std::string status_line(omgp::l3::Status s); // "ERR <name>"
std::string hex_lower(const uint8_t* data, size_t len);
bool parse_hex(const std::string& text, std::vector<uint8_t>& out); // spaces allowed, any case

// Descriptors (§4): records joined by " | "; strings quoted with \" \\ \xNN escapes.
std::string render_descriptor(const uint8_t* blob, size_t len); // canonical | "ERR <Status>"
bool encode_descriptor(const std::string& canonical, std::vector<uint8_t>& out, std::string& error);
// DVAL line: "OK skipped=<n> channels=<n> params=<n>" | "ERR <Status> type=0x.. offset=<n>"
std::string validate_line(const uint8_t* blob, size_t len);
std::string quote_str(const uint8_t* data, size_t len);
bool unquote_str(const std::string& quoted, std::vector<uint8_t>& out);

// Frames (specs/002-trunk-link-layer/contracts/frame-vectors.md "Canonical frame line").
// Renders the unstuffed fields, never the stuffed wire bytes (the caller's own
// encode_frame/Deframer produce those). "frame dst=0x.. src=0x.. flags=0x.. seq=N payload=hex".
std::string render_frame(const omgp::link::FrameFields& f);
// Parses that exact line grammar. On success `out.payload` points into `payload_storage`,
// which the caller must keep alive as long as `out` is used. Rejects a missing field, a seq
// outside 0-15, and odd-length/non-hex/oversized payload without ever building a FrameFields
// for the codec to see; sets error to "ERR BadRequest".
bool parse_frame_line(const std::string& canonical, omgp::link::FrameFields& out,
                      std::vector<uint8_t>& payload_storage, std::string& error);
std::string discard_line(omgp::link::Discard d);    // "ERR <Discard>" (Deframer discard)
std::string link_status_line(omgp::link::Status s); // "ERR <Status>" (encode_frame refusal)

// l3_helper frame verbs (contracts/frame-vectors.md "l3_helper verbs"). The whole verb body
// lives here, matching encode_message/render_message/encode_descriptor's split — l3_helper.cpp
// stays a thin per-verb dispatcher that only does hex<->bytes conversion.
// FENC: parses + encodes in one step; false on any refusal (malformed text or codec status).
bool encode_frame_line(const std::string& canonical, std::vector<uint8_t>& out, std::string& error);
// FDEC: feeds `data` through a fresh Deframer. "OK <frame line>" on delivery, "ERR <Discard>"
// naming the first discard reason, or "ERR BadRequest" if the bytes run out with neither.
std::string fdec_line(const uint8_t* data, size_t len);
// FSTREAM: one "OK <frame line>" per delivered frame (in arrival order, newline-joined),
// then "END <n>" where n is the total discarded (malformed, FLAG-delimited) frame count.
std::string fstream_lines(const uint8_t* data, size_t len);

} // namespace canon
} // namespace omgp
