// Verb table for tools/l3_helper.cpp's stdin/stdout loop, split out of main() so the
// dispatch itself — which verb string reaches which canonical.hpp function — is unit
// testable without spawning the built binary. QUIT is handled by the caller: it is a
// control-flow verb (exit the loop), not a line that produces a response.
#pragma once

#include <string>

namespace omgp {
namespace canon {

// Routes one "<VERB> <arg>" line to its handler and returns the response line(s) (FSTREAM's
// response embeds internal '\n's; the caller appends the final line terminator). An unknown
// verb, including "QUIT" (the caller's job to intercept first), returns "ERR BadRequest".
std::string dispatch_line(const std::string& line);

} // namespace canon
} // namespace omgp
