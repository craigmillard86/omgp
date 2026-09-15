// Trunk L2 end-to-end loop: a real Master (host side) driving real Responders (node side)
// over MockWire (spec 002 US3, T034). trunk §3/§7; contracts/link-cpp.md "Master engine" /
// "Responder engine"; data-model.md §4 (Transaction) / §5 (Responder); contracts/mock-wire.md;
// spec.md User Story 2 Acceptance Scenarios 2-4 (retry on timeout, retry on CRC failure, late
// duplicate discarded) and User Story 3 Acceptance Scenarios 1-5 (replay, new-sequence
// handling); FR-008/FR-009/FR-011/FR-015/FR-016/FR-033; success criteria SC-004 and SC-005.
//
// Written ahead of link/responder.hpp/.cpp (T035): this file compiles only once T035 lands
// (tasks.md: "T034 ... make T035 pass" runs the other way — T035 makes THIS pass). Until
// then, #include "link/responder.hpp" below fails to find the header, which is the intended
// red state (tasks.md line 225: "record the failing run in the PR").
//
// -- Bridge architecture (why this is not simply MockWire+Kind::Respond, contracts/mock-wire.md
// note that "Respond" is answered by "the node's RequestHandler, usually a real Responder") --
// MockWire's own Kind::Respond/CrcError/Duplicate scheduling (tests/support/mock_wire.cpp,
// schedule_respond() et al.) always fabricates the answer itself (echoes the request's own
// payload) — it does not run a real Responder's replay buffer or invoke a counted
// RequestHandler. Proving SC-004 needs exactly that (the replay buffer is what must be shown
// to never re-invoke the handler), so this file wires up its own bidirectional bridge instead,
// built entirely from MockWire's existing PUBLIC surface (inject_bytes/transcript/advance_to) —
// no new Kind, no change to MockWire's step semantics (issue #52's own "out of scope" list):
//   - `host_wire` is the Master's ByteWire. Its per-node script is pinned to Silence forever
//     (below), so it never fabricates its own answer; every byte Master ever receives on it
//     is one this file injects by hand.
//   - `node_wire` is the (one, real) Responder's ByteWire. A request is copied onto it via
//     inject_bytes() (never via node_wire.transmit(), which would make MockWire itself try to
//     answer — this bypasses that path entirely and lands the bytes straight on node_wire's RX
//     queue, exactly where Responder::poll() drains them).
//   - The Responder is then poll()ed at request_end and again at request_end + turnaround_us
//     (its default, TRUNK_T_turn_min_us): the first drains the request (invoking the handler,
//     or replaying, per its own state); the second is when it actually transmits — captured in
//     node_wire's OWN transcript (MockWire records every transmitted frame's fields, and a
//     transmitted RESPONSE frame does not consume a script step or get auto-answered:
//     mock_wire.cpp's transmit(), "if (view.f.response) continue;").
//   - That real, captured response is then re-encoded (clean) or corrupted
//     (encode_crc_corrupted(), exposed by mock_wire.hpp/.cpp for exactly this reuse — the
//     alternative was re-deriving its FLAG/ESCAPE stuffing-boundary corruption a second time
//     while it is itself pending a ruling, docs/OPEN-QUESTIONS.md 2026-09-05 "Kind::CrcError's
//     corrupted CRC byte") and delivered to host_wire the way this cell's fault kind demands:
//     dropped (nothing injected), duplicated (injected twice), delayed past the window, or
//     injected with its CRC corrupted.
//
// -- SC-005: trunk §7 mode -> script mapping (contracts/mock-wire.md "What the tests assert
// through the mock") --
//   response timeout      -> "SC-004: drop (silence) ..." / "SC-004: delay-past-T_resp ..."
//   CRC-failed response   -> "SC-004: CRC-corrupted response ..."
//   duplicate             -> "SC-004: duplicate response ..."
//   babble                -> "extraneous bus noise (babble) between transactions is discarded"
//   SUSPECT                -> "SC-005 SUSPECT: three consecutive failed transactions drive a
//                              real HealthTracker from ENROLLED to SUSPECT" (T039/#57)
//   OFFLINE                -> "SC-005 OFFLINE: SUSPECT persists past the offline threshold via
//                              a real HealthTracker's tick(), not a fourth failed transaction"
//                              (T039/#57; RECOVERED, the reverse transition, is not a trunk §7
//                              mode this table rows on, but is proven the same way immediately
//                              below: "SC-005 RECOVERED: a Respond step after OFFLINE drives a
//                              real HealthTracker back to ENROLLED")
//   BUS_FAULT              -> "SC-005 BUS_FAULT: a node that hears only the fallback rate ..."
//                             (T042, landed via #572 rather than #60 — the wrong-rate script at
//                             the end of this file: a real Master + real HealthTracker over a
//                             node deafened by a Kind::Rate step)
//   wrong-rate probe       -> same script as BUS_FAULT above: the alternating probe at
//                             omgp::TRUNK_bit_rate_fallback is the one request the deafened
//                             node hears, and Master::set_bit_rate carries each rate onto the
//                             mock (FR-025)
//   BUS_RECOVERED          -> same script again: the reference pass that follows that answer
//                             draws nothing, so the fault clears at the fallback rate with the
//                             answering node ENROLLED (FR-026). Not a trunk §7 MODE (nor is
//                             RECOVERED above), rowed here because this table is US5's map
//                             from §7's vocabulary to the script that produces it
#include "catch_amalgamated.hpp"
#include "fake_clock.hpp"
#include "link/crc16.hpp"
#include "link/frame.hpp"
#include "link/health.hpp"
#include "link/link_types.hpp"
#include "link/master.hpp"
#include "link/responder.hpp"
#include "mock_wire.hpp"
#include "omgp_protocol.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace omgp::link;
using omgp_test::encode_crc_corrupted;
using omgp_test::FakeClock;
using omgp_test::Kind;
using omgp_test::MockWire;
using omgp_test::Step;

namespace {

// The one node this file exercises; a second could be added the same way but SC-004's
// per-attempt fault matrix needs only one to prove the property (contracts/mock-wire.md,
// data-model.md §5: the replay buffer is per node, not per bus).
constexpr uint8_t kNode = 0x01;

// Mirrors test_link_master.cpp's own helper (kept file-local, matching that file's own
// convention of a private copy rather than a shared header): the wire bytes a frame with
// these fields would produce, used to build both request and response transcripts' bytes and
// never to verify an engine's own encoder indirectly through itself.
std::vector<uint8_t> encode_expected(uint8_t f_dst, uint8_t f_src, bool response, bool retry,
                                     uint8_t seq, const uint8_t* payload, size_t len) {
    FrameFields f{f_dst, f_src, response, retry, seq, static_cast<uint8_t>(len), payload};
    uint8_t out[kMaxWire];
    size_t written = 0;
    REQUIRE(encode_frame(f, out, sizeof out, written) == Status::Ok);
    return std::vector<uint8_t>(out, out + written);
}

uint64_t byte_us() {
    return byte_time_us(omgp::TRUNK_bit_rate);
}

// contracts/link-cpp.md "Responder engine": counts invocations so SC-004's "handler
// invocations == 1 per new sequence" is directly assertable, and echoes the request payload
// so the response content is trivially predictable.
struct CountingHandler : RequestHandler {
    unsigned invocations = 0;
    // Recorded, not REQUIRE'd, here: this call runs on Responder::poll()'s stack, and
    // link/CMakeLists.txt builds omgp_link with -fno-exceptions — a REQUIRE thrown from
    // this frame would unwind through it, which is not defined behaviour (same hazard
    // mock_wire.hpp's fault_ deferral exists to avoid). Checked from the caller's own
    // stack instead (run_transaction, after the advance_to() that reaches this handler).
    bool overflowed = false;
    size_t handle(const uint8_t* req, size_t len, uint8_t* resp, size_t cap) override {
        ++invocations;
        if (len > cap) {
            overflowed = true;
            len = cap;
        }
        if (len > 0)
            std::memcpy(resp, req, len);
        return len;
    }
};

// A script that never runs out (contracts/mock-wire.md: "an exhausted script behaves as
// Respond with the default delay" — this file's bridge must never let host_wire reach that
// fallback, since host_wire's own Respond would fabricate an echo instead of the real
// Responder's answer arriving through the hand-built bridge below). One Silence entry per
// possible transmission this file's tests ever make to one node.
// Sized deliberately, and asserted against in bridge_latest_request(): when a wildcard script
// runs out, MockWire falls back to answering for itself -- so exhausting this one would make
// the host_wire fabricate responses with no Responder involved, and every cell past that point
// would pass without testing anything (red team @e4fffd9). The busiest BRIDGED cell today is
// SC-005 RECOVERED: 1 enrolling clean transaction (1 transmission) + 3 failed transactions x 3
// attempts (9) + 1 recovering clean transaction (1) + 1 further failed transaction proving
// consecutive_failures reset (3, review @3b1a80e) = 14; the array below holds 16, two
// transmissions of headroom. That is this file's current contents, not a guarantee: the next
// bridged cell added, or one more retry position, would cross silently and must have this
// count re-derived, not assumed still generous. The assertion turns an actual overrun into
// "harness budget exceeded" instead of a fabricated pass.
//
// Re-derived for T042/#572 (the wrong-rate case at the end of this file), which is the first
// cell here that is NOT bridged: it transmits 56 frames on host_wire, far past the 16 above,
// and the count still holds because the two budgets are different things.
//   - The assertion is bridge_latest_request()'s, and that cell never calls it: it drives the
//     real Master against host_wire's own scripted node instead (see that section's header).
//     So the 14 above is still the largest value `transcript_size()` ever takes at that
//     REQUIRE, and the bridged cells are the only ones it governs.
//   - What the wildcard must still cover there is per NODE, because each node draws the
//     wildcard at its own cursor (mock_wire.hpp, wildcard_pos_): 0x02..0x0F are each probed
//     once, 3 transmissions apiece, so 3 of the 16 entries; kNode draws its OWN script for
//     every request it hears and the wildcard not at all. `transcript_size()` is the bridged
//     cells' conservative proxy for that per-node figure (they address one node), not a
//     second rule this file obeys everywhere.
//   - MockWire's transcript itself holds kTranscriptCapacity == 128 (mock_wire.hpp), so those
//     56 frames fit with room to spare, and an overrun there is a recorded fault rather than a
//     silent drop.
constexpr Step kAlwaysSilence[] = {
    {0xFF, Kind::Silence}, {0xFF, Kind::Silence}, {0xFF, Kind::Silence}, {0xFF, Kind::Silence},
    {0xFF, Kind::Silence}, {0xFF, Kind::Silence}, {0xFF, Kind::Silence}, {0xFF, Kind::Silence},
    {0xFF, Kind::Silence}, {0xFF, Kind::Silence}, {0xFF, Kind::Silence}, {0xFF, Kind::Silence},
    {0xFF, Kind::Silence}, {0xFF, Kind::Silence}, {0xFF, Kind::Silence}, {0xFF, Kind::Silence},
};
constexpr size_t kAlwaysSilenceLen = sizeof(kAlwaysSilence) / sizeof(kAlwaysSilence[0]);

// One real Master + one real Responder (behind a counting handler), each on its own MockWire,
// both sharing a FakeClock, built from MockWire's public surface only (see file comment).
//
// This is NOT #52's wiring as written -- that asks for ONE MockWire with each node's MockWire
// handler being that node's Responder, and no handler here is ever wired to the Responder.
// The quotation used to sit on this line as though it described the code, which read as a
// claim of conformance (review @ef1ec22). The divergence and its rationale are on file:
// docs/OPEN-QUESTIONS.md 2026-09-06, pending a human ruling on whether to accept the bridge
// or amend the criterion.
struct Loop {
    FakeClock clock;
    MockWire host_wire{clock};
    MockWire node_wire{clock};
    CountingHandler handler;
    Responder responder;
    Master master;

