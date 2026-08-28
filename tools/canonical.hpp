// Host-only canonical-text codec for L3 messages (C++ side of the differential test).
// Format: specs/001-protocol-foundation/contracts/canonical-text.md. Renders identical
// strings to tools/refimpl/canonical.py for identical values — string equality is the
// semantic-identity test. Uses the full language (std::string/vector): tools/, not l3/.
#pragma once

#include "l3/l3_types.hpp"

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

} // namespace canon
} // namespace omgp
