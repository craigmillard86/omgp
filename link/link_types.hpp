// OMGP trunk L2 — shared value types every other link/ component builds on: trunk §4
// (frame fields, framing constants), §5 (addressing, kAddrCount), §6/§7 (health states,
// notices), §9 (bit rates, byte_time_us). Embedded path: C++17, no exceptions, no RTTI,
// no heap, no OS.
// Contract: specs/002-trunk-link-layer/contracts/link-cpp.md "Types"
#pragma once

#include "omgp_protocol.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace omgp {
namespace link {

// Result of Master/Responder/frame-codec operations. Errors by return value; Ok == 0.
// Separate vocabulary from omgp::l3::Status (data-model.md §13: deframer discards are
// silent counters, never a Status).
enum class Status : uint8_t {
    Ok = 0,
    PayloadTooLong,  // encode_frame: len > LIMIT_max_l3_payload
    ReservedAddress, // encode_frame: dst == 0xFF (trunk §5)
    BufferTooSmall,  // caller output buffer insufficient (nothing written)
    Busy,            // Master::begin: a transaction is already open
    NotIdle,         // engine operation attempted outside the state it requires
};

// Enumerator name as a string literal ("Ok", "PayloadTooLong", ...). Used by tools and
// tests; never allocates. Mirrors omgp::l3::status_name.
const char* status_name(Status s);

// trunk §4: unstuffed frame layout is dst src ctrl len payload[len] crc_lo crc_hi.
constexpr size_t kHeaderLen = 4;
constexpr size_t kCrcLen = 2;
constexpr size_t kMaxUnstuffed = kHeaderLen + LIMIT_max_l3_payload + kCrcLen; // 70
// trunk §4: worst case every unstuffed byte is escaped, plus the two FLAG bytes.
constexpr size_t kMaxWire = 2 + 2 * kMaxUnstuffed; // 142
// trunk §5: addresses 0x00 (host) through 0x0F (last backplane slot) are the trunk's
// addressable node range. Guards the YAML-editable operands (golden rule 1) so a future
// edit that breaks the ordering fails the build here, not as a wrapped-around array size
// at some unrelated AddrStats[kAddrCount] declaration.
static_assert(ADDR_backplane_max >= ADDR_host,
              "ADDR_backplane_max must be >= ADDR_host or kAddrCount wraps");
constexpr size_t kAddrCount =
    static_cast<size_t>(ADDR_backplane_max) - static_cast<size_t>(ADDR_host) + 1; // 16

// trunk §9: 10 bits per byte at 8N1 (1 start + 8 data + 1 stop); integer microseconds
// per second divided by bits/sec, times 10 bits/byte. Precondition: bit_rate != 0 (every
// caller passes a protocol-defined rate, e.g. TRUNK_bit_rate/TRUNK_bit_rate_fallback);
// division by zero is UB, so it is asserted rather than silently producing garbage.
constexpr uint32_t byte_time_us(uint32_t bit_rate) {
    assert(bit_rate != 0 && "byte_time_us: bit_rate must be nonzero");
    return 10000000u / bit_rate; // 10 bits/byte * 1e6 us/s (not a protocol-defined value)
}

// Decoded frame fields (trunk §4). ctrl byte = response<<0 | retry<<1 | (seq&0x0F)<<4.
struct FrameFields {
    uint8_t dst, src;
    bool response, retry;
    uint8_t seq;
    uint8_t len;
    const uint8_t* payload;
};

// payload in FrameFields points into the Deframer's own accumulator; valid only until
// the next feed() call.
struct FrameView {
    FrameFields f;
};

// Deframer discard reasons (data-model.md §3); counted, never returned to a caller —
// a discard is silent per trunk §4.
enum class Discard : uint8_t { BadCrc, BadLength, BadEscape, TooLong, ReservedAddress, COUNT };

struct DeframerStats {
    uint32_t delivered;
    uint32_t discarded[static_cast<size_t>(Discard::COUNT)];
};

// trunk §6/§7 node health states — the trunk document's own words, distinct from
// omgp::l3 node_states (spec.md Clarifications, 2026-08-29).
enum class HealthState : uint8_t { UNENROLLED, ENROLLED, SUSPECT, OFFLINE };

// HealthListener notification kinds (data-model.md §9); each carries an address (0 for
// bus-level notices).
enum class Notice : uint8_t {
    ENROLLED,
    SUSPECT,
    OFFLINE,
    RECOVERED,
    BUS_FAULT,
    ALERT,
    BUS_RECOVERED
};

// Per-address statistics (FR-011a): fixed size (kAddrCount entries), read-only above
// the engine that owns them, resettable via reset_stats().
struct AddrStats {
    uint32_t transactions;
    uint32_t retries;
    uint32_t timeouts;
    uint32_t crc_failures;
    uint32_t discards;
    uint32_t replays_served;
    uint32_t late_responses;
};

// Bus-wide statistics (FR-011a).
struct BusStats {
    uint32_t rate_changes;
    uint32_t bus_faults;
};

} // namespace link
} // namespace omgp
