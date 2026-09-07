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
//   SUSPECT                -> reserved: T039 (#57) extends this file with the health scripts
//   OFFLINE                -> reserved: T039 (#57), same script as SUSPECT above
//   BUS_FAULT              -> reserved: T042 (#60) extends this file with the wrong-rate script
//   wrong-rate probe       -> reserved: T042 (#60), same script as BUS_FAULT above
#include "catch_amalgamated.hpp"
#include "fake_clock.hpp"
#include "link/crc16.hpp"
#include "link/frame.hpp"
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
// possible transmission this file's tests ever make to one node (generously above the 3
// attempts/transaction x 2 transactions any single TEST_CASE below needs).
constexpr Step kAlwaysSilence[] = {
    {0xFF, Kind::Silence}, {0xFF, Kind::Silence}, {0xFF, Kind::Silence}, {0xFF, Kind::Silence},
    {0xFF, Kind::Silence}, {0xFF, Kind::Silence}, {0xFF, Kind::Silence}, {0xFF, Kind::Silence},
    {0xFF, Kind::Silence}, {0xFF, Kind::Silence}, {0xFF, Kind::Silence}, {0xFF, Kind::Silence},
};

// One real Master + one real Responder (behind a counting handler), each on its own MockWire,
// both sharing a FakeClock — "one MockWire ... each node's MockWire handler being that node's
// Responder" (tasks.md T034), built from MockWire's public surface only (see file comment).
struct Loop {
    FakeClock clock;
    MockWire host_wire{clock};
    MockWire node_wire{clock};
    CountingHandler handler;
    Responder responder;
    Master master;

    Loop()
        : responder(node_wire, clock, handler, kNode), master(host_wire, clock, omgp::ADDR_host) {
        host_wire.set_script(0xFF, kAlwaysSilence,
                             sizeof(kAlwaysSilence) / sizeof(kAlwaysSilence[0]));
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
    const size_t idx = loop.host_wire.transcript_size() - 1;
    const auto req = loop.host_wire.transcript(idx); // copy: stable across further calls
    const auto req_bytes =
        encode_expected(dst, omgp::ADDR_host, false, req.retry, req.seq, req.payload, req.len);
    const uint64_t req_tx_end =
        req.tx_start_us + static_cast<uint64_t>(req_bytes.size()) * byte_us();

    // Bridge the request to the real Responder (see file comment): inject_bytes(), never
    // node_wire.transmit(), so MockWire itself never tries to auto-answer it.
    loop.node_wire.inject_bytes(req_bytes.data(), req_bytes.size(), req.tx_start_us);
    loop.node_wire.advance_to(req_tx_end, loop.responder); // drains the request
    REQUIRE_FALSE(loop.handler.overflowed);
    const uint64_t response_start_us = req_tx_end + omgp::TRUNK_T_turn_min_us; // default
    loop.node_wire.advance_to(response_start_us, loop.responder); // transmits the reply

    const auto resp = loop.node_wire.transcript(loop.node_wire.transcript_size() - 1);
    return BridgedAttempt{req_tx_end, response_start_us, resp, encode_response(resp)};
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
            const MasterEvent ev = loop.host_wire.advance_to(full_end, loop.master);
            const uint64_t dup_end =
                dup_start + static_cast<uint64_t>(resp_bytes.size()) * byte_us();
            const MasterEvent late = loop.host_wire.advance_to(dup_end, loop.master);
            REQUIRE(late.kind == MasterEvent::None);
            REQUIRE_FALSE(loop.master.busy());
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
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == 2);
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.responder.stats().replays_served == 1);
    REQUIRE_FALSE(loop.master.busy());
    REQUIRE(loop.master.stats(kNode).discards == 0); // nothing stray has arrived yet

    // The withheld attempt-0 response was scheduled at kFarFuture (run_transaction) — still
    // in host_wire's RX queue, since nothing has advanced the clock that far yet. Let it
    // arrive now that the transaction has long since concluded: no busy(), no second event,
    // and the frame OBSERVED as a discard charged to its claimed source (link/master.cpp
    // "no transaction at all ... the frame's own claimed source is charged") — the
    // assertion that separates "delivered and ignored" from "never delivered" (red team
    // @fc3fc1a finding 1: without it, deleting the stray from the wire left this cell green).
    const MasterEvent late = loop.host_wire.advance_to(kFarFuture + 10 * byte_us(), loop.master);
    REQUIRE(late.kind == MasterEvent::None);
    REQUIRE_FALSE(loop.master.busy());
    REQUIRE(loop.master.stats(kNode).discards == 1);
}

