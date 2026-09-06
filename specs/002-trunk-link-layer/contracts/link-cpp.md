# Contract: `omgp_link` C++ API

Library target `omgp_link` (`link/`), namespace `omgp::link` (interfaces `omgp::Clock` in
`omgp`). Embedded path: C++17, `-fno-exceptions -fno-rtti`, no heap, no OS, no time except
through `Clock`. Every header cites the trunk section it implements (`// trunk §4`).
Virtual interfaces are used for injection; nothing else is virtual.

## Types (`link_types.hpp`)

```cpp
enum class Status : uint8_t { Ok = 0, PayloadTooLong, ReservedAddress, BufferTooSmall, Busy, NotIdle };
const char* status_name(Status);

constexpr size_t kHeaderLen = 4, kCrcLen = 2;
constexpr size_t kMaxUnstuffed = kHeaderLen + LIMIT_max_l3_payload + kCrcLen;   // 70
constexpr size_t kMaxWire = 2 + 2 * kMaxUnstuffed;                               // 142
constexpr size_t kAddrCount = 16;                                                // trunk §5
constexpr uint32_t byte_time_us(uint32_t bit_rate);                              // 10 bits per byte, 8N1

struct FrameFields { uint8_t dst, src; bool response, retry; uint8_t seq; uint8_t len; const uint8_t* payload; };
struct FrameView   { FrameFields f; };            // payload points into the Deframer; valid until next feed()

enum class Discard : uint8_t { BadCrc, BadLength, BadEscape, TooLong, ReservedAddress, COUNT };
struct DeframerStats { uint32_t delivered; uint32_t discarded[static_cast<size_t>(Discard::COUNT)]; };

enum class HealthState : uint8_t { UNENROLLED, ENROLLED, SUSPECT, OFFLINE };
enum class Notice : uint8_t { ENROLLED, SUSPECT, OFFLINE, RECOVERED, BUS_FAULT, ALERT, BUS_RECOVERED };
struct AddrStats { uint32_t transactions, retries, timeouts, crc_failures, discards, replays_served, late_responses; };
struct BusStats  { uint32_t rate_changes, bus_faults; };
```

## Frame codec (`frame.hpp`) — trunk §4

```cpp
Status encode_frame(const FrameFields&, uint8_t* out, size_t cap, size_t& written);
// PayloadTooLong if len > LIMIT_max_l3_payload; ReservedAddress if dst == 0xFF (trunk §5);
// BufferTooSmall if cap < 2 + 2*(kHeaderLen + len + kCrcLen) worst case (writes nothing).
// Reserved ctrl bits are never set.

class Deframer {
public:
    Deframer();                                   // Hunting, empty accumulator
    bool feed(uint8_t byte, FrameView& out);      // true exactly when a frame is delivered by this byte
    void reset();                                 // back to Hunting; counters kept
    const DeframerStats& stats() const;
};
```
Guarantees: `feed` never reads memory other than `byte` and its own accumulator; delivered
payloads are ≤ 64 bytes; after any discard the next FLAG opens a frame; an escape byte
followed by anything other than 0x5E/0x5D discards the frame (ruling 2026-08-29); a
frame whose `dst == 0xFF` is discarded as `ReservedAddress` rather than delivered — it
could never be re-encoded, since `encode_frame` refuses that address (ruling 2026-08-31,
docs/OPEN-QUESTIONS.md).

## Byte wire and clock — see `byte-wire-and-clock.md`

## Master engine (`master.hpp`) — trunk §3, §7

