// Trunk L2 master transaction engine (spec 002 US2, T029). trunk §3 (media access) and §7
// (retry rule); contracts/link-cpp.md "Master engine"; data-model.md §4 "Transaction
// (Master)" and §8 "Statistics"; contracts/mock-wire.md. Originally written from the C++
// contract and spec.md User Story 2 (Acceptance Scenarios 1-7 and the Edge Cases naming
// T_resp/seq-wrap/"simulated time that does not advance") ahead of link/master.{hpp,cpp}
// (T031); link/master.{hpp,cpp} and MockWire's Kind::CrcError/Kind::Duplicate (T029's own
// slice of T030 — Garbage/Babble/Rate remain T030) now implement it, and this suite is
// green. Every assertion is driven through the scripted MockWire + FakeClock harness with
// simulated time advanced explicitly (CLAUDE.md rule 3): nothing here sleeps or reads a
// wall clock.
//
// Scope note (AC6, "wrong src/wrong dst/response-bit-clear" frames): MockWire's own
// Respond/CrcError/Duplicate scheduling (tests/support/mock_wire.cpp, schedule_respond())
// always mirrors the polled request's own src/dst back into the response's dst/src.
// schedule_respond() is reachable directly through MockWire::transmit() (a public
// ByteWire override) by feeding it a hand-encoded "request" with a forged src or dst, with
// no second Responder and no T034 dependency (PR #137 review, MEDIUM — an earlier version
// of this note claimed no such mechanism existed for wrong-dst, which was false). Only the
// response-bit-clear sub-case is genuinely unreachable this way: schedule_respond() always
// sets response = true, so it needs a real second Responder to produce honestly
// (tests/unit/test_link_loop.cpp, T034, which needs T031 first). This file covers the
// three sub-cases reachable today: a wrong/stale SEQUENCE arriving during an open response
// window (a late frame from an earlier, concluded transaction to the SAME node — spec.md
// US2 AC4's scenario); a wrong SRC arriving during a different destination's open window
// (a late Respond from one node queued behind an on-time one to a different node —
// mock_wire.hpp's RX-queue ordering note; needs only Kind::Respond, implemented today);
// and a wrong DST, synthesised by injecting a forged request whose src schedule_respond()
// mirrors into the answer's dst. Every one of these frames must land inside its window
// without overlapping the transaction's own genuine answer, or MockWire's single
// time-sorted RX queue hands the engine both frames byte-interleaved and neither can be
// delivered intact — each test below schedules the genuine answer via an explicit Step
// whose delay starts it strictly after the spoofed frame's last byte.
#include "catch_amalgamated.hpp"
#include "fake_clock.hpp"
#include "heap_guard.hpp"
#include "link/crc16.hpp"
#include "link/frame.hpp"
#include "link/link_types.hpp"
#include "link/master.hpp"
#include "mock_wire.hpp"
#include "omgp_protocol.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace omgp::link;
using omgp_test::FakeClock;
using omgp_test::Kind;
using omgp_test::MockWire;
using omgp_test::Step;

namespace {

// Independently encodes the wire bytes a frame with these fields would produce — used only
// to compute expected byte counts/timing and to cross-check transcript contents, never to
// verify Master's own encoder indirectly through itself.
std::vector<uint8_t> encode_expected(uint8_t f_dst, uint8_t f_src, bool response, bool retry,
                                     uint8_t seq, const uint8_t* payload, size_t len) {
    FrameFields f{f_dst, f_src, response, retry, seq, static_cast<uint8_t>(len), payload};
    uint8_t out[kMaxWire];
    size_t written = 0;
    REQUIRE(encode_frame(f, out, sizeof out, written) == Status::Ok);
    return std::vector<uint8_t>(out, out + written);
}

// The request frame Master is expected to transmit for a (new or retried) transaction to
// `node`.
std::vector<uint8_t> request_bytes(uint8_t node, uint8_t seq, bool retry, const uint8_t* payload,
                                   size_t len) {
    return encode_expected(node, omgp::ADDR_host, false, retry, seq, payload, len);
}

// The response frame a conforming node (or MockWire's default/Respond/CrcError/Duplicate
// echo) sends back.
std::vector<uint8_t> response_bytes(uint8_t node, uint8_t seq, const uint8_t* payload, size_t len) {
    return encode_expected(omgp::ADDR_host, node, true, false, seq, payload, len);
}

uint64_t byte_us() {
    return byte_time_us(omgp::TRUNK_bit_rate);
}

// The instant of the response's final stop bit (research.md R-04: "last byte start + 10
// bit-times at the current rate"), i.e. exactly what MockWire::transmit() would have
// returned had the response itself been transmitted starting at tx_end + delay_us.
uint64_t response_full_end(uint64_t tx_end, uint8_t node, uint8_t seq, const uint8_t* payload,
                           size_t len, uint32_t delay_us = omgp::TRUNK_T_turn_min_us) {
    const uint64_t t0 = tx_end + delay_us;
    const size_t n = response_bytes(node, seq, payload, len).size();
    return t0 + static_cast<uint64_t>(n) * byte_us();
}

} // namespace

// --- US2 AC1: happy path -------------------------------------------------------------

TEST_CASE("idle bus, node answers inside the response window: Answered with the payload, "
          "no retry",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    Master master(wire, clock, omgp::ADDR_host);

    const uint8_t dst = 0x01;
    const uint8_t payload[] = {0xAA, 0xBB};

    Status st{};
    HEAP_FREE_SCOPE({ st = master.begin(dst, payload, sizeof payload); });
    REQUIRE(st == Status::Ok);
    REQUIRE(master.busy());
    REQUIRE(master.attempts() == 1);

    REQUIRE(wire.transcript_size() == 1);
    const auto& req = wire.transcript(0);
    REQUIRE(req.dst == dst);
    REQUIRE(req.src == omgp::ADDR_host);
    REQUIRE_FALSE(req.response);
    REQUIRE_FALSE(req.retry);
    REQUIRE(req.seq == 0);
    REQUIRE(req.len == sizeof payload);
    REQUIRE(std::memcmp(req.payload, payload, sizeof payload) == 0);

    const uint64_t tx_end =
        req.tx_start_us +
        static_cast<uint64_t>(request_bytes(dst, 0, false, payload, sizeof payload).size()) * byte_us();
    const uint64_t full_end = response_full_end(tx_end, dst, 0, payload, sizeof payload);

    // advance_to(t) (clock-only) runs INFO/REQUIRE (Catch2 macros that allocate) and must
    // stay outside the guarded region below, or the guard measures Catch2's own
    // allocations instead of Master's (PR #137 review) — only poll() itself, a bare
    // engine call with no Catch2 macro inside, is measured.
    wire.advance_to(full_end);
    MasterEvent ev{};
    HEAP_FREE_SCOPE({ ev = master.poll(full_end); });
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(ev.response.len == sizeof payload);
    REQUIRE(std::memcmp(ev.response.payload, payload, sizeof payload) == 0);
    REQUIRE(master.stats(dst).transactions == 1);
    REQUIRE(master.stats(dst).retries == 0);
    REQUIRE_FALSE(master.busy());
}

// --- US2 AC2: silence -> two retries -> Failed{Timeout} -------------------------------

