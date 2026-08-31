// OMGP trunk L2 — frame codec implementation: trunk §4, data-model.md §2/§3. Ported
// 1:1 from the state machine `tools/refimpl/omgp_link.py` pins (research.md R-03; Q1
// ruling, docs/OPEN-QUESTIONS.md 2026-08-29: a single invalid escape aborts the frame at
// once, no tolerance for partial stuffing violations before the trunk doc's "≥ 8" bound).
#include "link/frame.hpp"

#include "link/crc16.hpp"
#include "omgp_protocol.h"

namespace omgp {
namespace link {

Status encode_frame(const FrameFields& f, uint8_t* out, size_t cap, size_t& written) {
    written = 0;
    if (f.len > omgp::LIMIT_max_l3_payload)
        return Status::PayloadTooLong;
    if (f.dst == 0xFF) // literal-ok: trunk §5 reserved broadcast address, not an L3 event code
        return Status::ReservedAddress;
    // Worst-case bound on the stuffed length for this `len`: every unstuffed byte escapes,
    // plus the two FLAGs. Checked before any byte is written (FR-005), independent of the
    // achieved (possibly smaller) length once the actual bytes are known.
    const size_t needed = 2 + 2 * (kHeaderLen + static_cast<size_t>(f.len) + kCrcLen);
    if (cap < needed)
        return Status::BufferTooSmall;

    uint8_t unstuffed[kMaxUnstuffed];
    size_t n = 0;
    unstuffed[n++] = f.dst;
    unstuffed[n++] = f.src;
    unstuffed[n++] = static_cast<uint8_t>((f.response ? 0x01 : 0x00) | (f.retry ? 0x02 : 0x00) |
                                          static_cast<uint8_t>((f.seq & 0x0F) << 4));
    unstuffed[n++] = f.len;
    for (uint8_t i = 0; i < f.len; ++i)
        unstuffed[n++] = f.payload[i];
    const uint16_t c = crc16_ccitt_false(unstuffed, n);
    unstuffed[n++] = static_cast<uint8_t>(c & 0xFF); // literal-ok: CRC low-byte mask, not an opcode
    unstuffed[n++] = static_cast<uint8_t>((c >> 8) & 0xFF); // literal-ok: CRC high-byte mask

    size_t w = 0;
    out[w++] = omgp::TRUNK_flag_byte;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = unstuffed[i];
        if (b == omgp::TRUNK_flag_byte) {
            out[w++] = omgp::TRUNK_escape_byte;
            out[w++] = static_cast<uint8_t>(omgp::TRUNK_flag_byte ^ omgp::TRUNK_escape_xor);
        } else if (b == omgp::TRUNK_escape_byte) {
            out[w++] = omgp::TRUNK_escape_byte;
            out[w++] = static_cast<uint8_t>(omgp::TRUNK_escape_byte ^ omgp::TRUNK_escape_xor);
        } else {
            out[w++] = b;
        }
    }
    out[w++] = omgp::TRUNK_flag_byte;
    written = w;
    return Status::Ok;
}

Deframer::Deframer() : state_(State::Hunting), len_(0), stats_{} {}

void Deframer::reset() {
    state_ = State::Hunting;
    len_ = 0;
    // counters kept (contract: "reset(); // back to Hunting; counters kept")
}

const DeframerStats& Deframer::stats() const {
    return stats_;
}

void Deframer::append(uint8_t byte) {
    // A 71st unstuffed byte aborts the frame at once (Q1: no tolerance for partial
    // violations before the "≥ 8" bound the trunk doc allows for babble).
    if (len_ >= kMaxUnstuffed) {
        stats_.discarded[static_cast<size_t>(Discard::TooLong)]++;
        state_ = State::Hunting;
        len_ = 0;
        return;
    }
    buf_[len_++] = byte;
}

bool Deframer::on_escaped_byte(uint8_t byte, FrameView& out) {
    (void)out;
    if (byte == static_cast<uint8_t>(omgp::TRUNK_flag_byte ^ omgp::TRUNK_escape_xor)) {
        append(omgp::TRUNK_flag_byte);
    } else if (byte == static_cast<uint8_t>(omgp::TRUNK_escape_byte ^ omgp::TRUNK_escape_xor)) {
        append(omgp::TRUNK_escape_byte);
    } else {
        stats_.discarded[static_cast<size_t>(Discard::BadEscape)]++;
        state_ = State::Hunting;
        len_ = 0;
        return false;
    }
    // append() may have just aborted to Hunting (TooLong); that must win over the
    // Escaped->InFrame transition, or later bytes would be treated as frame content with
    // no FLAG ever having opened a frame (trunk §4: resynchronise on the NEXT FLAG).
    if (state_ != State::Hunting)
        state_ = State::InFrame;
    return false;
}

bool Deframer::on_flag(FrameView& out) {
    // An escape byte as the last byte before a FLAG never gets its second byte.
    if (state_ == State::Escaped) {
        stats_.discarded[static_cast<size_t>(Discard::BadEscape)]++;
        state_ = State::InFrame;
        len_ = 0;
        return false;
    }
    // Hunting or InFrame: this FLAG opens the next frame either way (shared delimiter);
    // when Hunting, len_ is already 0 so the frame is empty (n == 0).
    const size_t n = len_;
    len_ = 0;
    state_ = State::InFrame;
    if (n == 0)
        return false; // empty frame / shared delimiter: discarded silently
    if (n < kHeaderLen + kCrcLen) {
        stats_.discarded[static_cast<size_t>(Discard::BadLength)]++;
        return false;
    }
    const uint8_t dst = buf_[0], src = buf_[1], ctrl = buf_[2], length = buf_[3];
    if (length != n - (kHeaderLen + kCrcLen)) {
        stats_.discarded[static_cast<size_t>(Discard::BadLength)]++;
        return false;
    }
    const uint8_t* payload = buf_ + kHeaderLen;
    const uint16_t crc_lo = buf_[n - kCrcLen];
    const uint16_t crc_hi = buf_[n - kCrcLen + 1];
    const uint16_t expected_crc = static_cast<uint16_t>(crc_lo | (crc_hi << 8));
    const uint16_t actual_crc = crc16_ccitt_false(buf_, n - kCrcLen);
    if (actual_crc != expected_crc) {
        stats_.discarded[static_cast<size_t>(Discard::BadCrc)]++;
        return false;
    }
    stats_.delivered++;
    out.f.dst = dst;
    out.f.src = src;
    out.f.response = (ctrl & 0x01) != 0;
    out.f.retry = (ctrl & 0x02) != 0;
    out.f.seq = static_cast<uint8_t>((ctrl >> 4) & 0x0F); // bits 2-3 (reserved) ignored
    out.f.len = length;
    out.f.payload = payload;
    return true;
}

bool Deframer::feed(uint8_t byte, FrameView& out) {
    if (byte == omgp::TRUNK_flag_byte)
        return on_flag(out);
    if (state_ == State::Hunting)
        return false;
    if (state_ == State::Escaped)
        return on_escaped_byte(byte, out);
    if (byte == omgp::TRUNK_escape_byte) {
        state_ = State::Escaped;
        return false;
    }
    append(byte);
    return false;
}

} // namespace link
} // namespace omgp