    Loop()
        : responder(node_wire, clock, handler, kNode), master(host_wire, clock, omgp::ADDR_host) {
        host_wire.set_script(0xFF, kAlwaysSilence, kAlwaysSilenceLen);
    }
};

// What happens to one attempt's response on its way from the real Responder back to Master.
enum class Fault {
    Clean,          // delivered unmodified, on time
    Drop,           // never delivered (response timeout, trunk §7)
    Duplicate,      // delivered on time, then the identical bytes again, late
    DelayPastTResp, // delivered, but not until after this attempt's window has closed
    Corrupt,        // delivered with its CRC corrupted (CRC-failed response, trunk §7)
};

// Bytes MockWire (or a MockWire-driven Responder) sends back for the fields captured in a
// TxRecord — never read TxRecord::payload's storage after the source MockWire is destroyed;
// it is a stable fixed-size member copy for that MockWire's own lifetime (mock_wire.hpp).
std::vector<uint8_t> encode_response(const MockWire::TxRecord& r) {
    return encode_expected(r.dst, r.src, r.response, r.retry, r.seq, r.payload, r.len);
}

// One transaction's worth of stray "arrives long after everything else has concluded" bytes
// (used by the "after give-up" cells below) — scheduled comfortably past any window or gap
// this file's own transactions ever reach, so it can never be legitimately accepted by
// anything, whatever fault kind produced it.
constexpr uint64_t kFarFuture = 1'000'000; // 1s: far past any single transaction's timing

// Drives one whole transaction to `dst`, applying `plan[attempt]` to that attempt's response
// (attempt 0, retry 1, retry 2). Every attempt's request is bridged to the real Responder
// first regardless of `plan`, so `loop.handler.invocations` and the replay buffer always see
// exactly what a real node would (see file comment); only what reaches Master back on
// host_wire is shaped by `plan`. Returns once a terminal MasterEvent is produced. If `plan`
// never both fails and reaches attempt 2, the loop terminates early on Answered.
// Opens a transaction on the Master and makes sure its first request is actually on
// host_wire's transcript before returning.
void begin_transaction(Loop& loop, uint8_t dst, const uint8_t* payload, size_t len) {
    const size_t before = loop.host_wire.transcript_size();
    REQUIRE(loop.master.begin(dst, payload, len) == Status::Ok);
    if (loop.host_wire.transcript_size() == before) {
        // link/master.cpp (T031): every received byte pushes last_activity_ out, including
        // bytes that never form a frame (red-team M5, "unkillable duplicate") — so begin()
        // right after this file's own babble bytes is gap-deferred rather than transmitting
        // synchronously. Flush it forward past any possible deferral bound (data-model.md §4
        // "Gap": bounded by defer_origin + max_frame + T_gap) before assuming the transcript
        // holds this transaction's request.
        const uint64_t flush_to = loop.clock.now_us() +
                                  static_cast<uint64_t>(kMaxWire) * byte_us() +
                                  2 * omgp::TRUNK_T_gap_us;
        loop.host_wire.advance_to(flush_to, loop.master);
        REQUIRE(loop.host_wire.transcript_size() > before);
    }
}

// One attempt, bridged: the Master's latest request copied onto node_wire, the real Responder
// poll()ed through it, and its real, captured response returned for the caller to deliver
// (or not) to host_wire.
struct BridgedAttempt {
    uint64_t req_tx_end;        // the request's last byte ends here on host_wire
    uint64_t response_start_us; // the Responder's first response byte (default turnaround)
    MockWire::TxRecord resp;    // the Responder's response as node_wire recorded it
    std::vector<uint8_t> resp_bytes;
};

BridgedAttempt bridge_latest_request(Loop& loop, uint8_t dst) {
    // What the node had said BEFORE this attempt. Without it the read below silently picks up
    // the previous attempt's record when the Responder transmits nothing, re-encodes it and
    // injects it as though the node had just answered -- same seq, same payload, so no content
    // assertion can tell. A Responder that counted replays_served and never scheduled the
    // retransmit passed 13 of 18 cells that way (red team @410ce9f finding 2).
    // The host_wire's silence script must still be covering this transmission; see
    // kAlwaysSilence. Checked here rather than trusted, because exhausting it fails OPEN.
    REQUIRE(loop.host_wire.transcript_size() < kAlwaysSilenceLen);
    const size_t node_frames_before = loop.node_wire.transcript_size();
    const size_t idx = loop.host_wire.transcript_size() - 1;
    const auto req = loop.host_wire.transcript(idx); // copy: stable across further calls
    // Observe the addressing the Master actually emitted, then forward THAT. The bridge used
    // to pass `dst`, `ADDR_host` and `false` to encode_expected regardless of what the record
    // held, silently correcting a Master that addressed the wrong node, claimed the wrong
    // source, or set the RESPONSE bit -- none of those three fields was read anywhere in this
    // file, so a Master that swapped them passed all 19 cells (red team @f5544fc).
    REQUIRE(req.dst == dst);
    REQUIRE(req.src == omgp::ADDR_host);
    REQUIRE_FALSE(req.response);
    const auto req_bytes =
        encode_expected(req.dst, req.src, req.response, req.retry, req.seq, req.payload, req.len);
    const uint64_t req_tx_end =
        req.tx_start_us + static_cast<uint64_t>(req_bytes.size()) * byte_us();

    // Bridge the request to the real Responder (see file comment): inject_bytes(), never
    // node_wire.transmit(), so MockWire itself never tries to auto-answer it.
    loop.node_wire.inject_bytes(req_bytes.data(), req_bytes.size(), req.tx_start_us);
    loop.node_wire.advance_to(req_tx_end, loop.responder); // drains the request
    REQUIRE_FALSE(loop.handler.overflowed);
    // Poll to the OUTER edge of trunk §9's turnaround, not to T_turn_min. The clock has to
    // reach T_turn_max for the upper bound below to be able to bind at all: advancing only to
    // T_turn_min made `resp.tx_start_us <= req_tx_end + T_turn_max` unfailable, and any
    // Responder conforming to §9 with a turnaround above the minimum failed at the
    // transcript-grew REQUIRE instead of at the range check the comment advertised (review
    // @2b34974 -- my own claim about what that check established was false). Both edges are
    // real now, and a conforming node anywhere in the band passes.
    loop.node_wire.advance_to(req_tx_end + omgp::TRUNK_T_turn_max_us, loop.responder);

    // The node really answered THIS attempt, and the record below is that answer.
    REQUIRE(loop.node_wire.transcript_size() == node_frames_before + 1);
    const auto resp = loop.node_wire.transcript(loop.node_wire.transcript_size() - 1);
    // ...and it answered when trunk §9 says it may. The instant is OBSERVED from the node's
    // own transcript, not assumed: the bridge used to inject at its own computed
    // req_tx_end + T_turn_min regardless, so a node keying down with zero turnaround -- a
    // trunk §3 violation on half-duplex RS-485 -- was silently re-timed and the whole file
    // stayed green (red team @410ce9f finding 3).
    REQUIRE(resp.tx_start_us >= req_tx_end + omgp::TRUNK_T_turn_min_us);
    REQUIRE(resp.tx_start_us <= req_tx_end + omgp::TRUNK_T_turn_max_us);
    return BridgedAttempt{req_tx_end, resp.tx_start_us, resp, encode_response(resp)};
}

MasterEvent run_transaction(Loop& loop, uint8_t dst, const uint8_t* payload, size_t len,
                            const Fault (&plan)[3]) {
    begin_transaction(loop, dst, payload, len);

    for (int attempt = 0; attempt < 3; ++attempt) {
        const BridgedAttempt a = bridge_latest_request(loop, dst);
        const uint64_t req_tx_end = a.req_tx_end;
        const uint64_t response_start_us = a.response_start_us;
        const auto& resp = a.resp;
        const auto& resp_bytes = a.resp_bytes;

        const Fault fault = plan[attempt];
        if (fault == Fault::Clean || fault == Fault::Duplicate) {
            loop.host_wire.inject_bytes(resp_bytes.data(), resp_bytes.size(), response_start_us);
            const uint64_t full_end =
                response_start_us + static_cast<uint64_t>(resp_bytes.size()) * byte_us();
            if (fault != Fault::Duplicate)
                return loop.host_wire.advance_to(full_end, loop.master);

            // contracts/mock-wire.md Kind::Duplicate: the real response, then the same
            // bytes again after it (here, after the whole transaction has had time to
            // conclude on the first copy, so it cannot be mistaken for a second genuine
            // answer arriving inside the SAME window). Deliver the duplicate and advance
            // past it here, on the caller's behalf, so every Duplicate cell actually drains
            // it and proves it had no effect, rather than leaving it sitting undelivered in
            // host_wire's RX queue until the test ends.
            const uint64_t dup_start = full_end + omgp::TRUNK_T_gap_us;
            loop.host_wire.inject_bytes(resp_bytes.data(), resp_bytes.size(), dup_start);
            MasterEvent ev = loop.host_wire.advance_to(full_end, loop.master);
            // link/master.hpp: "payload points into Master's own buffer and is valid only
            // until the next poll() call" -- and the duplicate below IS another poll(). The
            // bytes are copied into this frame's own storage first, so the caller's later
            // `ev.response.payload[0]` reads THIS answer rather than whatever the buffer holds
            // afterwards (review @ef1ec22: the cells were reading outside the documented
            // window, and reading the right bytes only by an internal detail master.hpp does
            // not promise -- byte-identical to the duplicate, so no mutation could tell).
            // Function-local static, not a stack array: the pointer below outlives this
            // frame, and a stack buffer would dangle the moment run_transaction returns
            // (ASan stack-use-after-return, caught here before it was committed). The suite
            // is single-threaded and each cell reads the answer before the next call.
            static uint8_t answer[omgp::LIMIT_max_l3_payload];
            const uint8_t answer_len = ev.response.len;
            REQUIRE(answer_len <= sizeof answer);
            if (ev.kind == MasterEvent::Answered && answer_len > 0)
                std::memcpy(answer, ev.response.payload, answer_len);
            const uint64_t dup_end =
                dup_start + static_cast<uint64_t>(resp_bytes.size()) * byte_us();
            const MasterEvent late = loop.host_wire.advance_to(dup_end, loop.master);
            REQUIRE(late.kind == MasterEvent::None);
            REQUIRE_FALSE(loop.master.busy());
            if (ev.kind == MasterEvent::Answered)
                ev.response.payload = answer;
            return ev;
        }

        uint64_t next_attempt_ready_at; // when THIS attempt concludes (retry or Failed follows)
        if (fault == Fault::Corrupt) {
            uint8_t buf[kMaxWire];
            FrameFields rf{};
            rf.dst = resp.dst;
            rf.src = resp.src;
            rf.response = resp.response;
            rf.retry = resp.retry;
            rf.seq = resp.seq;
            rf.len = resp.len;
            rf.payload = resp.payload;
            const size_t written = encode_crc_corrupted(rf, buf, sizeof buf);
            REQUIRE(written > 0);
            loop.host_wire.inject_bytes(buf, written, response_start_us);
            // data-model.md §4: "A CRC-failed response in the window ends the attempt
            // immediately" — no need to wait out the rest of T_resp.
            next_attempt_ready_at = response_start_us + static_cast<uint64_t>(written) * byte_us();
        } else {
            // Drop: nothing ever arrives. DelayPastTResp: something will, but not until well
            // after this attempt's window and this whole transaction have concluded (bridged
            // below, once the transaction reaches a terminal event) — from Master's
            // perspective during THIS attempt the two are identical: silence through T_resp.
            // The stray's own instant is offset by `attempt` so two DelayPastTResp attempts in
            // the same transaction (SC-004 "retry 2") each land at a distinct, non-overlapping
            // far-future instant rather than interleaving byte-for-byte at the same one.
            if (fault == Fault::DelayPastTResp) {
                const uint64_t stray_start =
                    kFarFuture + static_cast<uint64_t>(attempt) * 10 * omgp::TRUNK_T_resp_us;
                loop.host_wire.inject_bytes(resp_bytes.data(), resp_bytes.size(), stray_start);
            }
            next_attempt_ready_at = req_tx_end + omgp::TRUNK_T_resp_us;
        }

        if (attempt == 2)
            return loop.host_wire.advance_to(next_attempt_ready_at, loop.master); // Failed

        const uint64_t retry_tx_start = next_attempt_ready_at + omgp::TRUNK_T_gap_us;
        const MasterEvent ev = loop.host_wire.advance_to(retry_tx_start, loop.master);
        REQUIRE(ev.kind == MasterEvent::None); // retries remain: not yet terminal
    }
    FAIL("unreachable: the loop above always returns by attempt 2");
    return MasterEvent{};
}

} // namespace

