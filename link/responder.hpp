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
// of `resp` (capacity `cap`) it wrote. Called at most once per sequence carrying the
// retry bit SET that collides with an already-buffered response (trunk §7: such a retry
// is replayed from the buffer, never re-invoking this) -- NOT a guarantee against a
// request duplicated on the wire with the retry bit CLEAR (e.g. a reflection or repeat):
// that repeats the "new" path and re-invokes this a second time. Open spec question
// (docs/OPEN-QUESTIONS.md 2026-09-06, SC-004 vs this retry-bit-gated condition).
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
    // data-model.md §5's three states. Transmitting(until tx_end) matters as soon as more
    // than one request can be queued ahead of a single poll() call: without it, a second
    // wire_.transmit() could start before the first has left the (half-duplex, trunk §3)
    // wire — review + red-team @033182a finding 1. poll() drains the wire only while
    // Listening; in the other two states arrived bytes wait in the wire's receive queue
    // (red team @e510b29 finding 1).
    enum class State : uint8_t { Listening, Scheduled, Transmitting };

    // data-model.md §5: single-frame replay buffer, sized to the codec's own worst-case
    // stuffed-frame bound (kMaxWire).
    struct ReplayBuffer {
        bool valid = false;
        uint8_t seq = 0;
        // The requester (f.src) this response was encoded for: a retry with a colliding
        // sequence from a DIFFERENT station must not replay it back to the wrong address
        // (red-team @033182a finding 3; docs/OPEN-QUESTIONS.md 2026-09-06).
        uint8_t peer = 0;
        uint16_t len = 0;
        uint8_t bytes[kMaxWire] = {};
    };

    // Requests decoded while a late response was still waiting for an idle bus, kept
    // unanswered until the wire is free. kHeldRequests of them, and then the drain STOPS:
    // everything behind stays in the wire's own receive queue, exactly as it did before any
    // of this existed, so THIS ENGINE never destroys or drops a request (FR-015/FR-016; the
    // property test_link_responder.cpp's "eight well-spaced requests ... none discarded"
    // pins, itself the fix for red team @e510b29).
    //
    // That guarantee stops at the engine's edge, and claiming otherwise would be false (red
    // team @5830fc4 finding 2): it holds because MockWire's receive queue is 4 * kMaxWire.
    // The pinned target's UART RX FIFO is 128 bytes -- smaller than kMaxWire (142) -- and the
    // blind window while stopped is up to max_frame + T_gap of arrivals, so on an ESP32-S3 a
    // long enough stop overflows that FIFO and the HARDWARE drops bytes this engine believes
    // are safely queued. docs/OPEN-QUESTIONS.md 2026-09-07 records it. It is a firmware-side
    // bound (poll cadence, or a driver ring larger than kMaxWire), not something this file
    // can fix.
    //
    // Stopping means the engine has left bytes unread, so from that instant its picture of
    // the bus is INCOMPLETE -- and it says so (poll()'s belief_stale) rather than judging
    // T_gap against a reading it knows to be partial. A stale belief falls back to the
    // bounded cap, which BOUNDS the wait but does NOT make the transmit safe: the cap fires
    // against a bus the engine has not read since it stopped, and red team @6440074 finding
    // 1 demonstrates a frame being transmitted over there. See transmit_if_due() for why
    // Master's cap argument does not carry across, and the PR / docs/OPEN-QUESTIONS.md
    // 2026-09-07 for the open ruling on which corner of that trade to take.
    //
    // The alternative, stashing raw bytes to re-feed later, was tried and abandoned across
    // red team @b262d46 / @2efcb67 / @7a80ec3: any fixed buffer has a capacity at which the
    // engine must either destroy a byte or stop reading, and each revision moved that
    // boundary by one byte rather than closing it. One decoded request has no such capacity:
    // it is always holdable.
    struct HeldRequest {
        FrameFields f = {};
        uint8_t payload[omgp::LIMIT_max_l3_payload] = {};
        uint64_t request_end_us = 0;
    };

    // How many completed requests one wait may absorb before the engine stops reading. TWO,
    // and the second is not arbitrary: with one, the engine stopped as soon as anything
    // completed -- including when the wire was already empty behind it -- so the ordinary
    // "two requests queued ahead of one late poll" case fell back to the cap and every
    // answer in it was delayed by ~1.47 ms (measured against this suite's own :536 and :561
    // cases). Two lets the engine reach the end of the queue in that case and see for itself
    // that the bus is quiet. Past two it stops, holds what it has, and says its reading is
    // partial -- and two completed frames during one wait is itself evidence the bus was
    // busy, so the pessimism is then earned rather than assumed.
    static constexpr size_t kHeldRequests = 2;

    // Decides new-vs-replay for one intact frame addressed to my_addr, or counts a
    // discard: for a frame this node is not addressed by or could never answer — a
    // different dst, a RESPONSE-bit frame, a src equal to my_addr, or a src outside trunk
    // §5's L2 address range (see the .cpp). Only ever called while Listening — poll()
    // either stops draining or holds what it decodes otherwise — so a pending response is
    // never overwritten. Schedules an accepted request's response at request_end_us +
    // turnaround_us_ (data-model.md §5).
    void on_request(const FrameFields& f, uint64_t request_end_us);

    // data-model.md §5 "Request acceptance", plus the two address bounds this engine owes
    // trunk §5: the claimed source of an accepted request, and this node's own address.
    bool acceptable(const FrameFields& f) const;

    // One intact frame decoded while a response was pending: appended to held_ if this node
    // owes an answer to it, discarded and counted if it is not this node's. Never called
    // with held_ already full -- poll() stops draining at that point.
    void hold_or_discard(const FrameFields& f, uint64_t request_end_us);

    // True while a scheduled response is already outside trunk §9's turnaround window, the
    // FR-014 late-poll path: the one state in which the engine must judge the bus for
    // itself before transmitting (trunk §3; see transmit_if_due).
    bool past_window(uint64_t now_us) const;

    // kMaxWire byte times at the wire's current rate — the bound on how long the engine
    // waits for an idle bus (Master::max_frame_us(), link/master.cpp:173).
    uint64_t max_frame_us() const;

    // Transmits the Scheduled response and moves to Transmitting once `now_us` has
    // reached its deadline; moves Transmitting back to Listening once `now_us` has
    // reached the tx_end wire_.transmit() itself returned. Called at the top of every
    // iteration of poll()'s drain loop — ahead of every byte, and once more after the last
    // — so a response already due is flushed, and the wire found free again, before the
    // next queued byte is drained (see poll()'s own comment).
    // queue_drained: poll()'s drain loop has stopped, so the late path may now be judged --
    // never at the top of an iteration with bytes still unread behind it, which is how the
    // engine used to key down inside another station's frame.
    // belief_stale: it stopped because a request is held, not because the wire went quiet,
    // so bytes may remain unread and last_activity_us_ is a partial reading. The two exits
    // mean opposite things and poll() reports which it took (red team @2efcb67).
    void transmit_if_due(uint64_t now_us, bool queue_drained, bool belief_stale);

    ByteWire& wire_;
    // Stored for the constructor-signature parity with Master/Health (link-cpp.md
    // "Responder engine") and for future use (a scheduled deadline check keyed off the
    // engine's own clock rather than the now_us poll() already takes). poll() and
    // on_request() take `now_us` explicitly, so clock_ itself is not yet read; the
    // constructor body performs one discarded read, which silences clang's
    // -Wunused-private-field (fuzz preset) without [[maybe_unused]] — some gcc versions
    // reject that attribute on a data member under -Werror=attributes (see health.hpp).
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
    // Set to wire_.transmit()'s own return value once State::Transmitting is entered: the
    // instant the wire is free again (data-model.md §5 "until tx_end").
    uint64_t transmit_until_us_ = 0;

    // The END of the last byte drained from the wire, and whether any has been (Master's
    // last_activity_/has_last_activity_ pair, link/master.cpp): the engine's evidence that
    // the bus is busy, used only on the late path where trunk §3's window guarantee no
    // longer covers it.
    uint64_t last_activity_us_ = 0;
    bool has_activity_ = false;
    // The instant a late response first found the bus busy — the origin of the bounded
    // wait (defer_origin + max_frame + T_gap; Master's defer_origin_us_). Cleared on every
    // transmit; the flag distinguishes "not deferring" from a legitimate origin of 0.
    uint64_t defer_origin_us_ = 0;
    bool has_defer_origin_ = false;

    // A ring, not a shifted array. The shift it replaces was two mutants deep-verify could
    // not kill (`i < held_count_` -> `<=`, `++i` -> `--i`): the slot a mis-shift corrupts is
    // never read afterwards, so only ASan noticed, and the mutation build has no ASan. Here
    // every index arithmetic error hands on_request() the WRONG held request, which the
    // arrival-order assertions catch (red team @6440074's four-request case).
    HeldRequest held_[kHeldRequests] = {};
    size_t held_head_ = 0;
    size_t held_count_ = 0;

    AddrStats stats_ = {};
};

} // namespace link
} // namespace omgp