TEST_CASE("silence for the full response window triggers exactly two retries (same seq, "
          "retry set), then Failed{Timeout}",
          "[timing:retries]") {
    FakeClock clock;
    MockWire wire(clock);
    const uint8_t dst = 0x02;
    const Step silence[] = {{dst, Kind::Silence}, {dst, Kind::Silence}, {dst, Kind::Silence}};
    wire.set_script(dst, silence, 3);
    Master master(wire, clock, omgp::ADDR_host);

    const uint8_t payload[] = {0x42};
    REQUIRE(master.begin(dst, payload, sizeof payload) == Status::Ok);

    uint64_t tx_end =
        static_cast<uint64_t>(request_bytes(dst, 0, false, payload, sizeof payload).size()) * byte_us();

    for (uint32_t attempt = 0; attempt < omgp::TRUNK_retries; ++attempt) {
        const uint64_t timeout_instant = tx_end + omgp::TRUNK_T_resp_us;
        const uint64_t retry_tx_start = timeout_instant + omgp::TRUNK_T_gap_us;

        MasterEvent ev = wire.advance_to(retry_tx_start, master);
        REQUIRE(ev.kind == MasterEvent::None); // retries remain: not yet terminal
        REQUIRE(wire.transcript_size() == static_cast<size_t>(attempt) + 2);
        const auto& retry_rec = wire.transcript(static_cast<size_t>(attempt) + 1);
        REQUIRE(retry_rec.retry);
        REQUIRE(retry_rec.seq == 0); // a retry never advances seq
        REQUIRE(retry_rec.tx_start_us == retry_tx_start);

        tx_end = retry_tx_start + static_cast<uint64_t>(
                                       request_bytes(dst, 0, true, payload, sizeof payload).size()) *
                                       byte_us();
    }

    // Third (final) timeout: no attempts remain -> terminal Failed{Timeout}, no fourth
    // transmission.
    MasterEvent ev = wire.advance_to(tx_end + omgp::TRUNK_T_resp_us, master);
    REQUIRE(ev.kind == MasterEvent::Failed);
    REQUIRE(ev.reason == MasterEvent::Timeout);
    REQUIRE(master.attempts() == 3);
    REQUIRE(wire.transcript_size() == 3); // initial + exactly two retries, no third retry
    REQUIRE(master.stats(dst).retries == omgp::TRUNK_retries);
    REQUIRE(master.stats(dst).timeouts == 3);
    REQUIRE_FALSE(master.busy());
}

// --- A terminal Failed{Timeout} still counts as a concluded transaction, clears busy(),
// and still gap-defers the very next begin() from its own instant --------------------

TEST_CASE("after a terminal Failed{Timeout}, transactions is counted, busy() clears, and the "
          "next begin() is still gap-deferred from the failure's own instant",
          "[timing:retries]") {
    FakeClock clock;
    MockWire wire(clock);
    const uint8_t dst = 0x0F;
    const Step silence[] = {{dst, Kind::Silence}, {dst, Kind::Silence}, {dst, Kind::Silence}};
    wire.set_script(dst, silence, 3);
    Master master(wire, clock, omgp::ADDR_host);

    const uint8_t payload[] = {0x55};
    REQUIRE(master.begin(dst, payload, sizeof payload) == Status::Ok);

    uint64_t tx_end =
        static_cast<uint64_t>(request_bytes(dst, 0, false, payload, sizeof payload).size()) * byte_us();
    for (uint32_t attempt = 0; attempt < omgp::TRUNK_retries; ++attempt) {
        const uint64_t retry_tx_start = tx_end + omgp::TRUNK_T_resp_us + omgp::TRUNK_T_gap_us;
        wire.advance_to(retry_tx_start, master);
        tx_end = retry_tx_start + static_cast<uint64_t>(
                                       request_bytes(dst, 0, true, payload, sizeof payload).size()) *
                                       byte_us();
    }

    const uint64_t timeout_instant = tx_end + omgp::TRUNK_T_resp_us;
    MasterEvent ev = wire.advance_to(timeout_instant, master);
    REQUIRE(ev.kind == MasterEvent::Failed);
    REQUIRE(ev.reason == MasterEvent::Timeout);
    REQUIRE(master.stats(dst).transactions == 1); // a failed transaction still concluded
    REQUIRE_FALSE(master.busy());

    // last_activity_/has_last_activity_ must reflect this failure's own instant so the very
    // next begin() is gap-deferred, not transmitted immediately (data-model.md §4 "Gap").
    const uint8_t payload2[] = {0x56};
    REQUIRE(master.begin(dst, payload2, sizeof payload2) == Status::Ok);
    REQUIRE(master.busy());
    REQUIRE(wire.transcript_size() == 3); // not yet on the wire

    wire.advance_to(timeout_instant + omgp::TRUNK_T_gap_us - 1, master);
    REQUIRE(wire.transcript_size() == 3); // not one microsecond early

    ev = wire.advance_to(timeout_instant + omgp::TRUNK_T_gap_us, master);
    REQUIRE(wire.transcript_size() == 4);
    REQUIRE(wire.transcript(3).tx_start_us == timeout_instant + omgp::TRUNK_T_gap_us);
    REQUIRE(wire.transcript(3).seq == 1); // a new transaction, not a retry
    REQUIRE_FALSE(wire.transcript(3).retry);
}

// --- Edge case: the T_resp window is exclusive at its end ----------------------------

TEST_CASE("a response at tx_end + T_resp - 1 is accepted; at tx_end + T_resp it is missed",
          "[timing:T_resp]") {
    FakeClock clock;
    MockWire wire(clock);
    const uint8_t dst = 0x0A;
    const uint8_t payload[] = {0x03};
    // Hoisted beside `wire`, not SECTION-local: mock_wire.hpp requires `steps` to outlive
    // the MockWire that stores them, and `wire` (declared above, at TEST_CASE scope)
    // outlives either SECTION body (PR #137 review).
    const Step accept[] = {{dst, Kind::Respond, omgp::TRUNK_T_resp_us - 1}};
    const Step miss[] = {{dst, Kind::Respond, omgp::TRUNK_T_resp_us}};

    SECTION("accepted just inside the window") {
        wire.set_script(dst, accept, 1);
        Master master(wire, clock, omgp::ADDR_host);

        REQUIRE(master.begin(dst, payload, sizeof payload) == Status::Ok);
        const uint64_t tx_end = static_cast<uint64_t>(
            request_bytes(dst, 0, false, payload, sizeof payload).size() * byte_us());
        const uint64_t full_end =
            response_full_end(tx_end, dst, 0, payload, sizeof payload, omgp::TRUNK_T_resp_us - 1);

        MasterEvent ev = wire.advance_to(full_end, master);
        REQUIRE(ev.kind == MasterEvent::Answered);
    }

    SECTION("missed exactly at the window boundary") {
        wire.set_script(dst, miss, 1);
        Master master(wire, clock, omgp::ADDR_host);

        REQUIRE(master.begin(dst, payload, sizeof payload) == Status::Ok);
        const uint64_t tx_end = static_cast<uint64_t>(
            request_bytes(dst, 0, false, payload, sizeof payload).size() * byte_us());

        MasterEvent ev = wire.advance_to(tx_end + omgp::TRUNK_T_resp_us, master);
        REQUIRE(ev.kind != MasterEvent::Answered);
        REQUIRE(master.stats(dst).timeouts == 1);
    }
}

// --- Edge case: a genuine (correctly-addressed) response opening exactly AT the final
// attempt's T_resp deadline is still outside the window -------------------------------