// --- SC-004: drop (silence) at each retry position ------------------------------------------
// trunk §7 "response timeout". A dropped response is indistinguishable from one that was never
// sent: Master must retry with the SAME sequence, and the real Responder's replay buffer must
// answer the retry without invoking the handler again.

TEST_CASE("SC-004 drop at attempt 0: recovers on the first retry, handler invoked once", "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x11};
    const Fault plan[3] = {Fault::Drop, Fault::Clean, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    // The ANSWER ITSELF, not just its header: the handler echoes the request's payload, so
    // this is what separates "the right answer came back" from "a frame with the right
    // seq came back" -- the distinction the replay buffer exists to make, and the one
    // nothing in this file asserted (red team @c69679c finding 1).
    REQUIRE(ev.response.len == sizeof payload);
    REQUIRE(ev.response.payload[0] == payload[0]);
    REQUIRE(ev.response.seq == 0); // accepted seq == this transaction's own seq
    REQUIRE(loop.master.attempts() == 2);
    REQUIRE(loop.handler.invocations == 1); // retry replayed, never re-invoked
    REQUIRE(loop.responder.stats().replays_served == 1);
}

TEST_CASE("SC-004 drop through retry 1: recovers on the second retry, handler invoked once",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x12};
    const Fault plan[3] = {Fault::Drop, Fault::Drop, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    // The ANSWER ITSELF, not just its header: the handler echoes the request's payload, so
    // this is what separates "the right answer came back" from "a frame with the right
    // seq came back" -- the distinction the replay buffer exists to make, and the one
    // nothing in this file asserted (red team @c69679c finding 1).
    REQUIRE(ev.response.len == sizeof payload);
    REQUIRE(ev.response.payload[0] == payload[0]);
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == 3);
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.responder.stats().replays_served == 2);
}

TEST_CASE("SC-004 drop after give-up: Failed{Timeout} after exactly 3 transmissions, handler "
          "invoked once",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x14};
    const Fault plan[3] = {Fault::Drop, Fault::Drop, Fault::Drop};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Failed);
    REQUIRE(ev.reason == MasterEvent::Timeout);
    REQUIRE(loop.master.attempts() == 3); // initial + TRUNK_retries, never a fourth
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE_FALSE(loop.master.busy());
    // FR-011 (#52: "every matrix cell asserts the sequence the Responder accepted equals the
    // open transaction's own"): a Failed cell has no ev.response to read it from, but the
    // Responder DID accept and answer all three attempts -- its own transcript carries the
    // sequence it echoed, and every attempt of one transaction carries seq 0 (review
    // @c7adea0, LOW).
    REQUIRE(loop.node_wire.transcript_size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        INFO("attempt " << i);
        REQUIRE(loop.node_wire.transcript(i).seq == 0);
    }
}

// --- SC-004: response delayed past T_resp -------------------------------------------------
// trunk §7 "response timeout" (the response exists, but arrives too late to matter — the
// window discipline, not silence, is what must discard it).

TEST_CASE("SC-004 delay-past-T_resp at attempt 0: recovers via the retry; the stale late "
          "answer is ignored once it finally arrives",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x21};
    const Fault plan[3] = {Fault::DelayPastTResp, Fault::Clean, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    // The ANSWER ITSELF, not just its header: the handler echoes the request's payload, so
    // this is what separates "the right answer came back" from "a frame with the right
    // seq came back" -- the distinction the replay buffer exists to make, and the one
    // nothing in this file asserted (red team @c69679c finding 1).
    REQUIRE(ev.response.len == sizeof payload);
    REQUIRE(ev.response.payload[0] == payload[0]);
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == 2);
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.responder.stats().replays_served == 1);
    REQUIRE_FALSE(loop.master.busy());
    REQUIRE(loop.master.stats(kNode).discards == 0); // nothing stray has arrived yet
    REQUIRE(loop.master.bus_stats().discards == 0);  // ...on either counter (#144)

    // The withheld attempt-0 response was scheduled at kFarFuture (run_transaction) — still
    // in host_wire's RX queue, since nothing has advanced the clock that far yet. Let it
    // arrive now that the transaction has long since concluded: no busy(), no second event,
    // and the frame OBSERVED as a discard counted at bus level (link/master.cpp: with no
    // transaction open there is no address to attribute it to — #144, ruling 2026-09-11) —
    // the assertion that separates "delivered and ignored" from "never delivered" (red team
    // @fc3fc1a finding 1: without it, deleting the stray from the wire left this cell green).
    const MasterEvent late = loop.host_wire.advance_to(kFarFuture + 10 * byte_us(), loop.master);
    REQUIRE(late.kind == MasterEvent::None);
    REQUIRE_FALSE(loop.master.busy());
    REQUIRE(loop.master.bus_stats().discards == 1);
    REQUIRE(loop.master.stats(kNode).discards == 0);
}

TEST_CASE("SC-004 delay-past-T_resp through retry 1: recovers on the second retry; the stale "
          "late answer is ignored once it finally arrives",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x23};
    const Fault plan[3] = {Fault::Drop, Fault::DelayPastTResp, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    // The ANSWER ITSELF, not just its header: the handler echoes the request's payload, so
    // this is what separates "the right answer came back" from "a frame with the right
    // seq came back" -- the distinction the replay buffer exists to make, and the one
    // nothing in this file asserted (red team @c69679c finding 1).
    REQUIRE(ev.response.len == sizeof payload);
    REQUIRE(ev.response.payload[0] == payload[0]);
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == 3);
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.responder.stats().replays_served == 2);

    // run_transaction offsets each DelayPastTResp attempt's stray by attempt*10*T_resp so
    // several in one transaction never collide (attempt 1 here: kFarFuture + 10*T_resp).
    const MasterEvent late = loop.host_wire.advance_to(
        kFarFuture + 10 * omgp::TRUNK_T_resp_us + 10 * byte_us(), loop.master);
    REQUIRE(late.kind == MasterEvent::None);
    REQUIRE_FALSE(loop.master.busy());
    REQUIRE(loop.master.bus_stats().discards == 1); // the one stray, observed (bus-level, #144)
    REQUIRE(loop.master.stats(kNode).discards == 0);
}

TEST_CASE("SC-004 delay-past-T_resp at attempts 0 AND 1: uses the full retry budget and still "
          "recovers on retry 2; both stale late answers are ignored once they finally arrive",
          "[link]") {
    // Named for what it scripts, not for a matrix column it does not occupy (review
    // @47d481f): the delay faults here sit at attempts 0 and 1, so this is the TWO-STRAYS
    // cell -- `discards == 2` below, against the one-stray cell above. A delay at retry 2 is
    // scripted by "delay-past-T_resp after give-up" ({Drop, Drop, Delay}), because at
    // TRUNK_retries == 2 a delay there has no fourth attempt to recover on and always ends
    // Failed{Timeout} with the answer arriving after give-up: for this row, as for Drop's,
    // "retry 2" and "after give-up" are one script occupying two columns.
    Loop loop;
    const uint8_t payload[] = {0x24};
    const Fault plan[3] = {Fault::DelayPastTResp, Fault::DelayPastTResp, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    // The ANSWER ITSELF, not just its header: the handler echoes the request's payload, so
    // this is what separates "the right answer came back" from "a frame with the right
    // seq came back" -- the distinction the replay buffer exists to make, and the one
    // nothing in this file asserted (red team @c69679c finding 1).
    REQUIRE(ev.response.len == sizeof payload);
    REQUIRE(ev.response.payload[0] == payload[0]);
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == static_cast<uint8_t>(omgp::TRUNK_retries) + 1);
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.responder.stats().replays_served == 2);

    // Two strays this time (attempts 0 and 1); advance past the later of the two.
    const MasterEvent late = loop.host_wire.advance_to(
        kFarFuture + 10 * omgp::TRUNK_T_resp_us + 10 * byte_us(), loop.master);
    REQUIRE(late.kind == MasterEvent::None);
    REQUIRE_FALSE(loop.master.busy());
    REQUIRE(loop.master.bus_stats().discards == 2); // both strays, observed (bus-level, #144)
    REQUIRE(loop.master.stats(kNode).discards == 0);
}

TEST_CASE("SC-004 delay-past-T_resp after give-up: Failed{Timeout}; the stale late answer is "
          "still ignored once it finally arrives",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x22};
    const Fault plan[3] = {Fault::Drop, Fault::Drop, Fault::DelayPastTResp};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Failed);
    REQUIRE(ev.reason == MasterEvent::Timeout);
    REQUIRE(loop.master.attempts() == 3); // initial + TRUNK_retries, never a fourth
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.responder.stats().replays_served == 2);
    REQUIRE_FALSE(loop.master.busy());

    // attempt 2's stray: kFarFuture + 20*T_resp (run_transaction's per-attempt offset).
    const MasterEvent late = loop.host_wire.advance_to(
        kFarFuture + 20 * omgp::TRUNK_T_resp_us + 10 * byte_us(), loop.master);
    REQUIRE(late.kind == MasterEvent::None);
    REQUIRE_FALSE(loop.master.busy());
    // The stray, observed after give-up — bus-level: the transaction is over, so no address
    // owns the window it arrived outside of (#144, ruling 2026-09-11).
    REQUIRE(loop.master.bus_stats().discards == 1);
    REQUIRE(loop.master.stats(kNode).discards == 0);
    // FR-011, as the other Failed cells carry it: the Responder accepted and answered every
    // attempt of ONE transaction, so its own transcript must show that transaction's sequence
    // on each. Review @68a52ac: the previous commit put this loop on the ANSWERED "through
    // retry 2" cell -- which already reads ev.response.seq -- and left this one, the cell that
    // actually needed it, uncovered. Placed by the Failed cells' line numbers this time.
    REQUIRE(loop.node_wire.transcript_size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        INFO("attempt " << i);
        REQUIRE(loop.node_wire.transcript(i).seq == 0);
    }
}

// --- SC-004: delay-past-T_resp, AT the window's closing edge, while the attempt is open ------
// The four cells above deliver their late answer long after the transaction concluded, so the
// Master sees it with no window open at all — silence and lateness are the same thing to them,
// and the window discipline (link/master.cpp: `in_window` at the acceptance check) is never
// consulted (red team @fc3fc1a finding 2: deleting that conjunct left this file green). This
// cell is the one where it is: the frame OPENS exactly at deadline_ = request_end + T_resp
// (link/master.cpp:170) and is drained by the same poll() that times the attempt out, while
// the attempt is still awaiting — one instant late, so discarded; one instant earlier and it
// would be the answer (test_link_master.cpp pins that edge from the Master's side alone).

