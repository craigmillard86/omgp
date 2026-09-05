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
    // True while a frame accumulation is actually in progress: a FLAG has opened one AND at
    // least one byte has since been taken into it (len_ > 0), or an escape is pending
    // (state_ == Escaped). on_flag() (frame.cpp) sets state_ = InFrame on EVERY FLAG —
    // including the FLAG that CLOSES a frame, because that same FLAG is also the next frame's
    // opening delimiter (trunk §4) — and resets len_ to 0. So `state_ != Hunting` ALONE
    // stayed true after any frame ever seen, with nothing actually accumulating. The len_/
    // Escaped test distinguishes "a response is genuinely still arriving" (finish it: trunk
    // §3, the Master's T_resp timeout gates the START BIT, not full delivery) from "a FLAG
    // closed the last frame and the wire went quiet" (which must NOT hold the timeout off).
    // PR #137 review/red-team, HIGH: an in-window frame that was merely discarded left
    // state_ == InFrame with len_ == 0, so the old predicate wedged the Master's timeout
    // forever — one stray frame from any node stopped the host trunk permanently.
    bool in_frame() const {
        return state_ != State::Hunting && (len_ > 0 || state_ == State::Escaped);
    }

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