TEST_CASE("a genuine response whose opening FLAG lands exactly at the final attempt's T_resp "
          "deadline is discarded as outside the window, then concludes Failed{Timeout}, never "
          "Answered",
          "[timing:T_resp]") {
    FakeClock clock;
    MockWire wire(clock);
    const uint8_t dst = 0x03;
    const uint8_t payload[] = {0x2A};
    const Step script[] = {
        {dst, Kind::Silence},
        {dst, Kind::Silence},
        {dst, Kind::Respond, omgp::TRUNK_T_resp_us},
    };
    wire.set_script(dst, script, 3);
    Master master(wire, clock, omgp::ADDR_host);

    REQUIRE(master.begin(dst, payload, sizeof payload) == Status::Ok);

    uint64_t tx_end =
        static_cast<uint64_t>(request_bytes(dst, 0, false, payload, sizeof payload).size()) * byte_us();
    for (uint32_t attempt = 0; attempt < omgp::TRUNK_retries; ++attempt) {
        const uint64_t retry_tx_start = tx_end + omgp::TRUNK_T_resp_us + omgp::TRUNK_T_gap_us;
        MasterEvent retry_ev = wire.advance_to(retry_tx_start, master);
        REQUIRE(retry_ev.kind == MasterEvent::None);
        tx_end = retry_tx_start + static_cast<uint64_t>(
                                       request_bytes(dst, 0, true, payload, sizeof payload).size()) *
                                       byte_us();
    }

    // The final (3rd, no-retries-left) attempt's own response fires exactly at ITS T_resp
    // boundary: the opening FLAG's instant equals deadline_ exactly, which the exclusive
    // (`<`) window check (and a correctly-tracked resp_open_us_) must treat as OUTSIDE the
    // window (research.md R-04 / the T_resp-boundary edge case above), not as a genuine
    // answer — even though the frame is otherwise perfectly well-formed and on time enough
    // to be fully delivered.
    const uint64_t final_deadline = tx_end + omgp::TRUNK_T_resp_us;
    const uint64_t full_end =
        response_full_end(tx_end, dst, 0, payload, sizeof payload, omgp::TRUNK_T_resp_us);
    REQUIRE(full_end > final_deadline); // sanity: a real frame outlasts a single instant

    const uint32_t discards_before = master.stats(dst).discards;
    MasterEvent ev = wire.advance_to(full_end, master);
    REQUIRE(ev.kind == MasterEvent::Failed);
    REQUIRE(ev.reason == MasterEvent::Timeout);
    REQUIRE(master.stats(dst).discards == discards_before + 1);
    REQUIRE_FALSE(master.busy());
}

// --- US2 AC3: a CRC-failed response ends the attempt immediately ---------------------

TEST_CASE("a CRC-failed response ends the attempt immediately, without waiting out the "
          "remaining timeout",
          "[link]") {
    // Kind::CrcError is implemented in mock_wire.cpp (T029's own slice of T030): the real
    // response with its last CRC byte XOR 0xFF, per contracts/mock-wire.md.
    FakeClock clock;
    MockWire wire(clock);
    const uint8_t dst = 0x08;
    const Step crc[] = {{dst, Kind::CrcError}};
    wire.set_script(dst, crc, 1);
    Master master(wire, clock, omgp::ADDR_host);

    const uint8_t payload[] = {0x11};
    REQUIRE(master.begin(dst, payload, sizeof payload) == Status::Ok);
    REQUIRE(wire.transcript_size() == 1);

    const uint64_t tx_end = static_cast<uint64_t>(
        request_bytes(dst, 0, false, payload, sizeof payload).size() * byte_us());
    const uint64_t crc_full_end = response_full_end(tx_end, dst, 0, payload, sizeof payload);
    // Sanity: the corrupted response completes well inside attempt 0's own window - if it
    // didn't, this test couldn't distinguish "ends immediately" from "waits for T_resp".
    REQUIRE(crc_full_end < tx_end + omgp::TRUNK_T_resp_us);

    MasterEvent ev = wire.advance_to(crc_full_end, master);
    REQUIRE(ev.kind == MasterEvent::None); // attempt 0 over, but retries remain
    REQUIRE(master.stats(dst).crc_failures == 1);
    REQUIRE(wire.transcript_size() == 1); // the retry is gap-deferred, not yet on the wire

    // The retry fires at last_activity (the CRC failure's own end) + T_gap - not at
    // tx_end + T_resp + T_gap, which is what waiting out the timeout would produce.
    ev = wire.advance_to(crc_full_end + omgp::TRUNK_T_gap_us - 1, master);
    REQUIRE(wire.transcript_size() == 1); // not one microsecond early
    ev = wire.advance_to(crc_full_end + omgp::TRUNK_T_gap_us, master);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(wire.transcript(1).retry);
    REQUIRE(wire.transcript(1).seq == 0);
    REQUIRE(wire.transcript(1).tx_start_us == crc_full_end + omgp::TRUNK_T_gap_us);
}

TEST_CASE("a CrcError response's wire length matches the real response's even when the "
          "real CRC high byte would need byte-stuffing",
          "[link]") {
    // PR #137 review, MEDIUM: mock_wire.cpp's encode_crc_corrupted() used to XOR the real
    // CRC high byte with 0xFF unconditionally. For a real high byte of
    // TRUNK_flag_byte ^ 0xFF (0x81), the corrupted byte lands exactly on TRUNK_flag_byte
    // (0x7E) and needs an extra stuffing byte the real, uncorrupted response never would -
    // making the corrupted frame one byte_time_us LONGER on the wire than
    // response_full_end() below (which, like tests/unit/test_link_master.cpp throughout,
    // derives its expected instant from the UNCORRUPTED response's own encode_frame
    // length) predicts.
    //
    // Found via the engine's own CRC implementation rather than a hand-picked magic byte,
    // so this keeps reproducing the exact collision class if the CRC polynomial or frame
    // layout ever changes.
    uint8_t dst = 0, payload_byte = 0;
    bool found = false;
    for (uint16_t d = 1; d < static_cast<uint16_t>(kAddrCount) && !found; ++d) {
        for (uint16_t p = 0; p < 256 && !found; ++p) {
            const uint8_t unstuffed[] = {omgp::ADDR_host, static_cast<uint8_t>(d),
                                        /*ctrl: response=1, retry=0, seq=0*/ 0x01, 0x01,
                                        static_cast<uint8_t>(p)};
            const uint16_t c = omgp::crc16_ccitt_false(unstuffed, sizeof unstuffed);
            if (static_cast<uint8_t>((c >> 8) & 0xFF) ==
                static_cast<uint8_t>(omgp::TRUNK_flag_byte ^ 0xFF)) {
                dst = static_cast<uint8_t>(d);
                payload_byte = static_cast<uint8_t>(p);
                found = true;
            }
        }
    }
    REQUIRE(found); // a 15*256-combination sweep always finds this collision class

    FakeClock clock;
    MockWire wire(clock);
    const Step crc[] = {{dst, Kind::CrcError}};
    wire.set_script(dst, crc, 1);
    Master master(wire, clock, omgp::ADDR_host);

    const uint8_t payload[] = {payload_byte};
    REQUIRE(master.begin(dst, payload, sizeof payload) == Status::Ok);

    const uint64_t tx_end = static_cast<uint64_t>(
        request_bytes(dst, 0, false, payload, sizeof payload).size() * byte_us());
    const uint64_t crc_full_end = response_full_end(tx_end, dst, 0, payload, sizeof payload);
    REQUIRE(crc_full_end < tx_end + omgp::TRUNK_T_resp_us);

    // Checked one byte_time_us early (crc_full_end - 1), not at crc_full_end itself:
    // MockWire's RX queue releases a byte once clock_.now_us() reaches its own START
    // instant (mock_wire.cpp receive(): start_us <= now), not its completion instant, so
    // the reference (uncorrupted) frame's own closing FLAG - whose start precedes
    // crc_full_end by exactly one byte_time_us - is already available at crc_full_end - 1.
    // A corrupted frame with an EXTRA byte (the length-changing bug this fix prevents)
    // would instead place its true closing FLAG's start at crc_full_end, still unavailable
    // one microsecond earlier - the only instant this test can tell the two apart at.
    MasterEvent ev = wire.advance_to(crc_full_end - 1, master);
    REQUIRE(ev.kind == MasterEvent::None); // attempt 0 over, but retries remain
    REQUIRE(master.stats(dst).crc_failures == 1);
}

// --- US2 AC4 + AC6 (stale-seq sub-case): late duplicate discarded, next transaction unaffected

