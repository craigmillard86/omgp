// OMGP trunk L2 — frame codec: trunk §4 (framing), data-model.md §2/§3.
// Contract: specs/002-trunk-link-layer/contracts/link-cpp.md "Frame codec". Embedded
// path: C++17, no exceptions, no RTTI, no heap, no OS — the Deframer holds its entire
// state in one fixed-size accumulator plus counters.
#pragma once

#include "link/link_types.hpp"

namespace omgp {
namespace link {

// Encodes fields into a FLAG-delimited, byte-stuffed wire frame (trunk §4). Validates
// before writing any byte: `PayloadTooLong` if `f.len > LIMIT_max_l3_payload`;
// `ReservedAddress` if `f.dst == 0xFF` (trunk §5); `BufferTooSmall` if `cap` is less than
// the worst-case bound `2 + 2*(kHeaderLen + f.len + kCrcLen)` — a static bound on `len`,
// not the achieved (possibly smaller) stuffed length. On any refusal `written == 0` and
// `out` is untouched. Reserved ctrl bits (bits 2-3) are never set.
Status encode_frame(const FrameFields& f, uint8_t* out, size_t cap, size_t& written);

// Byte-at-a-time trunk frame parser (data-model.md §3: Hunting/InFrame/Escaped). Holds a
// single kMaxUnstuffed-byte accumulator; no input can grow it. `feed` returns true exactly
// when this byte completes and delivers a frame, in which case `out.f.payload` points into
// the Deframer's own accumulator and is valid only until the next `feed` call. Malformed
// frames are discarded silently (trunk §4) and counted in `stats()`; the next FLAG always
// opens the next frame (FR-003), including the FLAG that closed a discarded one.
class Deframer {
  public:
    Deframer();

    bool feed(uint8_t byte, FrameView& out);
    void reset(); // back to Hunting; counters kept
    const DeframerStats& stats() const;

  private:
    enum class State : uint8_t { Hunting, InFrame, Escaped };

    void append(uint8_t byte); // may abort to Hunting with Discard::TooLong
    bool on_flag(FrameView& out);
    bool on_escaped_byte(uint8_t byte, FrameView& out);

    State state_;
    uint8_t buf_[kMaxUnstuffed];
    size_t len_;
    DeframerStats stats_;
};

} // namespace link
} // namespace omgp