TEST_CASE("SC-004 delay-past-T_resp at the boundary: a response opening exactly at the "
          "attempt's T_resp deadline, drained by the poll() that times the attempt out, is "
          "discarded and charged to the node; the retry's answer is accepted",
          "[link][timing:T_resp]") {
    Loop loop;
    const uint8_t payload[] = {0x25};
    begin_transaction(loop, kNode, payload, sizeof payload);
    const BridgedAttempt a0 = bridge_latest_request(loop, kNode);

    // trunk §3: the response window is [request_end, request_end + T_resp) on the START bit;
    // this frame's opening FLAG lands on the closed edge.
    const uint64_t deadline = a0.req_tx_end + omgp::TRUNK_T_resp_us;
    loop.host_wire.inject_bytes(a0.resp_bytes.data(), a0.resp_bytes.size(), deadline);
    const uint64_t late_end = deadline + static_cast<uint64_t>(a0.resp_bytes.size()) * byte_us();

    // One poll sees the whole late frame (drained first, attempt still awaiting) and then the
    // expired deadline: not Answered, discard charged to dst_ (the window WAS this node's),
    // the attempt timed out, the retry pending.
    const MasterEvent ev0 = loop.host_wire.advance_to(late_end, loop.master);
    REQUIRE(ev0.kind == MasterEvent::None);
    REQUIRE(loop.master.busy());
    REQUIRE(loop.master.attempts() == 1);
    REQUIRE(loop.master.stats(kNode).discards == 1);
    REQUIRE(loop.master.stats(kNode).timeouts == 1);
    REQUIRE(loop.handler.invocations == 1);

    // The late frame's bytes were bus activity: the retry goes out T_gap after its last byte
    // (data-model.md §4 "Gap"), same sequence, retry bit set (trunk §7).
    const uint64_t retry_tx = late_end + omgp::TRUNK_T_gap_us;
    REQUIRE(loop.host_wire.advance_to(retry_tx, loop.master).kind == MasterEvent::None);
    REQUIRE(loop.host_wire.transcript_size() == 2);
    REQUIRE(loop.host_wire.transcript(1).tx_start_us == retry_tx);
    REQUIRE(loop.host_wire.transcript(1).retry);
    REQUIRE(loop.host_wire.transcript(1).seq == 0);

    // The real Responder replays; delivered clean and on time, it is the answer.
    const BridgedAttempt a1 = bridge_latest_request(loop, kNode);
    loop.host_wire.inject_bytes(a1.resp_bytes.data(), a1.resp_bytes.size(), a1.response_start_us);
    const MasterEvent ev = loop.host_wire.advance_to(
        a1.response_start_us + static_cast<uint64_t>(a1.resp_bytes.size()) * byte_us(),
        loop.master);
    REQUIRE(ev.kind == MasterEvent::Answered);
    // The ANSWER ITSELF, not just its header: the handler echoes the request's payload, so
    // this is what separates "the right answer came back" from "a frame with the right
    // seq came back" -- the distinction the replay buffer exists to make, and the one
    // nothing in this file asserted (red team @c69679c finding 1).
    REQUIRE(ev.response.len == sizeof payload);
    REQUIRE(ev.response.payload[0] == payload[0]);
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == 2);
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.responder.stats().replays_served == 1);
    REQUIRE(loop.master.stats(kNode).discards == 1);
    REQUIRE_FALSE(loop.master.busy());
}

// --- SC-004: CRC-corrupted response --------------------------------------------------------
// trunk §7 "CRC-failed response": ends the attempt at once (data-model.md §4), rather than
// waiting out the rest of T_resp — the retry timing above reflects that.

TEST_CASE("SC-004 CRC-corrupted response at attempt 0: ends that attempt immediately, "
          "recovers on the retry, handler invoked once",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x31};
    const Fault plan[3] = {Fault::Corrupt, Fault::Clean, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    // The ANSWER ITSELF, not just its header: the handler echoes the request's payload, so
    // this is what separates "the right answer came back" from "a frame with the right
    // seq came back" -- the distinction the replay buffer exists to make, and the one
    // nothing in this file asserted (red team @c69679c finding 1).
    REQUIRE(ev.response.len == sizeof payload);
    REQUIRE(ev.response.payload[0] == payload[0]);
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == 2);
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.master.stats(kNode).crc_failures == 1);
}

TEST_CASE("SC-004 CRC-corrupted response through retry 1: recovers on the second retry, "
          "handler invoked once",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x32};
    const Fault plan[3] = {Fault::Corrupt, Fault::Corrupt, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    // The ANSWER ITSELF, not just its header: the handler echoes the request's payload, so
    // this is what separates "the right answer came back" from "a frame with the right
    // seq came back" -- the distinction the replay buffer exists to make, and the one
    // nothing in this file asserted (red team @c69679c finding 1).
    REQUIRE(ev.response.len == sizeof payload);
    REQUIRE(ev.response.payload[0] == payload[0]);
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == 3);
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.master.stats(kNode).crc_failures == 2);
}

TEST_CASE("SC-004 CRC-corrupted response through retry 2: Failed{CrcFailed} after exactly 3 "
          "transmissions, handler invoked once",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x34};
    const Fault plan[3] = {Fault::Corrupt, Fault::Corrupt, Fault::Corrupt};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Failed);
    REQUIRE(ev.reason == MasterEvent::CrcFailed);
    REQUIRE(loop.master.attempts() == 3);
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.master.stats(kNode).crc_failures == 3);
    REQUIRE_FALSE(loop.master.busy());
    // FR-011 (#52: "every matrix cell asserts the sequence the Responder accepted equals the
    // open transaction's own"): a Failed cell has no ev.response to read it from, but the
    // Responder DID accept and answer all three attempts -- its own transcript carries the
    // sequence it echoed, and every attempt of one transaction carries seq 0 (review
    // @c7adea0, LOW).
    REQUIRE(loop.node_wire.transcript_size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        INFO("attempt " << i);
        REQUIRE(loop.node_wire.transcript(i).seq == 0);
    }
}

TEST_CASE("SC-004 CRC-corrupted frame after give-up: a corrupt frame arriving with no "
          "transaction open is discarded, with no event and no re-opened transaction",
          "[link]") {
    // Review @ef1ec22 [MEDIUM]: the case above scripts {Corrupt, Corrupt, Corrupt}, which is
    // the RETRY-2 position -- it was named "after give-up" and so claimed a matrix position it
    // did not exercise. Unlike the Drop row, whose "after give-up" cell really is degenerate
    // at TRUNK_retries == 2 (docs/OPEN-QUESTIONS.md 2026-09-06), the CrcError row's is a real
    // and distinct scenario: a corrupt frame with NO window open, which the Duplicate and
    // Delay rows both test and nothing here tested for a corrupt one.
    Loop loop;
    const uint8_t payload[] = {0x35};
    const Fault plan[3] = {Fault::Corrupt, Fault::Corrupt, Fault::Corrupt};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Failed);
    REQUIRE(ev.reason == MasterEvent::CrcFailed);
    REQUIRE(loop.master.attempts() == 3);
    const uint32_t crc_failures_before = loop.master.stats(kNode).crc_failures;
    REQUIRE(crc_failures_before == 3);

    // The node's last (corrupted) answer arrives again, long after the transaction failed.
    const auto resp = loop.node_wire.transcript(loop.node_wire.transcript_size() - 1);
    uint8_t buf[kMaxWire];
    const size_t n =
        omgp_test::encode_crc_corrupted(FrameFields{omgp::ADDR_host, kNode, /*response=*/true,
                                                    resp.retry, resp.seq, resp.len, resp.payload},
                                        buf, sizeof buf);
    REQUIRE(n > 0);
    const uint64_t late_start = kFarFuture;
    loop.host_wire.inject_bytes(buf, n, late_start);
    const MasterEvent late =
        loop.host_wire.advance_to(late_start + static_cast<uint64_t>(n) * byte_us(), loop.master);

    // No event, no transaction re-opened, and the bad CRC counted against the node that
    // claimed to send it -- the same accounting as inside a window, with no window open.
    REQUIRE(late.kind == MasterEvent::None);
    REQUIRE_FALSE(loop.master.busy());
    REQUIRE(loop.master.attempts() == 3);
    REQUIRE(loop.handler.invocations == 1);
    // FR-011, as the other Failed cells carry it: the Responder accepted and answered every
    // attempt of ONE transaction, so its own transcript must show that transaction's sequence
    // on each. When this loop was added at 7ecfac8 there were three Failed cells; there are
    // five at this head, and two had been left without it (review @049abac).
    REQUIRE(loop.node_wire.transcript_size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        INFO("attempt " << i);
        REQUIRE(loop.node_wire.transcript(i).seq == 0);
    }
    // ...and it is counted at BUS level, against no node. A corrupt frame never decodes, so
    // there is no f.src to charge it to (link/master.cpp's discard accounting charges dst_
    // while awaiting), and outside a window the Master is awaiting nothing — which is
    // precisely why it is bus-level evidence rather than a node's failure.
    //
    // This cell was written expecting crc_failures + 1, measured otherwise, and pinned the
    // "counted nowhere" outcome as today's observable while docs/OPEN-QUESTIONS.md 2026-09-07
    // asked whether trunk §4's discard accounting should see corruption occurring between
    // transactions. Ruled 2026-09-11: count it at bus level (#144). `crc_failures` stays a
    // per-node counter and is still unmoved.
    CHECK(loop.master.stats(kNode).crc_failures == crc_failures_before);
    CHECK(loop.master.stats(kNode).discards == 0);
    CHECK(loop.master.bus_stats().discards == 1);
    CHECK(loop.master.bus_stats().bus_faults == 0);
}

// --- SC-004: duplicate response -------------------------------------------------------------
// trunk §7 "duplicate": the genuine answer always arrives and the transaction always
// succeeds on it; the extra, late copy is what each cell checks is discarded without effect.
// `handler.invocations == 1` is NOT load-bearing in this row (review @fc3fc1a, LOW): the
// duplicate is of the RESPONSE, so the Responder sees each request once and the replay
// buffer is exercised only by the Drop attempts these plans contain. What each cell here
// pins is that the extra copy was delivered and OBSERVED as a discard — `bus_stats().
// discards` counts it (link/master.cpp: a frame with no window open has no address it may be
// charged to, #144 ruling 2026-09-11) — with no second event and no re-opened transaction
// (red team @fc3fc1a finding 1: without the discard count, deleting the copy from the wire
// left the row green).

TEST_CASE("SC-004 duplicate at attempt 0: succeeds once; the late duplicate has no effect",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x41};
    const Fault plan[3] = {Fault::Duplicate, Fault::Clean, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    // The ANSWER ITSELF, not just its header: the handler echoes the request's payload, so
    // this is what separates "the right answer came back" from "a frame with the right
    // seq came back" -- the distinction the replay buffer exists to make, and the one
    // nothing in this file asserted (red team @c69679c finding 1).
    REQUIRE(ev.response.len == sizeof payload);
    REQUIRE(ev.response.payload[0] == payload[0]);
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == 1); // the genuine answer landed first attempt
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE_FALSE(loop.master.busy());
    REQUIRE(loop.master.bus_stats().discards == 1); // the late copy, observed (bus-level, #144)
    REQUIRE(loop.master.stats(kNode).discards == 0);
}

TEST_CASE("SC-004 duplicate through retry 1: succeeds once the retry lands; the late "
          "duplicate has no effect",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x42};
    const Fault plan[3] = {Fault::Drop, Fault::Duplicate, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    // The ANSWER ITSELF, not just its header: the handler echoes the request's payload, so
    // this is what separates "the right answer came back" from "a frame with the right
    // seq came back" -- the distinction the replay buffer exists to make, and the one
    // nothing in this file asserted (red team @c69679c finding 1).
    REQUIRE(ev.response.len == sizeof payload);
    REQUIRE(ev.response.payload[0] == payload[0]);
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == 2);
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.responder.stats().replays_served == 1);
    REQUIRE_FALSE(loop.master.busy());
    REQUIRE(loop.master.bus_stats().discards == 1); // the late copy, observed (bus-level, #144)
    REQUIRE(loop.master.stats(kNode).discards == 0);
}