TEST_CASE("a late duplicate of a concluded transaction's response is discarded during the "
          "next transaction's open window, without ending it or corrupting its counters",
          "[link]") {
    // Kind::Duplicate is implemented in mock_wire.cpp (T029's own slice of T030): the real
    // response, then the same bytes again, per contracts/mock-wire.md.
    FakeClock clock;
    MockWire wire(clock);
    const uint8_t dst = 0x09;
    const uint8_t payload0[] = {0x01};
    const uint8_t payload1[] = {0x02};

    // Transaction N (seq 0): contracts/mock-wire.md's Duplicate step answers promptly (the
    // "real response", at the ordinary default T_turn_min_us delay - transaction N
    // succeeds immediately, no retry) and then repeats the same bytes delay_us after that
    // first copy ends; the repeat is the late duplicate this case proves gets discarded.
    // Every timestamp below is computed from the frame fields alone (independent of any
    // engine state), so delay_us can be chosen, before driving anything, to land the
    // repeat inside transaction N+1's window rather than transaction N's own.
    const uint64_t tx_end0 = static_cast<uint64_t>(
        request_bytes(dst, 0, false, payload0, sizeof payload0).size() * byte_us());
    const uint64_t full_end0 = response_full_end(tx_end0, dst, 0, payload0, sizeof payload0);
    const uint64_t tx1_start = full_end0 + omgp::TRUNK_T_gap_us;
    const uint64_t tx1_end =
        tx1_start +
        static_cast<uint64_t>(request_bytes(dst, 1, false, payload1, sizeof payload1).size()) * byte_us();

    // The repeat starts at full_end0 + delay_us (contracts/mock-wire.md: "the same bytes
    // again delay_us after the first ends"); choosing delay_us = (tx1_end - full_end0) +
    // margin lands it just after N+1 transmits.
    const uint32_t margin = 10;
    const uint32_t duplicate_delay_us = static_cast<uint32_t>(tx1_end - full_end0) + margin;
    // N+1's own answer is deliberately delayed past the repeated copy's own full length (a
    // real, 9-byte-on-the-wire frame takes far longer than TRUNK_T_turn_min_us to
    // transmit) so the two never interleave byte-for-byte on the shared MockWire queue —
    // exactly as two genuine transmitters could never overlap on a real half-duplex bus.
    const uint32_t tx1_answer_delay_us = 110; // stale copy (10..100 past tx1_end) fully clear
    const Step tx0_script[] = {
        {dst, Kind::Duplicate, duplicate_delay_us},
        {dst, Kind::Respond, tx1_answer_delay_us},
    };
    wire.set_script(dst, tx0_script, 2);

    Master master(wire, clock, omgp::ADDR_host);
    REQUIRE(master.begin(dst, payload0, sizeof payload0) == Status::Ok);

    MasterEvent ev = wire.advance_to(full_end0, master);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(master.stats(dst).transactions == 1);
    REQUIRE(master.stats(dst).retries == 0);
    REQUIRE_FALSE(master.busy());

    // Transaction N+1 (seq 1), same destination: issued the instant N concludes, well
    // before last_activity (== full_end0) + T_gap; draws the script's second (explicitly
    // delayed) Respond step.
    REQUIRE(master.begin(dst, payload1, sizeof payload1) == Status::Ok);
    REQUIRE(master.busy());
    REQUIRE(wire.transcript_size() == 1); // not yet on the wire

    wire.advance_to(tx1_start - 1, master);
    REQUIRE(wire.transcript_size() == 1); // not one microsecond early

    ev = wire.advance_to(tx1_start, master);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(wire.transcript(1).seq == 1);
    REQUIRE_FALSE(wire.transcript(1).retry);

    const uint64_t stale_copy_start = full_end0 + duplicate_delay_us;
    const size_t resp0_n = response_bytes(dst, 0, payload0, sizeof payload0).size();
    const uint64_t stale_copy_end = stale_copy_start + resp0_n * byte_us();
    // Sanity: this test isn't vacuous - the stale frame really does land inside N+1's open
    // window, not before N+1 transmitted or after N+1's own window would already have closed.
    REQUIRE(stale_copy_start > tx1_end);
    REQUIRE(stale_copy_end < tx1_end + omgp::TRUNK_T_resp_us);

    const uint32_t discards_before = master.stats(dst).discards;
    ev = wire.advance_to(stale_copy_end, master);
    REQUIRE(ev.kind == MasterEvent::None); // wrong seq (0, not N+1's 1): discarded silently
    REQUIRE(master.stats(dst).discards == discards_before + 1);
    REQUIRE(master.busy()); // N+1's window keeps running

    const uint64_t full_end1 =
        response_full_end(tx1_end, dst, 1, payload1, sizeof payload1, tx1_answer_delay_us);
    ev = wire.advance_to(full_end1, master);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(master.stats(dst).transactions == 2);
    REQUIRE(master.stats(dst).retries == 0); // unaffected by the stale frame
}

// --- Acceptance window has a lower bound, not just T_resp's upper one ------------------

TEST_CASE("a same-seq response that arrived just after attempt 0's window closed is not "
          "mistaken for the retry's own answer once it is finally drained",
          "[link]") {
    // PR #137 review, MEDIUM: contracts/link-cpp.md's acceptance window is
    // [tx_end, tx_end + T_resp), but master.cpp only checked the upper bound
    // (frame_open_us < deadline_). poll() stops draining the wire once an attempt times out
    // (sub_phase_ leaves AwaitResponse), so a response that misses attempt 0's window by a
    // few microseconds sits queued, undrained, until the gap-deferred retry re-enters
    // AwaitResponse - at which point it is the FIRST thing the retry's own poll() drains.
    // Retries reuse the same seq (trunk §7), so src/dst/seq/response-bit all "match"; only a
    // lower bound on frame_open_us can tell attempt 0's late answer apart from the retry's.
    FakeClock clock;
    MockWire wire(clock);
    const uint8_t dst = 0x0C;
    const uint8_t payload[] = {0x55};
    // Arrives TRUNK_T_resp_us + margin after tx_end: just past attempt 0's window close.
    const uint32_t late_delay_us = omgp::TRUNK_T_resp_us + 5;
    const Step attempt0[] = {{dst, Kind::Respond, late_delay_us}};
    wire.set_script(dst, attempt0, 1);
    Master master(wire, clock, omgp::ADDR_host);

    REQUIRE(master.begin(dst, payload, sizeof payload) == Status::Ok);
    const uint64_t tx_end0 =
        static_cast<uint64_t>(request_bytes(dst, 0, false, payload, sizeof payload).size()) * byte_us();
    const uint64_t stale_full_end = response_full_end(tx_end0, dst, 0, payload, sizeof payload,
                                                       late_delay_us);

    const uint64_t timeout_instant = tx_end0 + omgp::TRUNK_T_resp_us;
    const uint64_t retry_tx_start = timeout_instant + omgp::TRUNK_T_gap_us;
    const uint64_t tx_end1 =
        retry_tx_start +
        static_cast<uint64_t>(request_bytes(dst, 0, true, payload, sizeof payload).size()) * byte_us();
    // Sanity: the stale frame is fully on the wire (and thus fully queued) before the retry
    // even transmits - this test is about drain ORDER within the retry's window, not about
    // the stale bytes still arriving mid-retry.
    REQUIRE(stale_full_end < tx_end1);

    MasterEvent ev = wire.advance_to(retry_tx_start, master); // attempt 0 times out, retry fires
    REQUIRE(ev.kind == MasterEvent::None);
    REQUIRE(master.stats(dst).timeouts == 1);
    REQUIRE(wire.transcript_size() == 2);

    // The node's script is exhausted, so its retry request gets MockWire's default Respond
    // (T_turn_min_us delay) - the retry's genuine answer, landing well after the stale frame.
    const uint64_t genuine_full_end = response_full_end(tx_end1, dst, 0, payload, sizeof payload);

    // Drain just past the stale frame's own end: with only an upper bound, its frame_open_us
    // (attempt 0's era, long before the retry's own deadline_) wrongly satisfies
    // "< deadline_" and this poll() answers the transaction on the spot, using bytes that
    // were never addressed to this attempt.
    ev = wire.advance_to(tx_end1 + 1, master);
    REQUIRE(ev.kind == MasterEvent::None); // correctly discarded, not Answered
    REQUIRE(master.stats(dst).discards == 1);
    REQUIRE(master.busy()); // the retry's own window is still open

    ev = wire.advance_to(genuine_full_end, master);
    REQUIRE(ev.kind == MasterEvent::Answered); // the retry's real answer, drained afterward
    REQUIRE(master.stats(dst).transactions == 1);
    REQUIRE(master.stats(dst).retries == 1);
    REQUIRE_FALSE(master.busy());
}