```cpp
struct MasterEvent { enum Kind : uint8_t { None, Answered, Failed } kind;
                     FrameFields response;        // valid when Answered (payload into Master's buffer until next poll)
                     enum Reason : uint8_t { Timeout, CrcFailed } reason; };

class Master {
public:
    Master(ByteWire&, Clock&, uint8_t host_addr = 0x00);
    Status begin(uint8_t dst, const uint8_t* payload, size_t len);   // Busy if a transaction is open;
                                                                     // PayloadTooLong / ReservedAddress as encode_frame
    MasterEvent poll(uint64_t now_us);            // drains ByteWire::receive() into the deframer, then drives the
                                                  // state machine; at most one terminal event per transaction.
                                                  // The ONLY receive path — there is no public feed() (analysis F1).
    bool busy() const;
    uint8_t attempts() const;                     // 0..3 for the open/last transaction
    void set_bit_rate(uint32_t bps);              // pass-through to the wire + BusStats.rate_changes;
                                                  // bps == 0 refused: not forwarded, not counted (PR #137,
                                                  // pending a ruling — docs/OPEN-QUESTIONS.md 2026-09-06)
    const AddrStats& stats(uint8_t addr) const;   // per destination (kAddrCount entries)
    const BusStats& bus_stats() const;
    void reset_stats();
};
```
Behaviour (tests assert each): a new transaction uses the destination's next 4-bit
sequence; retries reuse it and set `retry`; at most `TRUNK_retries` retries; response
window `[tx_end, tx_end + TRUNK_T_resp_us)`; a CRC-failed frame in the window ends the
attempt at once; frames failing any acceptance check are discarded and counted; the next
transmission starts no earlier than `last_activity + TRUNK_T_gap_us`, where `last_activity`
is re-read on every `poll()` so a byte arriving during the deferral pushes the instant out.
That push-out is bounded: deferral for activity that is not the host's own ends at
`defer_origin + max_frame + TRUNK_T_gap_us` (`defer_origin` = the instant the transmission
was first deferred to; `max_frame` = `kMaxWire` byte times at the current rate — one
worst-case frame, trunk §4 / SC-008), and past that the host transmits on schedule (trunk §3:
the host is the only initiator, no CSMA; a station still occupying the wire is a §3
violator, and the transaction proceeds "on its own merits", spec.md Edge Cases "Babble").
`Failed` reasons remain exactly `Timeout | Crc`: the engine never concludes a transaction
from the bus state. *(Amended in PR #137 — pending a ruling, see `docs/OPEN-QUESTIONS.md`
2026-09-05 "bounded courtesy".)*

## Responder engine (`responder.hpp`) — trunk §3, §7

```cpp
struct RequestHandler { virtual size_t handle(const uint8_t* req, size_t len, uint8_t* resp, size_t cap) = 0;
                        protected: ~RequestHandler() = default; };

class Responder {
public:
    Responder(ByteWire&, Clock&, RequestHandler&, uint8_t my_addr, uint32_t turnaround_us = TRUNK_T_turn_min_us);
    // turnaround_us clamped to [TRUNK_T_turn_min_us, TRUNK_T_turn_max_us]
    void poll(uint64_t now_us);                   // drains ByteWire::receive(); transmits when the scheduled instant is
                                                  // reached. If now_us is already past request_end + T_turn_max when the
                                                  // response is due (late poll), transmits immediately and increments
                                                  // stats().late_responses (spec FR-014). No public feed().
    const AddrStats& stats() const;               // replays_served, discards, transactions (requests handled), late_responses
};
```

## Health tracker (`health.hpp`) — trunk §6, §7

```cpp
struct HealthListener { virtual void on_notice(Notice, uint8_t addr) = 0; protected: ~HealthListener() = default; };
struct Probe { uint8_t addr; uint32_t bit_rate; };

class HealthTracker {
public:
    HealthTracker(Clock&, HealthListener&);
    void on_result(uint8_t addr, bool ok, uint64_t now_us);   // one transaction outcome
    void tick(uint64_t now_us);                                // time-only transitions (SUSPECT → OFFLINE)
    HealthState state(uint8_t addr) const;
    bool poll_due(uint8_t addr, uint64_t now_us) const;        // ENROLLED: true; SUSPECT: every 10×T_poll; else false
    void mark_polled(uint8_t addr, uint64_t now_us);
    Probe next_probe(uint64_t now_us);                         // enrolment rotation; alternates rates while bus_fault()
    bool bus_fault() const;
    uint32_t bit_rate() const;                                 // rate in use after the last recovery
};
```
Rules: SUSPECT after `TRUNK_suspect_after_failures` consecutive failures; OFFLINE after
`TRUNK_offline_after_suspect_ms` in SUSPECT without a valid response; any valid response
→ ENROLLED; UNENROLLED never counts; BUS_FAULT when ≥ 1 node is enrolled and all enrolled
nodes are SUSPECT/OFFLINE (declared once); alternating-rate re-probe; first valid answer
clears the fault and pins the rate. Each transition notifies exactly once.

## What F3/F4 need (interface note, SC-010)

F3 (scheduler): `Master::begin/poll/feed`, `HealthTracker::poll_due/next_probe/on_result/
tick/mark_polled`, `HealthListener`, `Clock`. F4 (virtual rig): `ByteWire` (its virtual
wire), `Responder` + `RequestHandler` (virtual backplane), `MockWire` step kinds as the
mapping target for scenario YAML fault steps. All of it is in this contract and
`byte-wire-and-clock.md`; nothing else is required.