TEST_CASE("SC-004 duplicate through retry 2: uses the full retry budget and still succeeds "
          "once; the late duplicate has no effect",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x43};
    const Fault plan[3] = {Fault::Drop, Fault::Drop, Fault::Duplicate};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    // The ANSWER ITSELF, not just its header: the handler echoes the request's payload, so
    // this is what separates "the right answer came back" from "a frame with the right
    // seq came back" -- the distinction the replay buffer exists to make, and the one
    // nothing in this file asserted (red team @c69679c finding 1).
    REQUIRE(ev.response.len == sizeof payload);
    REQUIRE(ev.response.payload[0] == payload[0]);
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == static_cast<uint8_t>(omgp::TRUNK_retries) + 1);
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.responder.stats().replays_served == 2);
    REQUIRE_FALSE(loop.master.busy());
    REQUIRE(loop.master.bus_stats().discards == 1); // the late copy, observed (bus-level, #144)
    REQUIRE(loop.master.stats(kNode).discards == 0);
}

TEST_CASE("SC-004 duplicate after give-up: a late duplicate of the final, withheld attempt "
          "still has no effect on the already-Failed transaction",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x44};
    const Fault plan[3] = {Fault::Drop, Fault::Drop, Fault::Drop};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Failed);
    REQUIRE(ev.reason == MasterEvent::Timeout);
    REQUIRE(loop.master.attempts() == 3); // initial + TRUNK_retries, never a fourth
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.master.stats(kNode).discards == 0); // nothing has arrived at all yet
    REQUIRE(loop.master.bus_stats().discards == 0);  // ...on either counter (#144)
    // FR-011 (#52: "every matrix cell asserts the sequence the Responder accepted equals the
    // open transaction's own"): a Failed cell has no ev.response to read it from, but the
    // Responder DID accept and answer all three attempts -- its own transcript carries the
    // sequence it echoed, and every attempt of one transaction carries seq 0 (review
    // @c7adea0, LOW).
    REQUIRE(loop.node_wire.transcript_size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        INFO("attempt " << i);
        REQUIRE(loop.node_wire.transcript(i).seq == 0);
    }

    // The real (withheld) attempt-2 response still exists on node_wire's own transcript;
    // deliver two late copies of it now, long after Master gave up.
    const auto resp = loop.node_wire.transcript(loop.node_wire.transcript_size() - 1);
    const auto resp_bytes = encode_response(resp);
    loop.host_wire.inject_bytes(resp_bytes.data(), resp_bytes.size(), kFarFuture);
    const uint64_t first_end = kFarFuture + static_cast<uint64_t>(resp_bytes.size()) * byte_us();
    loop.host_wire.inject_bytes(resp_bytes.data(), resp_bytes.size(),
                                first_end + omgp::TRUNK_T_gap_us);

    const MasterEvent late = loop.host_wire.advance_to(
        first_end + omgp::TRUNK_T_gap_us + static_cast<uint64_t>(resp_bytes.size()) * byte_us(),
        loop.master);
    REQUIRE(late.kind == MasterEvent::None);
    REQUIRE_FALSE(loop.master.busy());
    REQUIRE(loop.master.bus_stats().discards == 2); // both late copies, observed (bus-level)
    REQUIRE(loop.master.stats(kNode).discards == 0);
}

// --- SC-005 "babble": extraneous bus noise between transactions ---------------------------
// trunk §7 "babble" (contracts/mock-wire.md Kind::Babble: bytes regardless of addressee).
// This case uses a fixed, hand-picked byte sequence that is not a valid frame for any node,
// injected onto host_wire outside any open transaction's window, rather than a Kind::Babble
// step: it was written while that Kind was still unimplemented (T030/#48, since landed), and
// hand-picked bytes keep the case's own wire content exactly stated. Kind::Babble is now
// available if a later case wants a scripted babbler instead — see tests/unit/test_mock_wire.cpp
// for what it emits.

TEST_CASE("SC-005 babble: extraneous bus noise between transactions is silently discarded, "
          "not attributed to the next transaction",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x55};
    const Fault plan[3] = {Fault::Clean, Fault::Clean, Fault::Clean};
    REQUIRE(run_transaction(loop, kNode, payload, sizeof payload, plan).kind ==
            MasterEvent::Answered);
    REQUIRE_FALSE(loop.master.busy());

    // Bytes that can never parse as a frame addressed to anyone: no FLAG at all, so the
    // Deframer stays in Hunting throughout (link/frame.hpp state table) — a station
    // babbling with no regard for framing, not a corrupted frame.
    const uint8_t babble[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    const uint64_t babble_start = kFarFuture;
    loop.host_wire.inject_bytes(babble, sizeof babble, babble_start);
    const uint64_t babble_end = babble_start + static_cast<uint64_t>(sizeof babble) * byte_us();
    const MasterEvent during = loop.host_wire.advance_to(babble_end, loop.master);
    REQUIRE(during.kind == MasterEvent::None);
    REQUIRE_FALSE(loop.master.busy());

    // A fresh transaction to the same node afterward is unaffected: new seq, its own answer.
    const uint8_t payload2[] = {0x56};
    const MasterEvent ev2 = run_transaction(loop, kNode, payload2, sizeof payload2, plan);
    REQUIRE(ev2.kind == MasterEvent::Answered);
    // next_seq advanced past the first transaction only, and babble invoked nothing.
    REQUIRE(ev2.response.seq == 1);
    REQUIRE(loop.master.attempts() == 1);
    REQUIRE(loop.handler.invocations == 2);
    // The babble was bus activity all the same (FR-010; data-model.md §4 "Gap"): the second
    // request could not go out until T_gap of idle after the burst's last byte. This is what
    // shows the burst was delivered at all — with no babble on the wire, begin() at
    // babble_end transmits synchronously, AT babble_end (red team @fc3fc1a finding 1).
    REQUIRE(loop.host_wire.transcript_size() == 2);
    REQUIRE(loop.host_wire.transcript(1).tx_start_us >= babble_end + omgp::TRUNK_T_gap_us);
    REQUIRE(loop.host_wire.transcript(1).tx_start_us < kFarFuture + 10 * omgp::TRUNK_T_resp_us);
}

// --- SC-005: SUSPECT / OFFLINE / RECOVERED, driven by a REAL Master + REAL HealthTracker ----
// trunk §6 (ENROLLED/SUSPECT/OFFLINE/RECOVERED lifecycle, data-model.md §6) as consumed by
// node health; contracts/link-cpp.md "Health tracker": `on_result` is the one thing that
// connects a transaction's outcome to health state, called from THIS driver once each
// transaction concludes -- nowhere inside Master, Responder, or HealthTracker itself (T038/
// #56 and T031/#49 are both closed and unmodified by this file). `tick()` drives the one
// time-only transition (SUSPECT -> OFFLINE, data-model.md §6) with no transaction outcome at
// all, so it is called directly against the shared FakeClock rather than through a
// transaction plan. spec.md US4 Acceptance Scenarios 2 (SUSPECT), 4 (OFFLINE), 5 (RECOVERED).
//
// consecutive_failures is private to HealthTracker (link/health.hpp) with no accessor: the
// only observable proxy for "failures reset to 0" is the public state() the data-model.md §6
// transition table promises alongside it. Whether that reset holds under further failures is
// unit-proven by test_link_health.cpp (T037/#55) -- out of scope here (tasks.md T039): this
// file proves the WIRING between a real engine and a real tracker, not the state machine's
// own arithmetic a second time.

struct RecordingHealthListener : HealthListener {
    struct Entry {
        Notice notice;
        uint8_t addr;
    };
    std::vector<Entry> entries;
    void on_notice(Notice notice, uint8_t addr) override {
        entries.push_back({notice, addr});
    }
};

// kNode is the only address these SC-005 scripts ever enrol, so trunk §7's bus-fault rule
// (US5/T043: declare when EVERY enrolled node is SUSPECT or worse; ruling Q2, "a single
// enrolled node counts as all") fires on its SUSPECT transition and clears on its next valid
// answer -- at the reference rate, the rate in use, which FR-026 says clears at once. The
// bus-level notices of data-model.md §9 are therefore part of the stream these cases see:
// two per declare (BUS_FAULT, ALERT) and one per clear (BUS_RECOVERED), each with addr 0.
// The rules themselves are tests/unit/test_link_busfault.cpp's subject; here they are
// counted so the wiring assertions stay exact.
constexpr size_t kDeclareNotices = 2;
constexpr size_t kClearNotices = 1;

// Runs one transaction to completion and feeds its outcome to `tracker` -- the wiring
// tasks.md T039 asks for: on_result invoked from the loop driver, once, after
// run_transaction()'s own terminal MasterEvent is already known, at the SAME clock instant
// the transaction concluded at (loop.clock.now_us(), advanced by run_transaction's own
// host_wire.advance_to calls -- nothing here reads or sets the clock independently).
MasterEvent run_health_transaction(Loop& loop, HealthTracker& tracker, uint8_t dst,
                                   const uint8_t* payload, size_t len, const Fault (&plan)[3]) {
    const MasterEvent ev = run_transaction(loop, dst, payload, len, plan);
    tracker.on_result(dst, ev.kind == MasterEvent::Answered, loop.clock.now_us());
    return ev;
}

// Enrols kNode (one clean transaction) then drives exactly TRUNK_suspect_after_failures
// consecutive FAILED transactions (each a real {Drop,Drop,Drop} L2 retry exhaustion) --
// spec.md US4 AS2 ("three consecutive transactions fail"). Returns the clock instant the
// third failure concluded at (== suspect_since, since on_result is fed that same instant).
uint64_t drive_to_suspect(Loop& loop, HealthTracker& tracker) {
    const uint8_t enroll_payload[] = {0x71};
    const Fault clean[3] = {Fault::Clean, Fault::Clean, Fault::Clean};
    const MasterEvent enrolled =
        run_health_transaction(loop, tracker, kNode, enroll_payload, sizeof enroll_payload, clean);
    REQUIRE(enrolled.kind == MasterEvent::Answered);
    REQUIRE(tracker.state(kNode) == HealthState::ENROLLED);

    const Fault drop_all[3] = {Fault::Drop, Fault::Drop, Fault::Drop};
    uint64_t concluded_at = 0;
    for (uint32_t i = 0; i < omgp::TRUNK_suspect_after_failures; ++i) {
        const uint8_t payload[] = {static_cast<uint8_t>(0x72 + i)};
        const MasterEvent ev =
            run_health_transaction(loop, tracker, kNode, payload, sizeof payload, drop_all);
        REQUIRE(ev.kind == MasterEvent::Failed);
        concluded_at = loop.clock.now_us();
        if (i + 1 < omgp::TRUNK_suspect_after_failures)
            REQUIRE(tracker.state(kNode) == HealthState::ENROLLED);
    }
    REQUIRE(tracker.state(kNode) == HealthState::SUSPECT);
    return concluded_at;
}

// Drives kNode to SUSPECT, then to OFFLINE via tick() alone once TRUNK_offline_after_suspect_ms
// has elapsed on the shared FakeClock (data-model.md §6 "tick(now) with >=1000ms elapsed (no
// result needed)") -- never a fourth failed transaction. The real Master is polled across that
// same span and stays idle throughout, showing the transition is time-only.
uint64_t drive_to_offline(Loop& loop, HealthTracker& tracker) {
    const uint64_t suspect_since = drive_to_suspect(loop, tracker);
    constexpr uint64_t kOfflineThresholdUs = uint64_t{omgp::TRUNK_offline_after_suspect_ms} * 1000;
    const uint64_t offline_at = suspect_since + kOfflineThresholdUs;
    const MasterEvent idle = loop.host_wire.advance_to(offline_at, loop.master);
    REQUIRE(idle.kind == MasterEvent::None);
    REQUIRE_FALSE(loop.master.busy());
    tracker.tick(loop.clock.now_us());
    REQUIRE(tracker.state(kNode) == HealthState::OFFLINE);
    return suspect_since;
}