// --- The acceptance window's lower bound is inclusive -----------------------------------

TEST_CASE("a response whose opening FLAG lands exactly at tx_end, the window's own lower "
          "bound, is accepted rather than discarded",
          "[link]") {
    // contracts/link-cpp.md: the acceptance window is [tx_end, tx_end + T_resp) - inclusive at
    // the lower bound. window_start_us_ is set to tx_end in do_transmit(); a response with zero
    // turnaround delay opens its FLAG at that very instant, the one value a `>` in place of
    // `>=` on the lower-bound check would wrongly reject.
    FakeClock clock;
    MockWire wire(clock);
    const uint8_t dst = 0x0E;
    const uint8_t payload[] = {0x77};
    const Step immediate[] = {{dst, Kind::Respond, 0}};
    wire.set_script(dst, immediate, 1);
    Master master(wire, clock, omgp::ADDR_host);

    REQUIRE(master.begin(dst, payload, sizeof payload) == Status::Ok);
    const uint64_t tx_end =
        static_cast<uint64_t>(request_bytes(dst, 0, false, payload, sizeof payload).size()) * byte_us();
    const uint64_t full_end = response_full_end(tx_end, dst, 0, payload, sizeof payload, 0);

    MasterEvent ev = wire.advance_to(full_end, master);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(master.stats(dst).discards == 0);
}

// --- US2 AC6 (wrong-src sub-case): a stale response from a different, already-concluded
// destination is discarded during another destination's open window -------------------

TEST_CASE("a late response from a different, already-concluded destination (wrong src) "
          "arriving during another transaction's open window is discarded, without "
          "ending it or corrupting its counters",
          "[link]") {
    // mock_wire.hpp: the RX queue is sorted by start_us precisely for "a late Respond
    // queued behind an on-time one from a different node" — this drives exactly that
    // case with only Kind::Respond, implemented today (neither T030 nor T034 needed).
    FakeClock clock;
    MockWire wire(clock);
    const uint8_t other = 0x0C; // "B": answers late on attempt 0; its retry answers on time
    const uint8_t dst = 0x0D;   // "A": the transaction whose window receives B's stale frame
    const uint8_t other_payload[] = {0x05};
    const uint8_t payload[] = {0x06};

    // B's attempt 0: a Respond step schedules a real answer, but so late (delay_us below)
    // that it lands well past dst A's transaction, not B's own T_resp window - so B's
    // attempt 0 times out. The script is then exhausted, so B's retry (attempt 1) falls
    // back to the default prompt Respond and answers on time; B's transaction concludes
    // there. The original, overdue attempt-0 answer is unaffected by that: MockWire
    // schedules each step against the request that consumed it (mock_wire.cpp,
    // schedule_respond()), not against whichever attempt is open when it later fires.
    const uint64_t other_tx_end0 = static_cast<uint64_t>(
        request_bytes(other, 0, false, other_payload, sizeof other_payload).size() * byte_us());
    const uint64_t other_retry_tx_start = other_tx_end0 + omgp::TRUNK_T_resp_us + omgp::TRUNK_T_gap_us;
    const uint64_t other_retry_tx_end =
        other_retry_tx_start + static_cast<uint64_t>(
                                    request_bytes(other, 0, true, other_payload,
                                                  sizeof other_payload)
                                        .size()) *
                                    byte_us();
    const uint64_t other_retry_full_end =
        response_full_end(other_retry_tx_end, other, 0, other_payload, sizeof other_payload);
    const uint64_t a_tx_start = other_retry_full_end + omgp::TRUNK_T_gap_us;
    const uint64_t a_tx_end =
        a_tx_start +
        static_cast<uint64_t>(request_bytes(dst, 0, false, payload, sizeof payload).size()) * byte_us();

    // The stale answer starts at other_tx_end0 + delay_us (contracts/mock-wire.md
    // "Respond"); choosing delay_us = (a_tx_end - other_tx_end0) + margin lands it just
    // after A transmits.
    const uint32_t margin = 10;
    const uint32_t stale_delay_us = static_cast<uint32_t>(a_tx_end - other_tx_end0) + margin;
    const Step other_script[] = {{other, Kind::Respond, stale_delay_us}};
    wire.set_script(other, other_script, 1);
    // A's own answer is deliberately delayed past the stale frame's own full length (a
    // real, 9-byte-on-the-wire frame takes far longer than TRUNK_T_turn_min_us to
    // transmit) so the two never interleave byte-for-byte on the shared MockWire queue —
    // exactly as two genuine transmitters could never overlap on a real half-duplex bus.
    const uint32_t a_answer_delay_us = 110; // stale frame (10..100 past a_tx_end) fully clear
    const Step a_script[] = {{dst, Kind::Respond, a_answer_delay_us}};
    wire.set_script(dst, a_script, 1);

    Master master(wire, clock, omgp::ADDR_host);
    REQUIRE(master.begin(other, other_payload, sizeof other_payload) == Status::Ok);

    MasterEvent ev = wire.advance_to(other_retry_tx_start, master);
    REQUIRE(ev.kind == MasterEvent::None);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(wire.transcript(1).retry);
    REQUIRE(wire.transcript(1).seq == 0);

    ev = wire.advance_to(other_retry_full_end, master);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(master.stats(other).transactions == 1);
    REQUIRE(master.stats(other).retries == 1);
    REQUIRE_FALSE(master.busy());

    // Transaction to A (a different destination): issued the instant B's transaction
    // concludes, well before last_activity (== other_retry_full_end) + T_gap.
    REQUIRE(master.begin(dst, payload, sizeof payload) == Status::Ok);
    REQUIRE(master.busy());
    REQUIRE(wire.transcript_size() == 2); // not yet on the wire

    wire.advance_to(a_tx_start - 1, master);
    REQUIRE(wire.transcript_size() == 2); // not one microsecond early

    ev = wire.advance_to(a_tx_start, master);
    REQUIRE(wire.transcript_size() == 3);
    REQUIRE(wire.transcript(2).dst == dst);
    REQUIRE(wire.transcript(2).seq == 0);
    REQUIRE_FALSE(wire.transcript(2).retry);

    const uint64_t stale_start = other_tx_end0 + stale_delay_us;
    const size_t stale_n = response_bytes(other, 0, other_payload, sizeof other_payload).size();
    const uint64_t stale_end = stale_start + stale_n * byte_us();
    // Sanity: this test isn't vacuous - B's stale answer really does land inside A's open
    // window, not before A transmitted or after A's own window would already have closed.
    REQUIRE(stale_start > a_tx_end);
    REQUIRE(stale_end < a_tx_end + omgp::TRUNK_T_resp_us);

    const uint32_t discards_before = master.stats(dst).discards;
    ev = wire.advance_to(stale_end, master);
    REQUIRE(ev.kind == MasterEvent::None); // wrong src (other, not A's own): discarded silently
    REQUIRE(master.stats(dst).discards == discards_before + 1);
    REQUIRE(master.busy()); // A's window keeps running

    const uint64_t a_full_end =
        response_full_end(a_tx_end, dst, 0, payload, sizeof payload, a_answer_delay_us);
    ev = wire.advance_to(a_full_end, master);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(master.stats(dst).transactions == 1);
    REQUIRE(master.stats(dst).retries == 0); // unaffected by B's stale frame
}

