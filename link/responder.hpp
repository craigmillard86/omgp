// OMGP trunk L2 — Responder engine: trunk §3 (media access), §7 (retry rule).
// Contract: specs/002-trunk-link-layer/contracts/link-cpp.md "Responder engine"; data
// model: specs/002-trunk-link-layer/data-model.md §5 "Responder". Makes
// tests/unit/test_link_responder.cpp (T033) and tests/unit/test_link_loop.cpp (T034)
// pass. Embedded path: C++17, no exceptions, no RTTI, no heap — one buffered response
// plus fixed statistics.
#pragma once

#include "link/byte_wire.hpp"
#include "link/clock.hpp"
#include "link/frame.hpp"
#include "link/link_types.hpp"

namespace omgp {
namespace link {

// contracts/link-cpp.md "Responder engine": the node-side callback that turns a decoded
// request payload into a response payload. `handle` is invoked at most once per genuinely
// new sequence (data-model.md §5) — a retry of the already-answered sequence is served
// from the Responder's own replay buffer instead.
struct RequestHandler {
    virtual size_t handle(const uint8_t* req, size_t len, uint8_t* resp, size_t cap) = 0;

  protected:
    ~RequestHandler() = default;
};

// Answers requests addressed to `my_addr` (trunk §3: the node side of one request/response
// transaction), replaying the last response verbatim on a matching retry rather than
// invoking `handler` again (trunk §7's retry rule is idempotent at L2: a retried request
// must never be double-processed). One outstanding response at a time, mirroring the
// trunk's own strict request/response cadence — a real node never answers two requests at
// once (data-model.md §5).
class Responder {
  public:
    // turnaround_us is clamped to [TRUNK_T_turn_min_us, TRUNK_T_turn_max_us] here, at
    // construction (contracts/link-cpp.md): no later call can put the engine outside the
    // range the trunk timing model requires.
    Responder(ByteWire& wire, Clock& clock, RequestHandler& handler, uint8_t my_addr,
              uint32_t turnaround_us = TRUNK_T_turn_min_us);

    // The only receive path (analysis F1, mirrored from Master): drains ByteWire::receive()
    // into the engine's own Deframer, handles or replays any accepted request, and
    // transmits a scheduled response once its instant is reached. No public feed().
    //
    // FR-014 "late poll": if the first poll() call at or after the response's own deadline
    // finds `now_us` already past `request_end + TRUNK_T_turn_max_us`, the response is
    // transmitted immediately (at `now_us`, not at the missed deadline) and
    // `stats().late_responses` is incremented — nothing is ever dropped, only delayed.
    void poll(uint64_t now_us);

    const AddrStats& stats() const;

  private:
    void handle_request(const FrameFields& f, uint64_t request_end_us);
    void transmit_response(uint64_t at_us);

    ByteWire& wire_;
    Clock& clock_;
    RequestHandler& handler_;
    uint8_t my_addr_;
    uint32_t turnaround_us_;

    Deframer deframer_;

    // ReplayBuffer (data-model.md §5): the last response sent, kept byte-for-byte so a
    // matching retry retransmits identically rather than re-deriving it from the request.
    bool buffer_valid_ = false;
    uint8_t buffer_seq_ = 0;
    uint8_t buffer_[kMaxWire] = {};
    size_t buffer_len_ = 0;

    // Scheduled(response at request_end + turnaround_us) while true; Listening otherwise
    // (data-model.md §5's state enum collapses to this one flag plus deadline_/
    // request_end_us_, since Transmitting is instantaneous — transmit_response() both
    // encodes onto the wire and clears the schedule in one call, as Master::do_transmit does).
    bool scheduled_ = false;
    uint64_t deadline_ = 0;       // request_end_us_ + turnaround_us_
    uint64_t request_end_us_ = 0; // instant of the request's own last byte

    AddrStats stats_ = {};
};

} // namespace link
} // namespace omgp