TEST_CASE("SC-004 delay-past-T_resp through retry 1: recovers on the second retry; the stale "
          "late answer is ignored once it finally arrives",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x23};
    const Fault plan[3] = {Fault::Drop, Fault::DelayPastTResp, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
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
    REQUIRE(loop.master.stats(kNode).discards == 1); // the one stray, observed
}

TEST_CASE("SC-004 delay-past-T_resp through retry 2: uses the full retry budget and still "
          "recovers; both stale late answers are ignored once they finally arrive",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x24};
    const Fault plan[3] = {Fault::DelayPastTResp, Fault::DelayPastTResp, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == static_cast<uint8_t>(omgp::TRUNK_retries) + 1);
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.responder.stats().replays_served == 2);

    // Two strays this time (attempts 0 and 1); advance past the later of the two.
    const MasterEvent late = loop.host_wire.advance_to(
        kFarFuture + 10 * omgp::TRUNK_T_resp_us + 10 * byte_us(), loop.master);
    REQUIRE(late.kind == MasterEvent::None);
    REQUIRE_FALSE(loop.master.busy());
    REQUIRE(loop.master.stats(kNode).discards == 2); // both strays, observed
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
    REQUIRE(loop.master.stats(kNode).discards == 1); // the stray, observed after give-up
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
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == 3);
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.master.stats(kNode).crc_failures == 2);
}

TEST_CASE("SC-004 CRC-corrupted response after give-up: Failed{CrcFailed} after exactly 3 "
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

// --- SC-004: duplicate response -------------------------------------------------------------
// trunk §7 "duplicate": the genuine answer always arrives and the transaction always
// succeeds on it; the extra, late copy is what each cell checks is discarded without effect.
// `handler.invocations == 1` is NOT load-bearing in this row (review @fc3fc1a, LOW): the
// duplicate is of the RESPONSE, so the Responder sees each request once and the replay
// buffer is exercised only by the Drop attempts these plans contain. What each cell here
// pins is that the extra copy was delivered and OBSERVED as a discard — `stats(kNode).
// discards` counts it (link/master.cpp: a frame with no window open is charged to its own
// claimed source) — with no second event and no re-opened transaction (red team @fc3fc1a
// finding 1: without the discard count, deleting the copy from the wire left the row green).

TEST_CASE("SC-004 duplicate at attempt 0: succeeds once; the late duplicate has no effect",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x41};
    const Fault plan[3] = {Fault::Duplicate, Fault::Clean, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == 1); // the genuine answer landed first attempt
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE_FALSE(loop.master.busy());
    REQUIRE(loop.master.stats(kNode).discards == 1); // the late copy, observed
}

TEST_CASE("SC-004 duplicate through retry 1: succeeds once the retry lands; the late "
          "duplicate has no effect",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x42};
    const Fault plan[3] = {Fault::Drop, Fault::Duplicate, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == 2);
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.responder.stats().replays_served == 1);
    REQUIRE_FALSE(loop.master.busy());
    REQUIRE(loop.master.stats(kNode).discards == 1); // the late copy, observed
}

TEST_CASE("SC-004 duplicate through retry 2: uses the full retry budget and still succeeds "
          "once; the late duplicate has no effect",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x43};
    const Fault plan[3] = {Fault::Drop, Fault::Drop, Fault::Duplicate};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == static_cast<uint8_t>(omgp::TRUNK_retries) + 1);
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE(loop.responder.stats().replays_served == 2);
    REQUIRE_FALSE(loop.master.busy());
    REQUIRE(loop.master.stats(kNode).discards == 1); // the late copy, observed
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
    REQUIRE(loop.master.stats(kNode).discards == 2); // both late copies, observed
}

// --- SC-005 "babble": extraneous bus noise between transactions ---------------------------
// trunk §7 "babble" (contracts/mock-wire.md Kind::Babble: bytes regardless of addressee).
// Kind::Babble itself is not yet implemented in MockWire (tasks.md T030, gated behind #48's
// needs-human ruling on widening Step::count — not this issue's to resolve or depend on); a
// fixed, hand-picked byte sequence that is not a valid frame for any node stands in for it,
// injected onto host_wire outside any open transaction's window.

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
        attempt0.tx_start_us + static_cast<uint64_t>(attempt0.len + 8) * byte_us();
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
    REQUIRE(loop.handler.invocations == 2);
    REQUIRE(loop.responder.stats().replays_served == 0);
    REQUIRE(loop.responder.stats().transactions == 2);
}