// --- US2 AC6 (wrong-dst sub-case): a response mis-addressed to someone other than the
// host is discarded during a transaction's open window ---------------------------------

TEST_CASE("a response mis-addressed to someone other than the host (wrong dst), synthesised "
          "by injecting a forged request whose src MockWire mirrors into the answer's dst, "
          "is discarded during a transaction's open window without ending it or corrupting "
          "its counters",
          "[link]") {
    // MockWire::transmit() is the public ByteWire override every real transmitter uses (a
    // real Master/Responder, or - as here - a test standing in for a second, spoofing
    // station); schedule_respond() (mock_wire.cpp) always mirrors the polled request's src
    // into the answer's dst. Feeding transmit() a hand-encoded "request" whose src is not
    // ADDR_host therefore synthesises a well-formed response frame addressed to someone
    // other than the host - AC6's wrong-dst sub-case - with no second Responder and no
    // T034 dependency (PR #137 review, MEDIUM: the file header's prior "no MockWire-public
    // mechanism" claim for this sub-case did not hold).
    FakeClock clock;
    MockWire wire(clock);
    const uint8_t dst = 0x0E;             // Master's own, real transaction
    const uint8_t forged_dst_node = 0x03; // the forged "request"'s claimed destination
    const uint8_t forged_src = 0x0F;      // != ADDR_host: mirrored into the wrong response.dst
    const uint8_t payload[] = {0x0A};

    const uint64_t tx_end = static_cast<uint64_t>(
        request_bytes(dst, 0, false, payload, sizeof payload).size() * byte_us());

    // The forged "request" MockWire will treat as coming from forged_src, addressed to
    // forged_dst_node; a zero-length payload keeps its own airtime (and its answer's)
    // small. wrong_resp_bytes mirrors schedule_respond()'s own field construction exactly
    // (dst = request.src, src = request.dst, response = true, retry = false), only to
    // compute its true (possibly stuffed) wire length, never fed to transmit() itself.
    const std::vector<uint8_t> forged_bytes =
        encode_expected(forged_dst_node, forged_src, false, false, 0, nullptr, 0);
    const std::vector<uint8_t> wrong_resp_bytes =
        encode_expected(forged_src, forged_dst_node, true, false, 0, nullptr, 0);

    // forged_dst_node's own script (delay 0) puts the resulting wrong-dst answer's start
    // fully under this test's control.
    const Step forged_script[] = {{forged_dst_node, Kind::Respond, 0}};
    wire.set_script(forged_dst_node, forged_script, 1);

    const uint32_t margin = 10;
    const uint64_t forged_tx_now = tx_end + margin;
    const uint64_t forged_tx_end =
        forged_tx_now + static_cast<uint64_t>(forged_bytes.size()) * byte_us();
    const uint64_t wrong_resp_start = forged_tx_end; // the script's delay above is 0
    const uint64_t wrong_resp_end =
        wrong_resp_start + static_cast<uint64_t>(wrong_resp_bytes.size()) * byte_us();
    // Sanity: this test isn't vacuous - the wrong-dst frame really does land inside dst's
    // open window.
    REQUIRE(wrong_resp_start > tx_end);
    REQUIRE(wrong_resp_end < tx_end + omgp::TRUNK_T_resp_us);

    // dst's own (real) answer must start strictly after the wrong-dst frame's last byte, or
    // MockWire's single time-sorted RX queue (mock_wire.hpp: "sorted by start_us") hands
    // the engine both frames byte-interleaved and neither can be delivered intact - same
    // reasoning as the AC4/AC6 wrong-src fixes above (PR #137 review, HIGH). Still
    // comfortably inside dst's own T_resp window (only the response's START instant has to
    // fall inside it, per the T_resp-boundary edge case above).
    const uint32_t legit_delay_us = static_cast<uint32_t>(wrong_resp_end - tx_end) + margin;
    REQUIRE(legit_delay_us < omgp::TRUNK_T_resp_us);
    const Step dst_script[] = {{dst, Kind::Respond, legit_delay_us}};
    wire.set_script(dst, dst_script, 1);

    Master master(wire, clock, omgp::ADDR_host);
    REQUIRE(master.begin(dst, payload, sizeof payload) == Status::Ok);
    REQUIRE(wire.transcript_size() == 1);

    // Inject the forged frame directly (standing in for a second, spoofing station);
    // MockWire records it in the transcript like any other transmitted frame.
    wire.transmit(forged_bytes.data(), forged_bytes.size(), forged_tx_now);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(wire.transcript(1).dst == forged_dst_node);
    REQUIRE(wire.transcript(1).src == forged_src);

    const uint32_t discards_before = master.stats(dst).discards;
    MasterEvent ev = wire.advance_to(wrong_resp_end, master);
    REQUIRE(ev.kind == MasterEvent::None); // wrong dst (forged_src, not ADDR_host): discarded
    REQUIRE(master.stats(dst).discards == discards_before + 1);
    REQUIRE(master.busy()); // dst's window keeps running

    const uint64_t full_end =
        response_full_end(tx_end, dst, 0, payload, sizeof payload, legit_delay_us);
    ev = wire.advance_to(full_end, master);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(master.stats(dst).transactions == 1);
    REQUIRE(master.stats(dst).retries == 0); // unaffected by the wrong-dst frame
}

// --- US2 AC5 (gap half) / data-model.md §4 "Gap" --------------------------------------

TEST_CASE("a second begin() before last_activity + T_gap defers transmission to exactly "
          "that instant, never earlier",
          "[timing:T_gap]") {
    FakeClock clock;
    MockWire wire(clock);
    Master master(wire, clock, omgp::ADDR_host);
    const uint8_t dst = 0x0B;
    const uint8_t p1[] = {0x01};
    const uint8_t p2[] = {0x02};

    REQUIRE(master.begin(dst, p1, sizeof p1) == Status::Ok);
    const uint64_t tx_end0 =
        static_cast<uint64_t>(request_bytes(dst, 0, false, p1, sizeof p1).size()) * byte_us();
    const uint64_t full_end0 = response_full_end(tx_end0, dst, 0, p1, sizeof p1);

    MasterEvent ev = wire.advance_to(full_end0, master);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE_FALSE(master.busy());

    // Issued the instant the first transaction concludes: well before
    // last_activity (== full_end0) + T_gap.
    REQUIRE(master.begin(dst, p2, sizeof p2) == Status::Ok);
    REQUIRE(master.busy()); // accepted immediately, even though transmission is deferred
    REQUIRE(wire.transcript_size() == 1); // not yet on the wire

    wire.advance_to(full_end0 + omgp::TRUNK_T_gap_us - 1, master);
    REQUIRE(wire.transcript_size() == 1); // not one microsecond early

    ev = wire.advance_to(full_end0 + omgp::TRUNK_T_gap_us, master);
    REQUIRE(wire.transcript_size() == 2);
    REQUIRE(wire.transcript(1).tx_start_us == full_end0 + omgp::TRUNK_T_gap_us);
    REQUIRE(wire.transcript(1).seq == 1); // a new transaction, not a retry
    REQUIRE_FALSE(wire.transcript(1).retry);
}

