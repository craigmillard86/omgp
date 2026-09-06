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
#include "heap_guard.hpp"
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
using omgp_test::corrupt_crc_hi;
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
    size_t handle(const uint8_t* req, size_t len, uint8_t* resp, size_t cap) override {
        ++invocations;
        REQUIRE(len <= cap);
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
MasterEvent run_transaction(Loop& loop, uint8_t dst, const uint8_t* payload, size_t len,
                            const Fault (&plan)[3]) {
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

    for (int attempt = 0; attempt < 3; ++attempt) {
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
        const uint64_t response_start_us = req_tx_end + omgp::TRUNK_T_turn_min_us; // default
        loop.node_wire.advance_to(response_start_us, loop.responder); // transmits the reply

        const auto resp = loop.node_wire.transcript(loop.node_wire.transcript_size() - 1);
        const auto resp_bytes = encode_response(resp);

        const Fault fault = plan[attempt];
        if (fault == Fault::Clean || fault == Fault::Duplicate) {
            loop.host_wire.inject_bytes(resp_bytes.data(), resp_bytes.size(), response_start_us);
            const uint64_t full_end =
                response_start_us + static_cast<uint64_t>(resp_bytes.size()) * byte_us();
            if (fault == Fault::Duplicate) {
                // contracts/mock-wire.md Kind::Duplicate: the real response, then the same
                // bytes again after it (here, after the whole transaction has had time to
                // conclude on the first copy, so it cannot be mistaken for a second genuine
                // answer arriving inside the SAME window).
                loop.host_wire.inject_bytes(resp_bytes.data(), resp_bytes.size(),
                                            full_end + omgp::TRUNK_T_gap_us);
            }
            return loop.host_wire.advance_to(full_end, loop.master);
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

TEST_CASE("SC-004 drop through retry 2: uses the full retry budget and still recovers", "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x13};
    // TRUNK_retries == 2: attempts 0 and 1 both drop, attempt 2 (the last) is answered.
    const Fault plan[3] = {Fault::Drop, Fault::Drop, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(ev.response.seq == 0);
    REQUIRE(loop.master.attempts() == static_cast<uint8_t>(omgp::TRUNK_retries) + 1);
    REQUIRE(loop.handler.invocations == 1);
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
    REQUIRE(loop.handler.invocations == 1);
    REQUIRE_FALSE(loop.master.busy());

    // The withheld attempt-0 response was scheduled at kFarFuture (run_transaction) — still
    // in host_wire's RX queue, since nothing has advanced the clock that far yet. Let it
    // arrive now that the transaction has long since concluded: no busy(), no second event.
    const MasterEvent late = loop.host_wire.advance_to(kFarFuture + 10 * byte_us(), loop.master);
    REQUIRE(late.kind == MasterEvent::None);
    REQUIRE_FALSE(loop.master.busy());
}

TEST_CASE("SC-004 delay-past-T_resp through retry 1: recovers on the second retry; the stale "
          "late answer is ignored once it finally arrives",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x23};
    const Fault plan[3] = {Fault::Drop, Fault::DelayPastTResp, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(loop.master.attempts() == 3);
    REQUIRE(loop.handler.invocations == 1);

    // run_transaction offsets each DelayPastTResp attempt's stray by attempt*10*T_resp so
    // several in one transaction never collide (attempt 1 here: kFarFuture + 10*T_resp).
    const MasterEvent late = loop.host_wire.advance_to(
        kFarFuture + 10 * omgp::TRUNK_T_resp_us + 10 * byte_us(), loop.master);
    REQUIRE(late.kind == MasterEvent::None);
    REQUIRE_FALSE(loop.master.busy());
}

TEST_CASE("SC-004 delay-past-T_resp through retry 2: uses the full retry budget and still "
          "recovers; both stale late answers are ignored once they finally arrive",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x24};
    const Fault plan[3] = {Fault::DelayPastTResp, Fault::DelayPastTResp, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(loop.master.attempts() == static_cast<uint8_t>(omgp::TRUNK_retries) + 1);
    REQUIRE(loop.handler.invocations == 1);

    // Two strays this time (attempts 0 and 1); advance past the later of the two.
    const MasterEvent late = loop.host_wire.advance_to(
        kFarFuture + 10 * omgp::TRUNK_T_resp_us + 10 * byte_us(), loop.master);
    REQUIRE(late.kind == MasterEvent::None);
    REQUIRE_FALSE(loop.master.busy());
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
    REQUIRE(loop.handler.invocations == 1);

    // attempt 2's stray: kFarFuture + 20*T_resp (run_transaction's per-attempt offset).
    const MasterEvent late = loop.host_wire.advance_to(
        kFarFuture + 20 * omgp::TRUNK_T_resp_us + 10 * byte_us(), loop.master);
    REQUIRE(late.kind == MasterEvent::None);
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

TEST_CASE("SC-004 CRC-corrupted response through retry 2: uses the full retry budget and "
          "still recovers",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x33};
    const Fault plan[3] = {Fault::Corrupt, Fault::Corrupt, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(loop.master.attempts() == static_cast<uint8_t>(omgp::TRUNK_retries) + 1);
    REQUIRE(loop.handler.invocations == 1);
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
}

// --- SC-004: duplicate response -------------------------------------------------------------
// trunk §7 "duplicate": the genuine answer always arrives and the transaction always
// succeeds on it; the extra, late copy is what each cell checks is discarded without effect.

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
}

TEST_CASE("SC-004 duplicate through retry 1: succeeds once the retry lands; the late "
          "duplicate has no effect",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x42};
    const Fault plan[3] = {Fault::Drop, Fault::Duplicate, Fault::Clean};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(loop.master.attempts() == 2);
    REQUIRE(loop.handler.invocations == 1);
}

TEST_CASE("SC-004 duplicate through retry 2: uses the full retry budget and still succeeds "
          "once; the late duplicate has no effect",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x43};
    const Fault plan[3] = {Fault::Drop, Fault::Drop, Fault::Duplicate};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(loop.master.attempts() == static_cast<uint8_t>(omgp::TRUNK_retries) + 1);
    REQUIRE(loop.handler.invocations == 1);
}

TEST_CASE("SC-004 duplicate after give-up: a late duplicate of the final, withheld attempt "
          "still has no effect on the already-Failed transaction",
          "[link]") {
    Loop loop;
    const uint8_t payload[] = {0x44};
    const Fault plan[3] = {Fault::Drop, Fault::Drop, Fault::Drop};
    const MasterEvent ev = run_transaction(loop, kNode, payload, sizeof payload, plan);
    REQUIRE(ev.kind == MasterEvent::Failed);
    REQUIRE(loop.handler.invocations == 1);

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
    REQUIRE(loop.handler.invocations == 2);
}