TEST_CASE("SC-005 SUSPECT: three consecutive failed transactions drive a real HealthTracker "
          "from ENROLLED to SUSPECT",
          "[link]") {
    Loop loop;
    RecordingHealthListener listener;
    HealthTracker tracker(loop.clock, listener);

    drive_to_suspect(loop, tracker);

    REQUIRE(tracker.state(kNode) == HealthState::SUSPECT);
    REQUIRE(listener.entries.size() == 2 + kDeclareNotices);
    REQUIRE(listener.entries[0].notice == Notice::ENROLLED);
    REQUIRE(listener.entries[0].addr == kNode);
    REQUIRE(listener.entries[1].notice == Notice::SUSPECT);
    REQUIRE(listener.entries[1].addr == kNode);
    REQUIRE(listener.entries[2].notice == Notice::BUS_FAULT);
    REQUIRE(listener.entries[3].notice == Notice::ALERT);
    REQUIRE(tracker.bus_fault()); // the rig's only node is SUSPECT: trunk §7's declare rule
}

TEST_CASE("SC-005 OFFLINE: SUSPECT persists past the offline threshold via a real "
          "HealthTracker's tick(), not a fourth failed transaction",
          "[link]") {
    Loop loop;
    RecordingHealthListener listener;
    HealthTracker tracker(loop.clock, listener);

    drive_to_offline(loop, tracker);

    REQUIRE(tracker.state(kNode) == HealthState::OFFLINE);
    REQUIRE(listener.entries.size() == 3 + kDeclareNotices);
    REQUIRE(listener.entries.back().notice == Notice::OFFLINE);
    REQUIRE(listener.entries.back().addr == kNode);
    // The declare came with the SUSPECT transition; ageing to OFFLINE is not a second
    // episode (FR-024: once per episode), so no further bus notice follows.
    REQUIRE(tracker.bus_fault());
}

TEST_CASE("SC-005 RECOVERED: a Respond step after OFFLINE drives a real HealthTracker back to "
          "ENROLLED",
          "[link]") {
    Loop loop;
    RecordingHealthListener listener;
    HealthTracker tracker(loop.clock, listener);

    drive_to_offline(loop, tracker);
    REQUIRE(tracker.state(kNode) == HealthState::OFFLINE);

    const uint8_t payload[] = {0x79};
    const Fault clean[3] = {Fault::Clean, Fault::Clean, Fault::Clean};
    const MasterEvent ev =
        run_health_transaction(loop, tracker, kNode, payload, sizeof payload, clean);
    REQUIRE(ev.kind == MasterEvent::Answered);

    // consecutive_failures is private (see file comment above); state() == ENROLLED is the
    // public proxy for the data-model.md §6 "OFFLINE | ok | ENROLLED (failures = 0) |
    // RECOVERED" row -- the only one this driver-level test can observe.
    REQUIRE(tracker.state(kNode) == HealthState::ENROLLED);
    REQUIRE(listener.entries.size() == 4 + kDeclareNotices + kClearNotices);
    const size_t n = listener.entries.size();
    REQUIRE(listener.entries[n - 2].notice == Notice::RECOVERED);
    REQUIRE(listener.entries[n - 2].addr == kNode);
    // The answer arrived at the rate in use (the reference rate — nothing has changed it),
    // so FR-026 clears the fault at once, on the same result.
    REQUIRE(listener.entries[n - 1].notice == Notice::BUS_RECOVERED);
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);

    // #57 AC3: "the node is ENROLLED again with a zero failure count" -- consecutive_failures
    // has no accessor, so the only public proxy for "reset to 0" (rather than left at 3) is one
    // MORE failed transaction: had OFFLINE->ENROLLED left the count at 3, this single failure
    // would push it to 4 (>= TRUNK_suspect_after_failures) and trip SUSPECT again immediately,
    // per data-model.md §6 "ENROLLED | fail, failures+1 == 3 | SUSPECT". Staying ENROLLED with
    // no new notification ("ENROLLED | fail, failures+1 < 3 | ENROLLED | —") is that proxy
    // (review finding @3b1a80e).
    const uint8_t post_recovery_payload[] = {0x7A};
    const Fault drop_all[3] = {Fault::Drop, Fault::Drop, Fault::Drop};
    const MasterEvent one_more_failure = run_health_transaction(
        loop, tracker, kNode, post_recovery_payload, sizeof post_recovery_payload, drop_all);
    REQUIRE(one_more_failure.kind == MasterEvent::Failed);
    REQUIRE(tracker.state(kNode) == HealthState::ENROLLED);
    REQUIRE(listener.entries.size() == 4 + kDeclareNotices + kClearNotices);
}

// --- FR-015/FR-016: the replay key's SEQUENCE conjunct, exercised end to end --------------
// Review + red team @97d0fb3: in every other cell of this file `f.seq == buffer_.seq` is
// constant-true whenever `buffer_.valid` is, because each cell runs one transaction on a
// fresh Loop and so only one sequence ever exists. `is_replay = f.retry && buffer_.valid`
// would be indistinguishable from the real predicate across the whole matrix -- the per-cell
// `seq` assertions cannot fail for the reason FR-011 names when there is no other
// transaction to be confused with. This cell supplies the second one.

TEST_CASE("SC-004 a retry whose sequence differs from the node's buffered answer is treated "
          "as NEW, not replayed: the handler runs again and the reply carries the new "
          "sequence",
          "[link]") {
    Loop loop;

    // Transaction A: clean, so the Responder's replay buffer ends up holding seq 0.
    const uint8_t a_payload[] = {0x51};
    const Fault clean[3] = {Fault::Clean, Fault::Clean, Fault::Clean};
    const MasterEvent ev_a = run_transaction(loop, kNode, a_payload, sizeof a_payload, clean);
    REQUIRE(ev_a.kind == MasterEvent::Answered);
    REQUIRE(ev_a.response.seq == 0);
    REQUIRE(ev_a.response.len == sizeof a_payload);
    REQUIRE(ev_a.response.payload[0] == a_payload[0]);
    REQUIRE(loop.handler.invocations == 1);

    // Transaction B, seq 1. Its FIRST attempt never reaches the node at all -- the request is
    // lost on the way out, not the response on the way back -- so the node's buffer still
    // holds A's seq 0 when B's retry arrives with the retry bit SET (spec US3 AS5's shape,
    // reached here through the real Master's own retry machinery).
    const uint8_t b_payload[] = {0x52};
    begin_transaction(loop, kNode, b_payload, sizeof b_payload);
    const size_t attempt0_idx = loop.host_wire.transcript_size() - 1;
    const auto attempt0 = loop.host_wire.transcript(attempt0_idx);
    REQUIRE(attempt0.seq == 1);
    REQUIRE_FALSE(attempt0.retry);
    // Not bridged: the node never sees it. Let the Master's own T_resp time it out.
    const uint64_t attempt0_end =
        attempt0.tx_start_us + static_cast<uint64_t>(encode_response(attempt0).size()) * byte_us();
    loop.host_wire.advance_to(attempt0_end + omgp::TRUNK_T_resp_us + omgp::TRUNK_T_gap_us,
                              loop.master);
    REQUIRE(loop.master.attempts() == 2); // the retry has gone out
    REQUIRE(loop.master.stats(kNode).timeouts == 1);

    const auto retry = loop.host_wire.transcript(loop.host_wire.transcript_size() - 1);
    REQUIRE(retry.retry); // trunk §7: same sequence, retry bit set
    REQUIRE(retry.seq == 1);

    // Now bridge that retry to the node, whose buffer holds seq 0 from transaction A.
    const BridgedAttempt r = bridge_latest_request(loop, kNode);
    loop.host_wire.inject_bytes(r.resp_bytes.data(), r.resp_bytes.size(), r.response_start_us);
    const uint64_t full_end =
        r.response_start_us + static_cast<uint64_t>(r.resp_bytes.size()) * byte_us();
    const MasterEvent ev_b = loop.host_wire.advance_to(full_end, loop.master);

    // Treated as NEW: the handler ran a second time and the answer carries B's own sequence.
    // Had the node replayed A's buffered response instead, the handler count would still be
    // 1 and the reply would carry seq 0 -- which is exactly what the `seq` assertions in
    // every other cell cannot distinguish.
    REQUIRE(ev_b.kind == MasterEvent::Answered);
    REQUIRE(ev_b.response.seq == 1);
    // B's OWN payload, not A's. This is the assertion that would catch a replay of A's
    // buffered answer even if the sequence somehow matched -- content, not just header.
    REQUIRE(ev_b.response.len == sizeof b_payload);
    REQUIRE(ev_b.response.payload[0] == b_payload[0]);
    REQUIRE(loop.handler.invocations == 2);
    REQUIRE(loop.responder.stats().replays_served == 0);
    REQUIRE(loop.responder.stats().transactions == 2);
}

// --- FR-011: the accepted response's sequence, given something to reject -------------------
// Red team @410ce9f finding 1: every cell asserts `ev.response.seq == <the transaction's>`,
// but no cell ever puts a response with a DIFFERENT sequence on the wire, so those 18
// assertions could not fail -- the Master would have to accept a mismatched answer first.
// This cell supplies the mismatch, in the window, where the acceptance check actually runs.

TEST_CASE("SC-004 a response whose sequence is not the open transaction's is rejected inside "
          "the window; the transaction is still awaiting, and the right answer is accepted",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x61};
    begin_transaction(loop, kNode, payload, sizeof payload);
    const BridgedAttempt a = bridge_latest_request(loop, kNode);
    REQUIRE(a.resp.seq == 0); // the open transaction's own sequence

    // The node's real answer, re-encoded with a NEIGHBOURING sequence and delivered inside the
    // window: everything else about it is right, which is what makes it the interesting case.
    const std::vector<uint8_t> wrong_seq =
        encode_expected(a.resp.dst, a.resp.src, a.resp.response, a.resp.retry,
                        static_cast<uint8_t>((a.resp.seq + 1) & 0x0F), a.resp.payload, a.resp.len);
    loop.host_wire.inject_bytes(wrong_seq.data(), wrong_seq.size(), a.response_start_us);
    const uint64_t wrong_end =
        a.response_start_us + static_cast<uint64_t>(wrong_seq.size()) * byte_us();
    const MasterEvent rejected = loop.host_wire.advance_to(wrong_end, loop.master);

    // Not accepted, and the transaction is untouched: still open, still one attempt, and the
    // frame charged as a discard (link/master.cpp charges the node whose window is open).
    REQUIRE(rejected.kind == MasterEvent::None);
    REQUIRE(loop.master.busy());
    REQUIRE(loop.master.attempts() == 1);
    REQUIRE(loop.master.stats(kNode).discards == 1);
    REQUIRE(loop.handler.invocations == 1); // the node answered once; nothing re-asked

    // The retry that follows carries the SAME sequence (trunk §7) and its answer is accepted,
    // so the rejection above was about the sequence and not about the frame being unwelcome.
    const uint64_t retry_at = wrong_end + omgp::TRUNK_T_resp_us + omgp::TRUNK_T_gap_us;
    loop.host_wire.advance_to(retry_at, loop.master);
    REQUIRE(loop.master.attempts() == 2);
    const BridgedAttempt b = bridge_latest_request(loop, kNode);
    REQUIRE(b.resp.seq == 0);
    loop.host_wire.inject_bytes(b.resp_bytes.data(), b.resp_bytes.size(), b.response_start_us);
    const MasterEvent ok = loop.host_wire.advance_to(
        b.response_start_us + static_cast<uint64_t>(b.resp_bytes.size()) * byte_us(), loop.master);
    REQUIRE(ok.kind == MasterEvent::Answered);
    REQUIRE(ok.response.seq == 0);
    REQUIRE(ok.response.len == sizeof payload);
    REQUIRE(ok.response.payload[0] == payload[0]);
    REQUIRE_FALSE(loop.master.busy());
}

// --- the payload assertions need a LENGTH dimension --------------------------------------
// Red team @37502f5: every other cell carries a 1-byte payload, so `ev.response.len ==
// sizeof payload` and `payload[0] == …` are both satisfied by a Responder that truncates
// every response to its first byte — the matrix could not tell a full echo from a truncated
// one. This cell carries the longest payload the protocol allows, through a fault path so the
// bytes travel the replay buffer as well as the direct path, and compares all of them.