TEST_CASE("a second begin() well after last_activity + T_gap transmits at the current "
          "instant, not at the stale gap boundary",
          "[timing:T_gap]") {
    // PR #137 review, HIGH: begin() computed deadline_ = last_activity_ + T_gap
    // unconditionally, even when that instant is already long past by the time begin() is
    // called (has_last_activity_ is set from an earlier, unrelated transaction). fire_pending
    // then transmitted at that stale, past `deadline_` instead of `now`, so the request left
    // the wire already outside its own response window and the transaction timed out before
    // any node could plausibly answer - this is the engine's ordinary/main operating mode
    // (a superframe scheduler calling begin() only once every T_poll, far longer than
    // T_gap), not a corner case, and no existing test (all of which poll() at-or-inside the
    // gap boundary) could catch it.
    FakeClock clock;
    MockWire wire(clock);
    Master master(wire, clock, omgp::ADDR_host);
    const uint8_t dst = 0x0B;
    const uint8_t p1[] = {0x01};
    const uint8_t p2[] = {0x02};

    REQUIRE(master.begin(dst, p1, sizeof p1) == Status::Ok);
    const uint64_t tx_end0 =
        static_cast<uint64_t>(request_bytes(dst, 0, false, p1, sizeof p1).size()) * byte_us();
    const uint64_t full_end0 = response_full_end(tx_end0, dst, 0, p1, sizeof p1);

    MasterEvent ev = wire.advance_to(full_end0, master);
    REQUIRE(ev.kind == MasterEvent::Answered);

    // Jump the clock far past last_activity (== full_end0) + T_gap with nothing driving the
    // engine in between - exactly what a superframe scheduler polling every T_poll (2000 us,
    // 40x T_gap) does between transactions.
    const uint64_t later = full_end0 + 10 * omgp::TRUNK_T_gap_us;
    wire.advance_to(later);

    REQUIRE(master.begin(dst, p2, sizeof p2) == Status::Ok);
    REQUIRE(wire.transcript_size() == 2);
    // Must transmit at `later` (now), never backdated to the long-elapsed
    // full_end0 + T_gap instant.
    REQUIRE(wire.transcript(1).tx_start_us == later);

    const uint64_t tx_end1 =
        later + static_cast<uint64_t>(request_bytes(dst, 1, false, p2, sizeof p2).size()) * byte_us();
    // A response window that actually opens after `later` (not one already exhausted at
    // transmission time): poll() right after transmitting must not immediately time out.
    ev = wire.advance_to(later + 1, master);
    REQUIRE(ev.kind == MasterEvent::None);
    REQUIRE(master.busy());

    const uint64_t full_end1 = response_full_end(tx_end1, dst, 1, p2, sizeof p2);
    ev = wire.advance_to(full_end1, master);
    REQUIRE(ev.kind == MasterEvent::Answered);
    REQUIRE(master.stats(dst).timeouts == 0); // never spuriously timed out
}

TEST_CASE("a gap-deferred retry skipped over at poll() time transmits at the actual poll "
          "instant, not the computed gap deadline",
          "[timing:T_gap]") {
    // Mirrors the begin()-side fix above (PR #137 review, HIGH) for fire_pending()'s OTHER
    // caller: a retry scheduled by end_attempt() (data-model.md §4 "Gap") is just as exposed
    // if the driving loop's next poll() lands well past the gap deadline instead of exactly on
    // it. Two separate poll() calls are needed to isolate this: the first lands exactly at the
    // (old) response deadline, which both times out attempt 0 AND computes the retry's gap
    // deadline in the same call, leaving nothing to distinguish (data-model.md §4). Only a
    // SECOND, later poll() call - with the retry already parked in PendingTransmit from the
    // first - exercises fire_pending() with now_us strictly past its own deadline_.
    FakeClock clock;
    MockWire wire(clock);
    const uint8_t dst = 0x0D;
    const Step silence[] = {{dst, Kind::Silence}};
    wire.set_script(dst, silence, 1);
    Master master(wire, clock, omgp::ADDR_host);

    const uint8_t payload[] = {0x09};
    REQUIRE(master.begin(dst, payload, sizeof payload) == Status::Ok);
    const uint64_t tx_end =
        static_cast<uint64_t>(request_bytes(dst, 0, false, payload, sizeof payload).size()) * byte_us();
    const uint64_t timeout_instant = tx_end + omgp::TRUNK_T_resp_us;

    // Lands exactly at the response deadline: times out attempt 0 and schedules the retry
    // (PendingTransmit, deadline_ = timeout_instant + T_gap) without yet transmitting it.
    MasterEvent ev = wire.advance_to(timeout_instant, master);
    REQUIRE(ev.kind == MasterEvent::None);
    REQUIRE(wire.transcript_size() == 1);

    const uint64_t retry_gap_deadline = timeout_instant + omgp::TRUNK_T_gap_us;
    const uint64_t later = retry_gap_deadline + 7 * byte_us();
    ev = wire.advance_to(later, master);
    REQUIRE(wire.transcript_size() == 2);
    // Must transmit at `later` (now), never backdated to the already-elapsed gap deadline.
    REQUIRE(wire.transcript(1).tx_start_us == later);
    REQUIRE(wire.transcript(1).retry);
}

// --- Edge case: sequence number wrap ---------------------------------------------------

TEST_CASE("new (non-retry) transactions to one destination use seq 0..15 then wrap to 0",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    Master master(wire, clock, omgp::ADDR_host);
    const uint8_t dst = 0x05;
    const uint8_t payload[] = {0x07};

    uint64_t now = 0;
    for (int i = 0; i < 17; ++i) {
        const uint8_t expected_seq = static_cast<uint8_t>(i % 16);
        wire.advance_to(now);
        REQUIRE(master.begin(dst, payload, sizeof payload) == Status::Ok);
        REQUIRE(wire.transcript_size() == static_cast<size_t>(i) + 1);
        const auto& req = wire.transcript(static_cast<size_t>(i));
        REQUIRE(req.seq == expected_seq);
        REQUIRE_FALSE(req.retry);

        const uint64_t tx_end =
            req.tx_start_us + static_cast<uint64_t>(
                                  request_bytes(dst, expected_seq, false, payload, sizeof payload)
                                      .size()) *
                                  byte_us();
        const uint64_t full_end = response_full_end(tx_end, dst, expected_seq, payload, sizeof payload);

        MasterEvent ev = wire.advance_to(full_end, master);
        REQUIRE(ev.kind == MasterEvent::Answered);
        now = full_end + omgp::TRUNK_T_gap_us;
    }
}

// --- contracts/link-cpp.md: begin() refusals ------------------------------------------

TEST_CASE("begin() while a transaction is open returns Busy and transmits nothing additional",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    const uint8_t dst = 0x04;
    const Step silence[] = {{dst, Kind::Silence}};
    wire.set_script(dst, silence, 1);
    Master master(wire, clock, omgp::ADDR_host);

    const uint8_t p1[] = {0x1};
    REQUIRE(master.begin(dst, p1, sizeof p1) == Status::Ok);
    REQUIRE(master.busy());
    REQUIRE(wire.transcript_size() == 1);

    const uint8_t p2[] = {0x2};
    REQUIRE(master.begin(dst, p2, sizeof p2) == Status::Busy);
    REQUIRE(wire.transcript_size() == 1); // nothing additional reached the wire
}

TEST_CASE("begin() refuses a 65-byte payload before anything reaches the wire", "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    Master master(wire, clock, omgp::ADDR_host);

    uint8_t payload[omgp::LIMIT_max_l3_payload + 1] = {};
    REQUIRE(master.begin(0x01, payload, sizeof payload) == Status::PayloadTooLong);
    REQUIRE(wire.transcript_size() == 0);
    REQUIRE_FALSE(master.busy());
}

