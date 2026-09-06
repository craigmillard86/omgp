// OMGP trunk L2 — Responder transaction engine: trunk §3 (media access — response
// scheduling inside the turnaround window), §7 (retries, replay). Contract:
// specs/002-trunk-link-layer/contracts/link-cpp.md "Responder engine"; data model:
// specs/002-trunk-link-layer/data-model.md §5 "Responder". Makes
// tests/unit/test_link_responder.cpp (T033) pass. Embedded path: C++17, no exceptions,
// no RTTI, no heap — one single-frame replay buffer plus fixed statistics.
#pragma once

#include "link/byte_wire.hpp"
#include "link/clock.hpp"
#include "link/frame.hpp"
#include "link/link_types.hpp"

namespace omgp {
namespace link {

// The application-facing side of a node: answers a request and returns how many bytes
// of `resp` (capacity `cap`) it wrote. Called at most once per NEW sequence (trunk §7:
// a retried sequence is replayed from the buffer, never re-invoking this).
struct RequestHandler {
    virtual size_t handle(const uint8_t* req, size_t len, uint8_t* resp, size_t cap) = 0;

  protected:
    ~RequestHandler() = default;
};

// Drives one node's side of a request/response transaction over a ByteWire (trunk §3):
// schedules the response inside [T_turn_min, T_turn_max] of the request's end, and
// replays an already-answered sequence byte-for-byte instead of re-invoking the
// handler (trunk §7; data-model.md §5).
class Responder {
  public:
    // turnaround_us is clamped into [TRUNK_T_turn_min_us, TRUNK_T_turn_max_us] at
    // construction (data-model.md §5): trunk §9 already fixes both bounds, so a value
    // outside that range is pulled to the nearer bound rather than refused.
    Responder(ByteWire& wire, Clock& clock, RequestHandler& handler, uint8_t my_addr,
              uint32_t turnaround_us = omgp::TRUNK_T_turn_min_us);

    // The only receive path (mirrors Master::poll, analysis F1): drains
    // ByteWire::receive() into the engine's own Deframer, decides new-vs-replay for any
    // intact request addressed to my_addr, and transmits a scheduled response once
    // now_us reaches its deadline. A response still due when now_us is already past
    // request_end + TRUNK_T_turn_max_us (late poll) is transmitted at once rather than
    // dropped, and counted in stats().late_responses (spec FR-014).
    void poll(uint64_t now_us);

    // replays_served, discards, transactions (requests handled), late_responses
    // (contracts/link-cpp.md "Responder engine"); retries/timeouts/crc_failures are
    // Master-only fields of this shared AddrStats type and stay zero here.
    const AddrStats& stats() const;

  private:
    enum class State : uint8_t { Listening, Scheduled };

    // data-model.md §5: single-frame replay buffer, sized to the codec's own worst-case
    // stuffed-frame bound (kMaxWire).
    struct ReplayBuffer {
        bool valid = false;
        uint8_t seq = 0;
        uint16_t len = 0;
        uint8_t bytes[kMaxWire] = {};
    };

    // Decides new-vs-replay for one intact frame (or counts a discard for anything not
    // addressed to my_addr / not a request) and schedules the response at
    // request_end_us + turnaround_us_ (data-model.md §5).
    void on_request(const FrameFields& f, uint64_t request_end_us);

    ByteWire& wire_;
    Clock& clock_;
    RequestHandler& handler_;
    uint8_t my_addr_;
    uint32_t turnaround_us_;

    Deframer deframer_;
    ReplayBuffer buffer_;

    State state_ = State::Listening;
    // The request the Scheduled response answers: its END instant (the late-poll bound
    // is request_end_us_ + TRUNK_T_turn_max_us, FR-014) and the transmit deadline
    // (request_end_us_ + turnaround_us_).
    uint64_t request_end_us_ = 0;
    uint64_t deadline_us_ = 0;

    AddrStats stats_ = {};
};

} // namespace link
} // namespace omgp