TEST_CASE("SC-004 a maximum-length payload survives the replay buffer byte-for-byte: a "
          "CRC-corrupted first answer, then the retry's replay, compared in full",
          "[link]") {
    Loop loop;
    // Worst case for the codec as well as for length: every byte is a FLAG or ESCAPE, so the
    // frame is maximally stuffed on both legs of the bridge.
    uint8_t payload[omgp::LIMIT_max_l3_payload];
    for (size_t i = 0; i < sizeof payload; ++i)
        payload[i] = (i % 2 == 0) ? omgp::TRUNK_flag_byte : omgp::TRUNK_escape_byte;

    const Fault plan[3] = {Fault::Corrupt, Fault::Clean, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);

    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(ev.response.seq == 0);
    // The whole payload, not just its first byte: this is the assertion the other cells
    // cannot make, because at length 1 truncation and fidelity are indistinguishable.
    REQUIRE(ev.response.len == sizeof payload);
    REQUIRE(std::memcmp(ev.response.payload, payload, sizeof payload) == 0);
    // It came back through the replay buffer -- the retry was served from the node's stored
    // bytes, not re-encoded -- so this also pins that the buffer holds a full-length frame.
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.responder.stats().replays_served == 1);
    REQUIRE(loop.master.stats(kNode).crc_failures == 1);
    REQUIRE(loop.master.attempts() == 2);
}

// --- SC-005 BUS_FAULT / wrong-rate probe / BUS_RECOVERED: the wrong-rate script (T042) ------
// trunk §7 "Bus health" (amended 2026-09-13, F4): declare once, re-probe alternating from the
// fallback rate, a reference pass after a fallback answer, clear at the rate that worked, no
// automatic return. spec.md FR-024 (declared once; ruling Q2 — one enrolled node counts as
// all), FR-025 (re-probe starts at the fallback rate and alternates; the transport MUST expose
// the rate change), FR-026 (a reference answer clears at once; a fallback answer only after a
// reference pass draws nothing), FR-030/FR-033 (§7 modes from script data, no test-specific
// paths), SC-005, SC-007. data-model.md §6 (while `fault`: poll_due() false everywhere,
// next_probe() includes SUSPECT, the pass yields each enrolled address once at the reference
// rate), §7, §8, §10 (Kind::Rate reads `count` as a bit rate).
//
// The loop-level companion to tests/unit/test_link_busfault.cpp, which pins these rules against
// a HealthTracker alone with no wire at all. What this file adds, and the only reason it exists
// (tasks.md T042): the silence is a real node's, the rate change is observed ON THE WIRE, and
// both go through the real Master.
//
// -- Why this section does not use the bridge above (the mechanism #572 leaves to the
// implementer; recorded here per CLAUDE.md rule 11, with what each choice would have cost) --
// The bridge copies every request onto node_wire and injects the real Responder's answer back
// onto host_wire whatever rate host_wire is running at, so a node deafened by a Kind::Rate step
// would still answer: the deafness would have to come from the DRIVER — a `Fault::Drop` in the
// plan, or a rate test in bridge_latest_request() — which is exactly the test-specific path
// FR-030/FR-033 rule out. And byte_us() is pinned at omgp::TRUNK_bit_rate (it has no reason not
// to be: every bridged cell runs at that one rate), so the bridge would mistime every instant
// of a fallback-rate transaction unless it were made rate-aware too.
// So this section drives the real Master against host_wire's OWN scripted node: Kind::Rate
// deafens it, Kind::Respond answers the one probe it can still hear, and every instant comes
// from byte_time_us(host_wire.bit_rate()) — the rate in force, read from the mock. node_wire,
// the Responder and the CountingHandler are simply unused here (this cell proves nothing about
// the replay buffer, which is SC-004's subject above). Nothing in tests/support/ or link/ is
// touched, byte_us() and bridge_latest_request() are unchanged, and no cell above is affected.

namespace {

// data-model.md §9: a bus-level notice (BUS_FAULT, ALERT, BUS_RECOVERED) carries addr 0.
constexpr uint8_t kBusAddr = 0;

// kNode's own script. Drawn before the 0xFF wildcard, and ONLY by requests the node can hear:
// mock_wire.cpp checks Kind::Rate's standing effect before next_step(), so a deafened node's
// script does not advance. Three steps, one per heard request in the whole case:
//   [0] Respond — the enrolment-rotation probe at omgp::TRUNK_bit_rate: answered, so the node
//                 enrols (UNENROLLED -> ENROLLED). Nothing enrolled means no fault can ever be
//                 declared (FR-023/FR-024), so this step is load-bearing, not scene-setting.
//   [1] Rate    — count == omgp::TRUNK_bit_rate_fallback: from here the node hears only the
//                 fallback rate. It governs the very request that carried it (mock_wire.cpp:
//                 "...and it governs this very request too"), so that status poll is already
//                 unanswered — real silence from a deafened node, not a driver-applied fault.
//                 seed == 0 selects the Rate row's Silence branch: a deaf node that is quiet,
//                 not one that babbles (contracts/mock-wire.md, Rate row).
//   [2] Respond — the alternating probe at the fallback rate: the one request the deafened node
//                 hears again, and the answer FR-026 must NOT let clear the fault.
// Every other request kNode is sent in this case is at the reference rate and unheard, so it
// draws no step at all; the wildcard's Silence steps are never reached on this node's cursor
// (see kAlwaysSilence's budget note above). The 0xFF wildcard is what answers for 0x02..0x0F.
constexpr Step kDeafAtReferenceRate[] = {
    {kNode, Kind::Respond, omgp::TRUNK_T_turn_min_us, 0, 0},
    {kNode, Kind::Rate, omgp::TRUNK_T_turn_min_us, omgp::TRUNK_bit_rate_fallback, 0},
    {kNode, Kind::Respond, omgp::TRUNK_T_turn_min_us, 0, 0},
};
constexpr size_t kDeafAtReferenceRateLen =
    sizeof(kDeafAtReferenceRate) / sizeof(kDeafAtReferenceRate[0]);

// A bound on one whole transaction at `rate`, in that rate's own byte times — never byte_us(),
// which is the reference rate's (data-model.md §4; link/master.hpp for the courtesy cap):
// TRUNK_retries + 1 attempts, each at most a worst-case request frame, one more worst-case
// frame plus T_gap of deferral courtesy, and its T_resp window. Only the poll loop's stopping
// condition: a transaction that concludes returns long before this, and one that never
// concludes fails by name instead of spinning.
uint64_t transaction_bound_us(uint32_t rate) {
    const uint64_t attempt = 2 * static_cast<uint64_t>(kMaxWire) * byte_time_us(rate) +
                             omgp::TRUNK_T_resp_us + 2 * omgp::TRUNK_T_gap_us;
    return (static_cast<uint64_t>(omgp::TRUNK_retries) + 1) * attempt;
}

// One whole transaction driven against host_wire's scripted node, at whatever rate the wire is
// running: begun on the real Master, then polled at the byte cadence OF THAT RATE until the
// engine produces its own terminal event. Time moves only through the shared FakeClock (via
// MockWire::advance_to), and no instant here is a literal or a reference-rate constant.
MasterEvent run_scripted_transaction(Loop& loop, uint8_t dst, uint8_t payload_byte) {
    const uint8_t payload[] = {payload_byte};
    REQUIRE(loop.master.begin(dst, payload, sizeof payload) == Status::Ok);
    const uint64_t step_us = byte_time_us(loop.host_wire.bit_rate());
    const uint64_t give_up_at =
        loop.clock.now_us() + transaction_bound_us(loop.host_wire.bit_rate());
    while (loop.clock.now_us() < give_up_at) {
        const MasterEvent ev =
            loop.host_wire.advance_to(loop.clock.now_us() + step_us, loop.master);
        if (ev.kind != MasterEvent::None)
            return ev;
    }
    FAIL("no terminal MasterEvent within one transaction's worst-case span at this rate");
    return MasterEvent{};
}

// One status poll of a node the tracker says is owed one, driven the way F3 must: poll_due()
// first (never a poll the health rules did not ask for), mark_polled(), the real transaction,
// then exactly one on_result for its outcome at the instant it concluded.
MasterEvent run_status_poll(Loop& loop, HealthTracker& tracker, uint8_t dst, uint8_t payload_byte) {
    REQUIRE(tracker.poll_due(dst, loop.clock.now_us()));
    tracker.mark_polled(dst, loop.clock.now_us());
    const MasterEvent ev = run_scripted_transaction(loop, dst, payload_byte);
    tracker.on_result(dst, ev.kind == MasterEvent::Answered, loop.clock.now_us());
    return ev;
}

struct ProbeRun {
    Probe probe;
    MasterEvent ev;
};

// One probe, issued the way contracts/link-cpp.md "What F3/F4 need" requires of F3: ONE
// next_probe() call per probe actually put on the wire (obligation 2), that probe the only
// traffic while a fault stands (obligation 1 — every transaction in the faulted half of this
// case comes from here), the wire moved to the probe's own rate through the real Master before
// it goes out, and exactly one on_result for its outcome.
// `apply_rate` is false only for the pre-fault enrolment probe: the wire is already at the rate
// next_probe() hands out there, and calling Master::set_bit_rate anyway would count a change on
// Master's own BusStats that nothing decided (link/master.cpp counts applied calls, the tracker
// counts decided changes — data-model.md §8).
ProbeRun run_probe(Loop& loop, HealthTracker& tracker, uint8_t payload_byte, bool apply_rate) {
    const Probe p = tracker.next_probe(loop.clock.now_us());
    // link/health.hpp: ADDR_host is the "nothing to probe" sentinel and must never reach the
    // wire. Unreachable while faulted (every backplane address is a candidate then), checked
    // because this helper issues whatever it is handed.
    REQUIRE(p.addr != omgp::ADDR_host);
    if (apply_rate)
        loop.master.set_bit_rate(p.bit_rate);
    // FR-025: "the transport MUST expose the rate change" — read back FROM THE MOCK, not
    // inferred from the tracker's own bit_rate() or from the Probe we just asked for.
    REQUIRE(loop.host_wire.bit_rate() == p.bit_rate);
    const MasterEvent ev = run_scripted_transaction(loop, p.addr, payload_byte);
    tracker.on_result(p.addr, ev.kind == MasterEvent::Answered, loop.clock.now_us());
    return ProbeRun{p, ev};
}

// Count equality over the WHOLE stream is what FR-024 ("declared once per episode") and SC-006
// ("exactly one notification per transition") need; "at least one" would pass for a tracker
// that re-declared on every probe.
size_t count_notices(const RecordingHealthListener& listener, Notice notice) {
    size_t n = 0;
    for (const RecordingHealthListener::Entry& e : listener.entries)
        if (e.notice == notice)
            ++n;
    return n;
}

// data-model.md §6, amended 2026-09-13: while a fault is declared there are no status polls
// anywhere — only next_probe() drives the wire, so a node that hears only the fallback rate is
// never polled at the reference rate it cannot hear. Every address, not just kNode.
void require_no_polls_due(const HealthTracker& tracker, uint64_t now_us) {
    for (uint8_t a = 0; a < static_cast<uint8_t>(kAddrCount); ++a) {
        INFO("addr " << static_cast<unsigned>(a));
        REQUIRE_FALSE(tracker.poll_due(a, now_us));
    }
}

} // namespace