TEST_CASE("begin() accepts a payload exactly at LIMIT_max_l3_payload bytes", "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    Master master(wire, clock, omgp::ADDR_host);

    uint8_t payload[omgp::LIMIT_max_l3_payload] = {};
    REQUIRE(master.begin(0x01, payload, sizeof payload) == Status::Ok);
    REQUIRE(master.busy());
}

// --- contracts/link-cpp.md: set_bit_rate() forwards to the wire and is counted ---------

TEST_CASE("set_bit_rate forwards to the wire and counts each change in bus_stats()", "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    Master master(wire, clock, omgp::ADDR_host);

    REQUIRE(master.bus_stats().rate_changes == 0);
    master.set_bit_rate(19200);
    REQUIRE(wire.bit_rate() == 19200);
    REQUIRE(master.bus_stats().rate_changes == 1);
    master.set_bit_rate(9600);
    REQUIRE(wire.bit_rate() == 9600);
    REQUIRE(master.bus_stats().rate_changes == 2);
}

// --- begin() is bounds-checked against its own kAddrCount-entry tables, not just 0xFF ---

TEST_CASE("begin() refuses a dst outside kAddrCount before writing next_seq_/stats_ past "
          "either table",
          "[link]") {
    // PR #137 review, HIGH: encode_frame/the Deframer refuse only dst == 0xFF
    // (link/frame.cpp), so 0x10..0xFE survive both intact and previously reached
    // next_seq_[dst] = ... / stats_[dst_] in Master, which are only kAddrCount (16) entries
    // (master.hpp). begin(0x20, ...) indexed 16 entries past the end of both arrays.
    FakeClock clock;
    MockWire wire(clock);
    Master master(wire, clock, omgp::ADDR_host);

    const uint8_t payload[] = {0x01};
    REQUIRE(master.begin(0x20, payload, sizeof payload) != Status::Ok);
    REQUIRE(wire.transcript_size() == 0);
    REQUIRE_FALSE(master.busy());
    // stats() for that address must still read back as untouched (also exercises the
    // stats() reader's own bounds check from the prior review pass, not just begin()'s).
    REQUIRE(master.stats(0x20).transactions == 0);

    // The table itself is unharmed: a valid destination still starts at seq 0, not
    // whatever an out-of-bounds write at 0x20 might have aliased onto it.
    REQUIRE(master.begin(0x01, payload, sizeof payload) == Status::Ok);
    REQUIRE(wire.transcript(0).seq == 0);
}

TEST_CASE("begin() refuses dst == kAddrCount exactly; kAddrCount - 1 is still accepted",
          "[link]") {
    // The test above (dst = 0x20) is already out of range under `>` alone and cannot tell
    // begin()'s `dst >= kAddrCount` guard apart from a weaker `dst > kAddrCount`. kAddrCount
    // itself (link_types.hpp: one past the last addressable node, ADDR_backplane_max) is the
    // first genuinely out-of-range index and the only value that pins the boundary.
    FakeClock clock;
    MockWire wire(clock);
    Master master(wire, clock, omgp::ADDR_host);

    const uint8_t payload[] = {0x01};
    REQUIRE(master.begin(static_cast<uint8_t>(kAddrCount), payload, sizeof payload) ==
            Status::ReservedAddress);
    REQUIRE(wire.transcript_size() == 0);
    REQUIRE_FALSE(master.busy());

    REQUIRE(master.begin(static_cast<uint8_t>(kAddrCount - 1), payload, sizeof payload) ==
            Status::Ok);
}

// --- contracts/link-cpp.md: stats() is bounds-checked, not a raw table index -----------

TEST_CASE("stats() for an address outside kAddrCount returns a benign, all-zero record "
          "rather than indexing past the table",
          "[link]") {
    // 0x10..0xFE survive both encode_frame and the Deframer (only dst == 0xFF is refused,
    // link/frame.cpp), so a wire-derived address reaching stats() is not a hypothetical
    // caller error (PR #137 review, MEDIUM). stats_ is AddrStats[kAddrCount] (16 entries,
    // master.hpp); addr >= kAddrCount must not read past it the way HealthTracker's
    // is_node_addr guard already prevents for the same address-keyed-table shape
    // (link/health.cpp).
    FakeClock clock;
    MockWire wire(clock);
    Master master(wire, clock, omgp::ADDR_host);

    const AddrStats& oob = master.stats(0xFF);
    REQUIRE(oob.transactions == 0);
    REQUIRE(oob.retries == 0);
    REQUIRE(oob.timeouts == 0);
    REQUIRE(oob.crc_failures == 0);
    REQUIRE(oob.discards == 0);
    REQUIRE(oob.replays_served == 0);
    REQUIRE(oob.late_responses == 0);
}

TEST_CASE("stats() treats kAddrCount as the first out-of-range index; kAddrCount-1 is the "
          "last valid one",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    Master master(wire, clock, omgp::ADDR_host);

    // The last valid table entry and the out-of-range sentinel must be genuinely different
    // objects: the boundary is at kAddrCount, one past the last valid index.
    REQUIRE(&master.stats(static_cast<uint8_t>(kAddrCount - 1)) != &master.stats(static_cast<uint8_t>(kAddrCount)));
    // kAddrCount itself already aliases the same all-zero sentinel a clearly out-of-range
    // address (0xFF) does.
    REQUIRE(&master.stats(static_cast<uint8_t>(kAddrCount)) == &master.stats(0xFF));
}

// --- CLAUDE.md rule 5: no dynamic allocation in embedded-path code --------------------

TEST_CASE("a full transaction (begin through the terminal event) allocates nothing", "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    Master master(wire, clock, omgp::ADDR_host);
    const uint8_t dst = 0x07;
    const uint8_t payload[] = {0xAA};

    // Pre-computed outside the HEAP_FREE_SCOPE below: encode_expected()'s own std::vector
    // return would otherwise count as an allocation attributed (wrongly) to Master/MockWire.
    const uint64_t tx_end =
        static_cast<uint64_t>(request_bytes(dst, 0, false, payload, sizeof payload).size()) * byte_us();
    const uint64_t full_end = response_full_end(tx_end, dst, 0, payload, sizeof payload);

    Status begin_st{};
    MasterEvent ev{};
    // advance_to(t) (clock-only) runs INFO/REQUIRE (Catch2 macros that allocate) and must
    // stay outside the guarded region below, or the guard measures Catch2's own
    // allocations instead of Master's (PR #137 review) — only begin()/poll() themselves,
    // bare engine calls with no Catch2 macro inside, are measured.
    HEAP_FREE_SCOPE({ begin_st = master.begin(dst, payload, sizeof payload); });
    wire.advance_to(full_end);
    HEAP_FREE_SCOPE({ ev = master.poll(full_end); });
    REQUIRE(begin_st == Status::Ok);
    REQUIRE(ev.kind == MasterEvent::Answered);
}

// --- Edge case: simulated time that does not advance ----------------------------------

TEST_CASE("the engine makes no progress without poll(); repolling at an unchanged now_us "
          "changes nothing",
          "[link]") {
    FakeClock clock;
    MockWire wire(clock);
    const uint8_t dst = 0x06;
    const Step silence[] = {{dst, Kind::Silence}};
    wire.set_script(dst, silence, 1);
    Master master(wire, clock, omgp::ADDR_host);

    const uint8_t payload[] = {0x9};
    REQUIRE(master.begin(dst, payload, sizeof payload) == Status::Ok);

    MasterEvent ev1 = master.poll(clock.now_us());
    REQUIRE(ev1.kind == MasterEvent::None);
    REQUIRE(master.busy());

    // Same now_us again: the clock never advanced, so nothing further can have happened.
    MasterEvent ev2 = master.poll(clock.now_us());
    REQUIRE(ev2.kind == MasterEvent::None);
    REQUIRE(master.busy());
}