TEST_CASE("SC-005 BUS_FAULT: a node that hears only the fallback rate falls silent at the "
          "reference rate and declares the bus faulty once; the alternating fallback probe is "
          "answered without clearing it, and the reference pass that then draws nothing "
          "recovers the bus at the fallback rate",
          "[link][timing:bit_rate_fallback]") {
    Loop loop;
    loop.host_wire.set_script(kNode, kDeafAtReferenceRate, kDeafAtReferenceRateLen);
    RecordingHealthListener listener;
    HealthTracker tracker(loop.clock, listener);

    REQUIRE(loop.host_wire.bit_rate() == omgp::TRUNK_bit_rate);
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);

    // --- the node enrols at the reference rate ------------------------------------------
    // Issued as the enrolment-rotation probe it is (data-model.md §6): outside a fault
    // next_probe() hands out the rate in use and walks the rotation cursor, which also fixes
    // where the fault-time rotation below starts from.
    const ProbeRun enrolling = run_probe(loop, tracker, 0x81, /*apply_rate=*/false);
    REQUIRE(enrolling.probe.addr == kNode);
    REQUIRE(enrolling.probe.bit_rate == omgp::TRUNK_bit_rate);
    REQUIRE(enrolling.ev.kind == MasterEvent::Answered);
    REQUIRE(tracker.state(kNode) == HealthState::ENROLLED);
    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(tracker.bus_stats().rate_changes == 0); // nothing has moved the wire yet

    // --- then it stops hearing the trunk, and exactly TRUNK_suspect_after_failures
    // --- consecutive status polls fail ---------------------------------------------------
    // Poll 0 draws the Kind::Rate step, which governs that very request: the node is deaf from
    // it on, so the poll goes unanswered. Polls 1 and 2 draw no step at all — mock_wire.cpp
    // checks the standing effect before next_step(), so a deafened node's script stands still.
    // Each is a real L2 retry exhaustion against a silent node, never a Fault::Drop applied by
    // this driver (FR-030/FR-033: the deafness is script data).
    for (uint32_t i = 0; i < omgp::TRUNK_suspect_after_failures; ++i) {
        INFO("failed poll " << i);
        const MasterEvent ev =
            run_status_poll(loop, tracker, kNode, static_cast<uint8_t>(0x82 + i));
        REQUIRE(ev.kind == MasterEvent::Failed);
        // Silence, not a corrupted or mis-sequenced answer: the node heard nothing to answer.
        REQUIRE(ev.reason == MasterEvent::Timeout);
        REQUIRE(loop.master.attempts() == static_cast<uint8_t>(omgp::TRUNK_retries) + 1);
        if (i + 1 < omgp::TRUNK_suspect_after_failures) {
            REQUIRE(tracker.state(kNode) == HealthState::ENROLLED);
            REQUIRE_FALSE(tracker.bus_fault()); // not before the LAST failure
        }
    }
    REQUIRE(tracker.state(kNode) == HealthState::SUSPECT);
    REQUIRE(loop.master.stats(kNode).timeouts ==
            omgp::TRUNK_suspect_after_failures * (static_cast<uint32_t>(omgp::TRUNK_retries) + 1));

    // --- declared once, at bus level ------------------------------------------------------
    REQUIRE(tracker.bus_fault());
    REQUIRE(count_notices(listener, Notice::BUS_FAULT) == 1); // FR-024: once per episode
    REQUIRE(count_notices(listener, Notice::ALERT) == 1);     // FR-024: one system alert
    REQUIRE(count_notices(listener, Notice::BUS_RECOVERED) == 0);
    REQUIRE(tracker.bus_stats().bus_faults == 1);
    for (const RecordingHealthListener::Entry& e : listener.entries)
        if (e.notice == Notice::BUS_FAULT || e.notice == Notice::ALERT)
            REQUIRE(e.addr == kBusAddr); // data-model.md §9: bus-level notices carry addr 0
    require_no_polls_due(tracker, loop.clock.now_us());

    // --- the re-probe: fallback first, then alternating, each rate applied to the WIRE -----
    // FR-025. The rotation covers all 15 backplane addresses while faulted (nothing is ENROLLED
    // then, and SUSPECT joins the candidate set) and the alternation has period 2, so kNode
    // recurs at the fallback rate within 2 x 15 probes — 15 being odd is what makes the two
    // cycles co-prime, the same bound probe_until() carries in test_link_busfault.cpp.
    constexpr int kProbeCycle = 2 * (omgp::ADDR_backplane_max - omgp::ADDR_backplane_min + 1);
    uint32_t probes_issued = 0;
    ProbeRun answered{};
    bool answered_yet = false;
    for (int i = 0; i < kProbeCycle; ++i) {
        INFO("probe " << i);
        // FR-025: the first probe after the declare is at the FALLBACK rate, and successive
        // probes alternate fallback -> reference -> fallback while no pass is running. Checked
        // per probe rather than for the first three alone, so a tracker that alternated for a
        // while and then stopped would fail here too.
        const uint32_t expected_rate =
            (i % 2 == 0) ? omgp::TRUNK_bit_rate_fallback : omgp::TRUNK_bit_rate;
        const ProbeRun r = run_probe(loop, tracker, static_cast<uint8_t>(0x90 + i), true);
        ++probes_issued;
        REQUIRE(r.probe.bit_rate == expected_rate);
        // data-model.md §8: one increment per change, at the point it is decided. Every probe
        // here changes the rate (it alternates), so the count tracks the probes issued.
        REQUIRE(tracker.bus_stats().rate_changes == probes_issued);
        if (r.probe.addr == kNode && r.probe.bit_rate == omgp::TRUNK_bit_rate_fallback) {
            answered = r;
            answered_yet = true;
            break;
        }
        // Every other probe is to an address no node answers for: the wildcard script is
        // Silence, so the transaction times out and the fault stands.
        REQUIRE(r.ev.kind == MasterEvent::Failed);
        REQUIRE(tracker.bus_fault());
    }
    REQUIRE(answered_yet);
    // ...and it took a WHOLE rotation to come round, so the fallback -> reference -> fallback
    // run the alternation criterion asks for really happened before the answer (a case that
    // was handed kNode back on the first probe would satisfy everything above vacuously).
    // 15 probes, one per backplane address: the enrolment probe above left the cursor on kNode,
    // so kNode comes round again after one full rotation — and kBackplaneCount is odd, which
    // is what puts that probe on the fallback rate rather than the reference one.
    constexpr uint32_t kBackplaneCount =
        static_cast<uint32_t>(omgp::ADDR_backplane_max) - omgp::ADDR_backplane_min + 1;
    REQUIRE(probes_issued == kBackplaneCount);

    // --- the wrong-rate probe is answered, and that does NOT clear the fault (FR-026) ------
    REQUIRE(answered.probe.addr == kNode);
    REQUIRE(answered.probe.bit_rate == omgp::TRUNK_bit_rate_fallback);
    REQUIRE(answered.ev.kind == MasterEvent::Answered);
    REQUIRE(loop.master.attempts() == 1); // heard the first time it was asked at that rate
    REQUIRE(tracker.bus_fault());         // still faulted: a fallback answer is not a recovery
    REQUIRE(count_notices(listener, Notice::BUS_RECOVERED) == 0);
    REQUIRE(tracker.state(kNode) == HealthState::SUSPECT); // its §6 transition is DEFERRED
    // data-model.md §7: the rate in use is assigned only by a clear, so a probe's own rate
    // never moves it — the wire is at the fallback rate, the tracker's rate in use is not.
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate);
    REQUIRE(loop.host_wire.bit_rate() == omgp::TRUNK_bit_rate_fallback);
    require_no_polls_due(tracker, loop.clock.now_us());

    // --- the reference pass draws nothing, and the fault clears at the fallback rate -------
    // FR-026: every enrolled address once at omgp::TRUNK_bit_rate, without alternating. Ruling
    // Q2 makes kNode the whole enrolled set, so the pass is one probe.
    // Labelled per rule 11: the pass probe's RATE alone does not distinguish a pass from the
    // alternation here — after a fallback probe the alternation's next rate is the reference
    // rate too, and with one enrolled address there is no second pass probe to show the
    // absence of alternation. What this cell pins is the pass's OUTCOME, which the alternation
    // has none of: a failing pass probe clears the fault at the fallback rate, where a failed
    // alternating probe clears nothing. Distinguishing the two by rate needs a pass longer than
    // one probe, which is test_link_busfault.cpp's multi-node subject (and #572 rules a second
    // node here out of scope).
    const ProbeRun pass = run_probe(loop, tracker, 0xA0, /*apply_rate=*/true);
    ++probes_issued;
    REQUIRE(pass.probe.addr == kNode);
    REQUIRE(pass.probe.bit_rate == omgp::TRUNK_bit_rate);
    REQUIRE(loop.host_wire.bit_rate() == omgp::TRUNK_bit_rate);
    REQUIRE(pass.ev.kind == MasterEvent::Failed); // the deafened node cannot hear this one

    REQUIRE_FALSE(tracker.bus_fault());
    REQUIRE(count_notices(listener, Notice::BUS_RECOVERED) == 1);
    REQUIRE(tracker.bit_rate() == omgp::TRUNK_bit_rate_fallback); // the rate that works, pinned
    REQUIRE(tracker.state(kNode) == HealthState::ENROLLED);       // the deferred §6 transition
    // No second episode on the way out (FR-024, and data-model.md §7's "no immediate
    // re-declare": the answerer is ENROLLED by the clear, so the declare rule is not satisfied
    // when it is next evaluated).
    REQUIRE(count_notices(listener, Notice::BUS_FAULT) == 1);
    REQUIRE(count_notices(listener, Notice::ALERT) == 1);
    REQUIRE(tracker.bus_stats().bus_faults == 1);

    // The whole notice stream, in order (SC-006: exactly one notification per transition) —
    // which is also what says no BUS_FAULT or ALERT follows the recovery.
    // Three node-level transitions (enrol, suspect, recover) plus this episode's declare and
    // clear notices, counted through the same constants the SC-005 cases above use.
    REQUIRE(listener.entries.size() == 3 + kDeclareNotices + kClearNotices);
    REQUIRE(listener.entries[0].notice == Notice::ENROLLED);
    REQUIRE(listener.entries[0].addr == kNode);
    REQUIRE(listener.entries[1].notice == Notice::SUSPECT);
    REQUIRE(listener.entries[1].addr == kNode);
    REQUIRE(listener.entries[2].notice == Notice::BUS_FAULT);
    REQUIRE(listener.entries[2].addr == kBusAddr);
    REQUIRE(listener.entries[3].notice == Notice::ALERT);
    REQUIRE(listener.entries[3].addr == kBusAddr);
    // The deferred §6 transition the clear applies, and then the clear itself — in that order,
    // and last, so nothing re-declared behind them.
    REQUIRE(listener.entries[4].notice == Notice::RECOVERED);
    REQUIRE(listener.entries[4].addr == kNode);
    REQUIRE(listener.entries[5].notice == Notice::BUS_RECOVERED);
    REQUIRE(listener.entries[5].addr == kBusAddr);

    // data-model.md §7: the clear PINS that rate and there is no automatic return (ruling
    // 2026-09-13), so the layer above moves the wire to what the tracker now reports — the one
    // rate change in this episode that no probe decided.
    loop.master.set_bit_rate(tracker.bit_rate());
    REQUIRE(loop.host_wire.bit_rate() == omgp::TRUNK_bit_rate_fallback);
    // data-model.md §8: one per change decided — each fault-time probe moved the wire (they
    // alternate), and the clear pinned it once more.
    REQUIRE(tracker.bus_stats().rate_changes == probes_issued + 1);
    // Master counts the calls it APPLIED (link/master.cpp), which is the same number here only
    // because every rate this case applied was a change: each probe's, plus the pin above.
    REQUIRE(loop.master.bus_stats().rate_changes == probes_issued + 1);

    // What this case actually put on host_wire, derived rather than counted by hand — the
    // figure kAlwaysSilence's budget note at the top of this file quotes, and the assertion
    // that every transaction here but the two answered ones spent its whole retry budget.
    constexpr size_t kAttempts = static_cast<size_t>(omgp::TRUNK_retries) + 1;
    // One answered enrolment probe; the failed status polls; the fault-time probes nobody
    // answers; the answered fallback probe; the reference pass probe.
    const size_t expected_frames = 1 + omgp::TRUNK_suspect_after_failures * kAttempts +
                                   (kBackplaneCount - 1) * kAttempts + 1 + kAttempts;
    REQUIRE(loop.host_wire.transcript_size() == expected_frames);
}
