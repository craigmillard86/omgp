// Host-core test-support contract (spec 003 T013, #684): pins the script-consumption order,
// steady-state-on-exhaustion, event-queue and genuine-timeout behaviour of MockL3Node — this
// feature's own MESSAGE-level test double (contracts/mock-l3-node.md, research.md R-08) —
// before any core/ behaviour is built on it, exactly as F2's tests/unit/test_mock_wire.cpp
// pinned MockWire before Master/Responder were written. A later test_core_*.cpp failure is then
// attributable to CoreEngine rather than to the instrument measuring it.
//
// Written from contracts/mock-l3-node.md (Step table, Scheduling, "What this double is for"),
// docs/protocol-l3.md §3 (header), §3.1 (opcodes/responses), §3.2 (SELECT_CHANNEL accepted
// immediately, CHANNEL_SETTLED later), §3.3 (status block), §3.4 (events, NONE == 0x00) and
// docs/trunk-link-layer.md §3/§7/§9 (response window, retry rule, timing table) — not from
// tests/support/mock_l3_node.{hpp,cpp} (T012, #683), which this file was written before.
//
// Time is explicit throughout (CLAUDE.md rule 3): nothing sleeps, nothing reads a wall clock,
// and every deadline is a TRUNK_T_* symbol from the generated header (rule 4). The expectation
// for the double's wire bytes is built independently here with omgp::link::encode_frame and
// read back with omgp::link::Deframer (never by calling into mock_l3_node.cpp), so a
// pre-decoded struct or skipped stuffing/CRC16 on the double's side is red.
#include "catch_amalgamated.hpp"
#include "fake_clock.hpp"
#include "l3/l3_header.hpp"
#include "l3/l3_payload.hpp"
#include "l3/l3_types.hpp"
#include "link/frame.hpp"
#include "link/link_types.hpp"
#include "link/master.hpp"
#include "mock_l3_node.hpp"
#include "omgp_protocol.h"

#include <cstdint>
#include <vector>

using namespace omgp::link;
using omgp_test::ChannelStep;
using omgp_test::DescChunkStep;
using omgp_test::ErrorStep;
using omgp_test::EventStep;
using omgp_test::FakeClock;
using omgp_test::IdentifyStep;
using omgp_test::L3Step;
using omgp_test::MockL3Node;
using omgp_test::ParamStep;
using omgp_test::SilenceStep;
using omgp_test::SlotMapStep;
using omgp_test::StatusStep;

namespace {

// A backplane trunk address (docs/trunk-link-layer.md §5: 0x01..0x0F), and a second one for
// the cases that need two independently scripted nodes.
constexpr uint8_t kNode = 0x03;
constexpr uint8_t kOtherNode = 0x04;

uint64_t byte_us() {
    return byte_time_us(omgp::TRUNK_bit_rate);
}

// The largest READ_DESC chunk the protocol can carry: the response is `u16 offset, u8 len,
// u8[len] bytes` (protocol-l3 §3.1), so len <= LIMIT_max_l3_payload - 3. Computed from the
// symbol, never written as a literal — research.md R-09's own correction (2026-09-21) records
// that the number moved from 61 to 56 when LIMIT_max_l3_payload split from max_l3_message, and
// #684's acceptance criterion still quotes the stale 61. CLAUDE.md rule 4.
constexpr uint8_t kMaxDescChunk = static_cast<uint8_t>(omgp::LIMIT_max_l3_payload - 3);

// A complete L3 message: the five-byte header (protocol-l3 §3) then its payload, which is what
// one trunk frame carries as its payload and what Master::begin() is handed.
std::vector<uint8_t> l3_message(uint8_t opcode, uint8_t node_id, uint8_t seq, uint8_t flags,
                                const uint8_t* payload, size_t len) {
    const omgp::l3::Header h{opcode, node_id, seq, flags, static_cast<uint8_t>(len)};
    uint8_t out[omgp::LIMIT_max_l3_message];
    size_t written = 0;
    REQUIRE(omgp::l3::encode_header(h, out, sizeof out, written) == omgp::l3::Status::Ok);
    std::vector<uint8_t> msg(out, out + written);
    for (size_t i = 0; i < len; ++i)
        msg.push_back(payload[i]);
    return msg;
}

std::vector<uint8_t> request_msg(uint8_t opcode, uint8_t node_id, uint8_t seq,
                                 const uint8_t* payload = nullptr, size_t len = 0) {
    return l3_message(opcode, node_id, seq, 0, payload, len); // flags bit0 clear: a request
}

// The L3 message a conforming node's answer carries, built here from the codecs directly so the
// double's own output is compared against an independent construction.
std::vector<uint8_t> response_msg(uint8_t opcode, uint8_t node_id, uint8_t seq, uint8_t flags,
                                  const uint8_t* payload, size_t len) {
    const uint8_t f = static_cast<uint8_t>(omgp::FLAG_response | flags);
    return l3_message(opcode, node_id, seq, f, payload, len);
}

// One trunk frame (§4) carrying `msg`, encoded by the real codec.
std::vector<uint8_t> encode_one(uint8_t dst, uint8_t src, bool response, bool retry, uint8_t l2seq,
                                const std::vector<uint8_t>& msg) {
    FrameFields f{};
    f.dst = dst;
    f.src = src;
    f.response = response;
    f.retry = retry;
    f.seq = l2seq;
    f.len = static_cast<uint8_t>(msg.size());
    f.payload = msg.data();
    uint8_t out[kMaxWire];
    size_t written = 0;
    REQUIRE(encode_frame(f, out, sizeof out, written) == Status::Ok);
    return std::vector<uint8_t>(out, out + written);
}

// One wire frame carrying `msg` as a request to `dst`.
std::vector<uint8_t> request_frame(uint8_t dst, uint8_t l2seq, const std::vector<uint8_t>& msg) {
    return encode_one(dst, omgp::ADDR_host, false, false, l2seq, msg);
}

// Wire length of the request frame Master transmits for `msg`, with the retry bit set or clear
// (trunk §7: a retry reuses the same seq and sets the bit, so its stuffed length may differ).
uint64_t req_frame_us(uint8_t dst, uint8_t l2seq, bool retry, const std::vector<uint8_t>& msg) {
    const std::vector<uint8_t> f = encode_one(dst, omgp::ADDR_host, false, retry, l2seq, msg);
    return static_cast<uint64_t>(f.size()) * byte_us();
}

// The wire bytes a conforming node's answer to that request would be: mirrored addresses,
// response bit set, L2 seq echoed (trunk §4/§7).
std::vector<uint8_t> answer_frame(uint8_t node, uint8_t l2seq, const std::vector<uint8_t>& msg) {
    return encode_one(omgp::ADDR_host, node, true, false, l2seq, msg);
}

// A bound on when any single answer to a request ending at `tx_end` has finished arriving:
// the double's turnaround (trunk §9 T_turn) plus one worst-case frame at the current rate
// (trunk §4 kMaxWire). Derived from symbols, never a hand-picked instant.
uint64_t answer_deadline(uint64_t tx_end) {
    return tx_end + omgp::TRUNK_T_turn_min_us + static_cast<uint64_t>(kMaxWire) * byte_us();
}

// One request/answer exchange driven DIRECTLY against the double (no Master in the loop), for
// the cases that assert byte content or an exact instant: transmit() at `at_us`, advance to
// `until_us`, drain every byte due by then and decode what came back.
struct Answer {
    // Whether a complete frame was delivered by the real Deframer.
    bool got = false;
    // Every byte the double put on the wire, and the start instant of the first.
    std::vector<uint8_t> wire;
    uint64_t first_byte_us = 0;
    // Decoded L2 fields (trunk §4).
    uint8_t l2_dst = 0;
    uint8_t l2_src = 0;
    uint8_t l2_seq = 0;
    bool l2_response = false;
    // Decoded L3 header and payload (protocol-l3 §3).
    omgp::l3::Header hdr{};
    std::vector<uint8_t> payload;
};

Answer exchange(MockL3Node& node, const std::vector<uint8_t>& frame, uint64_t at_us,
                uint64_t until_us) {
    Answer a;
    const uint64_t tx_end = node.transmit(frame.data(), frame.size(), at_us);
    REQUIRE(tx_end == at_us + static_cast<uint64_t>(frame.size()) * byte_us());
    node.advance_to(until_us);
    uint8_t b = 0;
    uint64_t start = 0;
    while (node.receive(b, start)) {
        if (a.wire.empty())
            a.first_byte_us = start;
        REQUIRE(start <= until_us);
        a.wire.push_back(b);
    }
    Deframer d;
    FrameView v{};
    for (uint8_t x : a.wire) {
        if (!d.feed(x, v))
            continue;
        a.got = true;
        a.l2_dst = v.f.dst;
        a.l2_src = v.f.src;
        a.l2_seq = v.f.seq;
        a.l2_response = v.f.response;
        omgp::l3::Bytes p{};
        REQUIRE(omgp::l3::decode_message(v.f.payload, v.f.len, a.hdr, p) == omgp::l3::Status::Ok);
        a.payload.assign(p.data, p.data + p.len);
    }
    return a;
}

// A decoded GET_EVENT response that OWNS its detail bytes: omgp::l3::GetEventResp::detail is a
// view into the payload it was decoded from, so returning one from a helper would dangle.
struct Event {
    uint8_t event_type = 0;
    uint8_t remaining_count = 0;
    std::vector<uint8_t> detail;
};

Event decode_event(const Answer& a) {
    omgp::l3::GetEventResp r{};
    REQUIRE(omgp::l3::decode_get_event_resp(a.payload.data(), a.payload.size(), r) ==
            omgp::l3::Status::Ok);
    Event e;
    e.event_type = r.event_type;
    e.remaining_count = r.remaining_count;
    e.detail.assign(r.detail.data, r.detail.data + r.detail.len);
    return e;
}

// The same exchange, with the frame built from an L3 message and the deadline derived.
Answer ask(MockL3Node& node, uint8_t dst, uint8_t l2seq, const std::vector<uint8_t>& msg,
           uint64_t at_us) {
    const std::vector<uint8_t> frame = request_frame(dst, l2seq, msg);
    const uint64_t tx_end = at_us + static_cast<uint64_t>(frame.size()) * byte_us();
    return exchange(node, frame, at_us, answer_deadline(tx_end));
}

// How far a single transaction may run: three attempts (trunk §7: one plus TRUNK_retries),
// each a response window, a gap and at most one worst-case frame each way.
uint64_t transaction_horizon() {
    const uint64_t per_attempt = omgp::TRUNK_T_resp_us + omgp::TRUNK_T_gap_us +
                                 2 * static_cast<uint64_t>(kMaxWire) * byte_us();
    return (omgp::TRUNK_retries + 1) * per_attempt;
}

// One REAL Master transaction driven to its terminal event over the double, polled at byte
// cadence. The response payload is copied out immediately: MasterEvent::response points into
// Master's own buffer and is valid only until the next poll() (link/master.hpp).
struct TxOutcome {
    MasterEvent::Kind kind = MasterEvent::None;
    MasterEvent::Reason reason = MasterEvent::Timeout;
    omgp::l3::Header hdr{};
    std::vector<uint8_t> payload;
};

TxOutcome transact(MockL3Node& node, Master& master, FakeClock& clock, uint8_t dst,
                   const std::vector<uint8_t>& msg) {
    REQUIRE(master.begin(dst, msg.data(), msg.size()) == Status::Ok);
    const uint64_t horizon = clock.t + transaction_horizon();
    TxOutcome out;
    for (uint64_t t = clock.t; t <= horizon; t += byte_us()) {
        const MasterEvent ev = node.advance_to(t, master);
        if (ev.kind == MasterEvent::None)
            continue;
        out.kind = ev.kind;
        out.reason = ev.reason;
        if (ev.kind == MasterEvent::Answered) {
            omgp::l3::Bytes p{};
            REQUIRE(omgp::l3::decode_message(ev.response.payload, ev.response.len, out.hdr, p) ==
                    omgp::l3::Status::Ok);
            out.payload.assign(p.data, p.data + p.len);
        }
        return out;
    }
    FAIL("no terminal MasterEvent within the transaction horizon");
    return out;
}

// A scripted status block and identify response, valid by the codecs' own range rules
// (protocol-l3 §3.1/§3.3): state from NODE_STATE_CODES, module_type from MODULE_TYPE_CODES.
omgp::l3::StatusBlock status_block(uint8_t active_channel, uint16_t uptime_s,
                                   uint8_t event_pending = 0) {
    omgp::l3::StatusBlock b{};
    b.state = omgp::STATE_READY;
    b.active_channel = active_channel;
    b.uptime_s = uptime_s;
    b.event_pending = event_pending;
    return b;
}

omgp::l3::IdentifyResp identify_resp(uint16_t desc_len, uint16_t desc_crc) {
    return omgp::l3::IdentifyResp{omgp::PROTOCOL_MAJOR, omgp::PROTOCOL_MINOR,
                                  omgp::MT_ANALOGUE_EFFECT, desc_len, desc_crc};
}

} // namespace

// --- Real wire bytes (contracts/mock-l3-node.md "Scheduling") --------------------------------

TEST_CASE("a scripted answer is real wire bytes: byte-for-byte what encode_frame produces, "
          "read back through the real Deframer",
          "[core][mock_l3_node][timing:T_turn]") {
    FakeClock clock;
    MockL3Node node(clock);
    const omgp::l3::StatusBlock blk = status_block(2, 1234);
    const L3Step script[] = {L3Step::of(StatusStep{blk})};
    node.set_script(kNode, script, 1);

    const uint8_t l3_seq = 0x05;
    const uint8_t l2_seq = 0x03;
    const std::vector<uint8_t> msg = request_msg(omgp::OP_GET_STATUS, kNode, l3_seq);
    const std::vector<uint8_t> frame = request_frame(kNode, l2_seq, msg);
    const uint64_t at = 1000;
    const uint64_t tx_end = at + static_cast<uint64_t>(frame.size()) * byte_us();

    const Answer a = exchange(node, frame, at, answer_deadline(tx_end));
    REQUIRE(a.got);

    // The expectation, built here from the codecs alone: the §3.3 status block payload inside a
    // response-flagged L3 header inside a mirrored trunk frame.
    uint8_t status_payload[omgp::LIMIT_max_l3_payload];
    size_t status_len = 0;
    const omgp::l3::Status enc =
        omgp::l3::encode_status_block(blk, status_payload, sizeof status_payload, status_len);
    REQUIRE(enc == omgp::l3::Status::Ok);
    const std::vector<uint8_t> expect_msg =
        response_msg(omgp::OP_GET_STATUS, kNode, l3_seq, 0, status_payload, status_len);
    const std::vector<uint8_t> expect_wire = answer_frame(kNode, l2_seq, expect_msg);
    REQUIRE(a.wire == expect_wire);

    // ...and the same bytes, read as a frame: mirrored addresses, response bit, echoed seq.
    REQUIRE(a.l2_dst == omgp::ADDR_host);
    REQUIRE(a.l2_src == kNode);
    REQUIRE(a.l2_response);
    REQUIRE(a.l2_seq == l2_seq);
    REQUIRE(a.hdr.opcode == omgp::OP_GET_STATUS);
    REQUIRE(a.hdr.node_id == kNode);
    REQUIRE(a.hdr.seq == l3_seq);
    REQUIRE((a.hdr.flags & omgp::FLAG_response) != 0);
    REQUIRE((a.hdr.flags & omgp::FLAG_error) == 0);

    omgp::l3::StatusBlock got{};
    REQUIRE(omgp::l3::decode_status_block(a.payload.data(), a.payload.size(), got) ==
            omgp::l3::Status::Ok);
    REQUIRE(got.state == blk.state);
    REQUIRE(got.active_channel == blk.active_channel);
    REQUIRE(got.uptime_s == blk.uptime_s);

    // trunk §9: the answer starts one T_turn after the request's last stop bit (rule 4 — the
    // symbol, not an instant chosen by hand).
    REQUIRE(a.first_byte_us == tx_end + omgp::TRUNK_T_turn_min_us);
}

// --- Per-opcode-class cursors (contracts/mock-l3-node.md: "consumed in order per opcode class")

TEST_CASE("steps are consumed in order PER OPCODE CLASS: one cursor per class, not one shared "
          "cursor over the script",
          "[core][mock_l3_node]") {
    const omgp::l3::IdentifyResp id_a = identify_resp(100, 0xAAAA);
    const omgp::l3::IdentifyResp id_b = identify_resp(200, 0xBBBB);
    const omgp::l3::StatusBlock s1 = status_block(1, 11);
    const omgp::l3::StatusBlock s2 = status_block(2, 22);
    const L3Step script[] = {L3Step::of(IdentifyStep{id_a}), L3Step::of(IdentifyStep{id_b}),
                             L3Step::of(StatusStep{s1}), L3Step::of(StatusStep{s2})};

    auto identify_at = [](MockL3Node& node, uint8_t seq, uint64_t at) {
        const Answer a = ask(node, kNode, seq, request_msg(omgp::OP_IDENTIFY, kNode, seq), at);
        REQUIRE(a.got);
        REQUIRE(a.hdr.opcode == omgp::OP_IDENTIFY);
        omgp::l3::IdentifyResp r{};
        REQUIRE(omgp::l3::decode_identify_resp(a.payload.data(), a.payload.size(), r) ==
                omgp::l3::Status::Ok);
        return r;
    };
    auto status_at = [](MockL3Node& node, uint8_t seq, uint64_t at) {
        const Answer a = ask(node, kNode, seq, request_msg(omgp::OP_GET_STATUS, kNode, seq), at);
        REQUIRE(a.got);
        REQUIRE(a.hdr.opcode == omgp::OP_GET_STATUS);
        omgp::l3::StatusBlock b{};
        REQUIRE(omgp::l3::decode_status_block(a.payload.data(), a.payload.size(), b) ==
                omgp::l3::Status::Ok);
        return b;
    };

    SECTION("interrogated IDENTIFY-first: A, S1, B, S2") {
        FakeClock clock;
        MockL3Node node(clock);
        node.set_script(kNode, script, 4);
        REQUIRE(identify_at(node, 0, 1000).desc_len == id_a.desc_len);
        REQUIRE(status_at(node, 1, 10000).uptime_s == s1.uptime_s);
        REQUIRE(identify_at(node, 2, 20000).desc_len == id_b.desc_len);
        REQUIRE(status_at(node, 3, 30000).uptime_s == s2.uptime_s);
    }

    // The falsifying case for one shared cursor: it would hand the FIRST script entry (an
    // IdentifyStep) to a GET_STATUS, or refuse it.
    SECTION("interrogated GET_STATUS-first: S1, then A — a GET_STATUS never takes an "
            "IdentifyStep") {
        FakeClock clock;
        MockL3Node node(clock);
        node.set_script(kNode, script, 4);
        REQUIRE(status_at(node, 0, 1000).uptime_s == s1.uptime_s);
        REQUIRE(identify_at(node, 1, 10000).desc_len == id_a.desc_len);
        REQUIRE(status_at(node, 2, 20000).uptime_s == s2.uptime_s);
        REQUIRE(identify_at(node, 3, 30000).desc_len == id_b.desc_len);
    }
}

// --- Exhausted script: steady state, not silence ---------------------------------------------

TEST_CASE("an exhausted opcode class answers its last step forever: three further GET_STATUS "
          "requests are Answered with the same block, never a T_resp timeout",
          "[core][mock_l3_node][timing:T_resp]") {
    FakeClock clock;
    MockL3Node node(clock);
    const omgp::l3::StatusBlock blk = status_block(3, 777, 0);
    const L3Step script[] = {L3Step::of(StatusStep{blk})};
    node.set_script(kNode, script, 1);
    Master master(node, clock, omgp::ADDR_host);

    // Four transactions through the REAL Master: the first consumes the only StatusStep, the
    // next three are answered by the steady state. A silence-by-default double fails here with
    // Failed{Timeout} from the second onwards.
    for (uint8_t i = 0; i < 4; ++i) {
        const TxOutcome out =
            transact(node, master, clock, kNode, request_msg(omgp::OP_GET_STATUS, kNode, i));
        REQUIRE(out.kind == MasterEvent::Answered);
        REQUIRE(out.hdr.opcode == omgp::OP_GET_STATUS);
        REQUIRE(out.hdr.seq == i);
        omgp::l3::StatusBlock got{};
        REQUIRE(omgp::l3::decode_status_block(out.payload.data(), out.payload.size(), got) ==
                omgp::l3::Status::Ok);
        REQUIRE(got.state == blk.state);
        REQUIRE(got.active_channel == blk.active_channel);
        REQUIRE(got.bypass == blk.bypass);
        REQUIRE(got.fault_code == blk.fault_code);
        REQUIRE(got.uptime_s == blk.uptime_s);
        REQUIRE(got.event_pending == blk.event_pending);
    }
    REQUIRE(master.stats(kNode).transactions == 4);
    REQUIRE(master.stats(kNode).timeouts == 0);
}

// --- EventStep: a DRAINED QUEUE is the steady state (the contract's stated exception) --------

TEST_CASE("EventStep's steady state is no event: two EventSteps answer two GET_EVENTs, then "
          "every later GET_EVENT answers NONE rather than repeating the last event",
          "[core][mock_l3_node]") {
    FakeClock clock;
    MockL3Node node(clock);
    const uint8_t detail_a[] = {0x07};
    const omgp::l3::GetEventResp e1{omgp::EVT_FAULT_RAISED, 1, {detail_a, 1}};
    const omgp::l3::GetEventResp e2{omgp::EVT_FAULT_CLEARED, 0, {nullptr, 0}};
    const L3Step script[] = {L3Step::of(EventStep{e1}), L3Step::of(EventStep{e2})};
    node.set_script(kNode, script, 2);

    auto drain = [&node](uint8_t seq, uint64_t at) {
        const std::vector<uint8_t> msg = request_msg(omgp::OP_GET_EVENT, kNode, seq);
        const Answer a = ask(node, kNode, seq, msg, at);
        REQUIRE(a.got);
        REQUIRE(a.hdr.opcode == omgp::OP_GET_EVENT);
        return decode_event(a);
    };

    const Event first = drain(0, 1000);
    REQUIRE(first.event_type == omgp::EVT_FAULT_RAISED);
    REQUIRE(first.remaining_count == 1); // the script's own value, not one the double invents
    REQUIRE(first.detail == std::vector<uint8_t>(detail_a, detail_a + 1));

    const Event second = drain(1, 10000);
    REQUIRE(second.event_type == omgp::EVT_FAULT_CLEARED);
    REQUIRE(second.detail.empty());

    // protocol-l3 §3.4: NONE (0x00) is what GET_EVENT answers on an empty queue. A
    // last-step-repeats rule here would re-deliver event 2 forever.
    for (uint8_t i = 0; i < 2; ++i) {
        const Event none = drain(static_cast<uint8_t>(2 + i), 20000 + i * 10000);
        REQUIRE(none.event_type == omgp::EVT_NONE);
        REQUIRE(none.remaining_count == 0);
        REQUIRE(none.detail.empty());
    }
}

// --- SilenceStep: a genuine Master timeout over a real wire (trunk §3/§7) --------------------

TEST_CASE("a SilenceStep puts NO byte on the wire, so the real Master times out only after its "
          "full retry sequence and reports Failed{Timeout}",
          "[core][mock_l3_node][timing:T_resp][timing:retries]") {
    FakeClock clock;
    MockL3Node node(clock);
    const L3Step script[] = {L3Step::of(SilenceStep{})};
    node.set_script(kNode, script, 1);
    Master master(node, clock, omgp::ADDR_host);

    const std::vector<uint8_t> msg = request_msg(omgp::OP_GET_STATUS, kNode, 0);
    REQUIRE(master.begin(kNode, msg.data(), msg.size()) == Status::Ok);
    REQUIRE(master.attempts() == 1);

    // The first transmission is never gap-deferred (link/master.hpp), so it starts at 0.
    uint64_t tx_end = req_frame_us(kNode, 0, false, msg);
    const uint64_t retry_us = req_frame_us(kNode, 0, true, msg);

    for (uint32_t attempt = 0; attempt < omgp::TRUNK_retries; ++attempt) {
        // Not one microsecond before the window closes (trunk §3: the window is
        // [tx_end, tx_end + T_resp)).
        MasterEvent early = node.advance_to(tx_end + omgp::TRUNK_T_resp_us - 1, master);
        REQUIRE(early.kind == MasterEvent::None);
        const uint64_t retry_tx_start =
            tx_end + omgp::TRUNK_T_resp_us + omgp::TRUNK_T_gap_us; // trunk §7 + §9
        REQUIRE(node.advance_to(retry_tx_start, master).kind == MasterEvent::None);
        REQUIRE(master.attempts() == static_cast<uint8_t>(attempt + 2));
        tx_end = retry_tx_start + retry_us;
    }

    // Terminal only after the LAST attempt's own window: T_resp past that attempt's transmit
    // end, and not a microsecond earlier.
    MasterEvent early = node.advance_to(tx_end + omgp::TRUNK_T_resp_us - 1, master);
    REQUIRE(early.kind == MasterEvent::None);
    const MasterEvent ev = node.advance_to(tx_end + omgp::TRUNK_T_resp_us, master);
    REQUIRE(ev.kind == MasterEvent::Failed);
    REQUIRE(ev.reason == MasterEvent::Timeout);
    REQUIRE(master.attempts() == omgp::TRUNK_retries + 1);
    REQUIRE(master.stats(kNode).timeouts == omgp::TRUNK_retries + 1);

    // The timeout came from the wire being empty, not from a late or malformed answer: the
    // double scheduled nothing at all, for any of the three attempts it saw.
    REQUIRE(node.bytes_scheduled() == 0);
    REQUIRE(node.requests_seen() == omgp::TRUNK_retries + 1);
}

// --- DescChunkStep: the request's own offset/max_len (contract line 15, research.md R-09) ----

TEST_CASE("DescChunkStep answers every READ_DESC from its blob at the request's own offset and "
          "max_len, and the chunks concatenate to the blob byte-for-byte",
          "[core][mock_l3_node]") {
    FakeClock clock;
    MockL3Node node(clock);
    // Deliberately not a multiple of kMaxDescChunk, so the last non-empty chunk is short and
    // the zero-length terminator's own offset is inside LIMIT_max_descriptor_bytes.
    constexpr uint16_t kBlobLen = 200;
    uint8_t blob[kBlobLen];
    for (uint16_t i = 0; i < kBlobLen; ++i)
        blob[i] = static_cast<uint8_t>(i * 7 + 1); // deterministic, spans the stuffed bytes
    const L3Step script[] = {L3Step::of(DescChunkStep{blob, kBlobLen})};
    node.set_script(kNode, script, 1);

    auto read_desc = [&node](uint16_t offset, uint8_t max_len, uint8_t seq, uint64_t at) {
        uint8_t req[omgp::LIMIT_max_l3_payload];
        size_t req_len = 0;
        REQUIRE(omgp::l3::encode_read_desc_req(omgp::l3::ReadDescReq{offset, max_len}, req,
                                               sizeof req, req_len) == omgp::l3::Status::Ok);
        const Answer a =
            ask(node, kNode, seq, request_msg(omgp::OP_READ_DESC, kNode, seq, req, req_len), at);
        REQUIRE(a.got);
        REQUIRE(a.hdr.opcode == omgp::OP_READ_DESC);
        omgp::l3::ReadDescResp r{};
        REQUIRE(omgp::l3::decode_read_desc_resp(a.payload.data(), a.payload.size(), r) ==
                omgp::l3::Status::Ok);
        REQUIRE(r.offset == offset);
        return std::vector<uint8_t>(r.bytes.data, r.bytes.data + r.bytes.len);
    };

    SECTION("a full chunked read at the protocol's largest chunk reassembles the blob, ending "
            "in a zero-length chunk") {
        std::vector<uint8_t> got;
        uint16_t offset = 0;
        size_t chunks = 0;
        uint64_t at = 1000;
        while (true) {
            const std::vector<uint8_t> chunk =
                read_desc(offset, kMaxDescChunk, static_cast<uint8_t>(chunks & 0x0F), at);
            ++chunks;
            at += 10000;
            const size_t expect = kBlobLen - offset < kMaxDescChunk
                                      ? static_cast<size_t>(kBlobLen - offset)
                                      : static_cast<size_t>(kMaxDescChunk);
            REQUIRE(chunk.size() == expect); // min(max_len, blob_len - offset)
            if (chunk.empty())
                break;
            got.insert(got.end(), chunk.begin(), chunk.end());
            offset = static_cast<uint16_t>(offset + chunk.size());
            REQUIRE(chunks < 64); // a non-advancing read would otherwise spin
        }
        REQUIRE(got.size() == kBlobLen); // no over-read, no short read
        REQUIRE(std::vector<uint8_t>(blob, blob + kBlobLen) == got);
        // ceil(kBlobLen / kMaxDescChunk) full-or-short chunks, plus the empty terminator.
        REQUIRE(chunks == (kBlobLen + kMaxDescChunk - 1) / kMaxDescChunk + 1);
    }

    SECTION("a small max_len is honoured, not widened to the double's own idea of a chunk") {
        const std::vector<uint8_t> chunk = read_desc(10, 4, 0, 1000);
        REQUIRE(chunk.size() == 4);
        REQUIRE(chunk == std::vector<uint8_t>(blob + 10, blob + 14));
    }

    SECTION("an offset at the end of the blob answers a zero-length chunk, never an over-read") {
        REQUIRE(read_desc(kBlobLen, kMaxDescChunk, 0, 1000).empty());
    }
}

// --- ChannelStep: accepted immediately, CHANNEL_SETTLED later (protocol-l3 §3.2/§3.4) -------

TEST_CASE("ChannelStep accepts SELECT_CHANNEL immediately and, when it settles, makes "
          "CHANNEL_SETTLED drainable only once settle_delay_us has elapsed",
          "[core][mock_l3_node]") {
    FakeClock clock;
    MockL3Node node(clock);
    // A module's declared settle time (descriptor SWITCHING record, protocol-l3 §4.1): script
    // data, not a protocol constant.
    constexpr uint64_t kSettleUs = 1500;
    const L3Step script[] = {L3Step::of(ChannelStep{true, kSettleUs})};
    node.set_script(kNode, script, 1);

    const uint8_t channel = 2;
    const uint8_t sel_seq = 0x09;
    uint8_t sel_payload[omgp::LIMIT_max_l3_payload];
    size_t sel_len = 0;
    REQUIRE(omgp::l3::encode_select_channel(omgp::l3::SelectChannelReq{channel}, sel_payload,
                                            sizeof sel_payload, sel_len) == omgp::l3::Status::Ok);
    const std::vector<uint8_t> sel_msg =
        request_msg(omgp::OP_SELECT_CHANNEL, kNode, sel_seq, sel_payload, sel_len);
    const std::vector<uint8_t> sel_frame = request_frame(kNode, 0, sel_msg);
    const uint64_t at = 1000;
    const uint64_t sel_tx_end = at + static_cast<uint64_t>(sel_frame.size()) * byte_us();

    // §3.2: the answer is an immediate acceptance with an empty payload, not the completion.
    const Answer accepted = exchange(node, sel_frame, at, answer_deadline(sel_tx_end));
    REQUIRE(accepted.got);
    REQUIRE(accepted.hdr.opcode == omgp::OP_SELECT_CHANNEL);
    REQUIRE((accepted.hdr.flags & omgp::FLAG_error) == 0);
    REQUIRE(accepted.payload.empty());

    const uint64_t due = sel_tx_end + kSettleUs;

    auto get_event = [&node](uint8_t seq, uint64_t request_end) {
        // Sent so that the GET_EVENT request FINISHES on the wire exactly at `request_end`.
        const std::vector<uint8_t> msg = request_msg(omgp::OP_GET_EVENT, kNode, seq);
        const std::vector<uint8_t> frame = request_frame(kNode, seq, msg);
        const uint64_t start = request_end - static_cast<uint64_t>(frame.size()) * byte_us();
        const Answer a = exchange(node, frame, start, answer_deadline(request_end));
        REQUIRE(a.got);
        return decode_event(a);
    };

    // One microsecond before the settle delay has elapsed: nothing to report.
    const Event early = get_event(1, due - 1);
    REQUIRE(early.event_type == omgp::EVT_NONE);

    // At the settle instant: CHANNEL_SETTLED, whose detail is `u8 channel, u8 seq` (§3.4) —
    // the channel that settled and the SELECT_CHANNEL request's own L3 seq.
    const Event settled = get_event(2, due);
    REQUIRE(settled.event_type == omgp::EVT_CHANNEL_SETTLED);
    REQUIRE(settled.detail == std::vector<uint8_t>{channel, sel_seq});

    // Drained once, so it is gone: the queue, not the last step, is the steady state.
    REQUIRE(get_event(3, due + 10000).event_type == omgp::EVT_NONE);
}

TEST_CASE("ChannelStep{settles = false} never reports CHANNEL_SETTLED, however far time "
          "advances",
          "[core][mock_l3_node]") {
    FakeClock clock;
    MockL3Node node(clock);
    constexpr uint64_t kSettleUs = 1500;
    const L3Step script[] = {L3Step::of(ChannelStep{false, kSettleUs})};
    node.set_script(kNode, script, 1);

    uint8_t sel_payload[omgp::LIMIT_max_l3_payload];
    size_t sel_len = 0;
    REQUIRE(omgp::l3::encode_select_channel(omgp::l3::SelectChannelReq{1}, sel_payload,
                                            sizeof sel_payload, sel_len) == omgp::l3::Status::Ok);
    const std::vector<uint8_t> sel_msg =
        request_msg(omgp::OP_SELECT_CHANNEL, kNode, 0, sel_payload, sel_len);
    const Answer accepted = ask(node, kNode, 0, sel_msg, 1000);
    REQUIRE(accepted.got);
    REQUIRE(accepted.payload.empty()); // still accepted immediately (§3.2)

    // Ten times the settle delay, sampled ten times: never an event.
    for (uint8_t i = 1; i <= 10; ++i) {
        const std::vector<uint8_t> get = request_msg(omgp::OP_GET_EVENT, kNode, i);
        const Answer a = ask(node, kNode, i, get, 10000 + i * kSettleUs);
        REQUIRE(a.got);
        REQUIRE(decode_event(a).event_type == omgp::EVT_NONE);
    }
}

// --- ParamStep, SlotMapStep, ErrorStep ------------------------------------------------------

TEST_CASE("ParamStep answers GET_PARAM with its value and SET_PARAM with an acceptance when ok, "
          "and an ERROR carrying its code when not",
          "[core][mock_l3_node]") {
    FakeClock clock;
    MockL3Node node(clock);
    constexpr uint16_t kValue = 2000;
    const L3Step ok_script[] = {L3Step::of(ParamStep{true, kValue, 0})};
    const L3Step bad_script[] = {L3Step::of(ParamStep{false, 0, omgp::ERR_NOT_PERMITTED})};
    node.set_script(kNode, ok_script, 1);
    node.set_script(kOtherNode, bad_script, 1);

    const uint8_t param_id = 0x11;
    const uint8_t scope = 0xFF; // module scope (protocol-l3 §3.1)
    uint8_t get_payload[omgp::LIMIT_max_l3_payload];
    size_t get_len = 0;
    REQUIRE(omgp::l3::encode_get_param_req(omgp::l3::GetParamReq{param_id, scope}, get_payload,
                                           sizeof get_payload, get_len) == omgp::l3::Status::Ok);

    SECTION("ok: GET_PARAM carries the scripted value back") {
        const Answer a = ask(node, kNode, 0,
                             request_msg(omgp::OP_GET_PARAM, kNode, 0, get_payload, get_len), 1000);
        REQUIRE(a.got);
        REQUIRE(a.hdr.opcode == omgp::OP_GET_PARAM);
        omgp::l3::GetParamResp r{};
        REQUIRE(omgp::l3::decode_get_param_resp(a.payload.data(), a.payload.size(), r) ==
                omgp::l3::Status::Ok);
        REQUIRE(r.param_id == param_id);
        REQUIRE(r.scope == scope);
        REQUIRE(r.value == kValue);
    }

    SECTION("ok: SET_PARAM is an empty-payload acceptance (protocol-l3 §3.1)") {
        uint8_t set_payload[omgp::LIMIT_max_l3_payload];
        size_t set_len = 0;
        const omgp::l3::SetParamReq req{param_id, scope, 1234};
        const omgp::l3::Status enc =
            omgp::l3::encode_set_param(req, set_payload, sizeof set_payload, set_len);
        REQUIRE(enc == omgp::l3::Status::Ok);
        const std::vector<uint8_t> msg =
            request_msg(omgp::OP_SET_PARAM, kNode, 1, set_payload, set_len);
        const Answer a = ask(node, kNode, 1, msg, 1000);
        REQUIRE(a.got);
        REQUIRE(a.hdr.opcode == omgp::OP_SET_PARAM);
        REQUIRE((a.hdr.flags & omgp::FLAG_error) == 0);
        REQUIRE(a.payload.empty());
    }

    SECTION("not ok: an ERROR response carrying the step's own code") {
        const Answer a =
            ask(node, kOtherNode, 0,
                request_msg(omgp::OP_GET_PARAM, kOtherNode, 0, get_payload, get_len), 1000);
        REQUIRE(a.got);
        REQUIRE(a.hdr.opcode == omgp::OP_ERROR);
        REQUIRE((a.hdr.flags & omgp::FLAG_error) != 0);
        omgp::l3::ErrorResp r{};
        REQUIRE(omgp::l3::decode_error_resp(a.payload.data(), a.payload.size(), r) ==
                omgp::l3::Status::Ok);
        REQUIRE(r.code == omgp::ERR_NOT_PERMITTED);
    }
}

TEST_CASE("SlotMapStep round-trips slot_count/occupied_bits/changed_bits through the real "
          "BP_SLOT_MAP codec pair",
          "[core][mock_l3_node]") {
    FakeClock clock;
    MockL3Node node(clock);
    constexpr uint8_t kSlots = 12;
    constexpr uint32_t kOccupied = 0x0A5Au; // slots 1,3,4,6,8,11
    constexpr uint32_t kChanged = 0x0003u;  // slots 0,1
    const L3Step script[] = {L3Step::of(SlotMapStep{kSlots, kOccupied, kChanged})};
    node.set_script(kNode, script, 1);

    const Answer a = ask(node, kNode, 0, request_msg(omgp::OP_BP_SLOT_MAP, kNode, 0), 1000);
    REQUIRE(a.got);
    REQUIRE(a.hdr.opcode == omgp::OP_BP_SLOT_MAP);
    REQUIRE((a.hdr.flags & omgp::FLAG_error) == 0);

    omgp::l3::BpSlotMapResp r{};
    REQUIRE(omgp::l3::decode_bp_slot_map_resp(a.payload.data(), a.payload.size(), r) ==
            omgp::l3::Status::Ok);
    REQUIRE(r.slot_count == kSlots);
    REQUIRE(r.occupied.len == (kSlots + 7) / 8);
    REQUIRE(r.changed.len == (kSlots + 7) / 8);
    // protocol-l3 §3.1: bit (i % 8) of byte i/8, LSB first, is slot i.
    for (uint8_t i = 0; i < kSlots; ++i) {
        const bool occupied = (r.occupied.data[i / 8] >> (i % 8)) & 1;
        const bool changed = (r.changed.data[i / 8] >> (i % 8)) & 1;
        REQUIRE(occupied == (((kOccupied >> i) & 1) != 0));
        REQUIRE(changed == (((kChanged >> i) & 1) != 0));
    }
}

TEST_CASE("ErrorStep answers any request with a real ERROR (0x7F) frame carrying its code",
          "[core][mock_l3_node]") {
    FakeClock clock;
    MockL3Node node(clock);
    const L3Step script[] = {L3Step::of(ErrorStep{omgp::ERR_BUSY})};
    node.set_script(kNode, script, 1);

    const Answer a = ask(node, kNode, 0, request_msg(omgp::OP_GET_STATUS, kNode, 0), 1000);
    REQUIRE(a.got);
    REQUIRE(a.hdr.opcode == omgp::OP_ERROR);
    REQUIRE(a.hdr.node_id == kNode);
    REQUIRE((a.hdr.flags & omgp::FLAG_response) != 0);
    REQUIRE((a.hdr.flags & omgp::FLAG_error) != 0);
    omgp::l3::ErrorResp r{};
    REQUIRE(omgp::l3::decode_error_resp(a.payload.data(), a.payload.size(), r) ==
            omgp::l3::Status::Ok);
    REQUIRE(r.code == omgp::ERR_BUSY);
}

// --- The UNSET opcode class: decided, not implicit -------------------------------------------

// contracts/mock-l3-node.md covers an EXHAUSTED class (steady state) but never one with no step
// of its kind ever scripted, where F2's own mock-wire.md does state the answer. Recorded in
// docs/OPEN-QUESTIONS.md 2026-09-23 "MockL3Node: an opcode class with no step ever scripted,
// and how SilenceStep/ErrorStep are targeted" with Ruling: pending. This asserts the
// recommended default that T012 implements: ERR_UNKNOWN_OPCODE (0x01), because silence is
// reserved for SilenceStep — a second silent path would make an unscripted class
// indistinguishable from a deliberately mute node.
TEST_CASE("an opcode class with no step ever scripted answers ERROR/ERR_UNKNOWN_OPCODE, never "
          "silence (docs/OPEN-QUESTIONS.md 2026-09-23, ruling pending)",
          "[core][mock_l3_node]") {
    FakeClock clock;
    MockL3Node node(clock);
    const L3Step script[] = {L3Step::of(StatusStep{status_block(0, 5)})};
    node.set_script(kNode, script, 1);

    auto expect_unknown_opcode = [&node](uint8_t dst, uint8_t opcode, uint8_t seq, uint64_t at) {
        const Answer a = ask(node, dst, seq, request_msg(opcode, dst, seq), at);
        REQUIRE(a.got); // not silence
        REQUIRE(a.hdr.opcode == omgp::OP_ERROR);
        REQUIRE((a.hdr.flags & omgp::FLAG_error) != 0);
        omgp::l3::ErrorResp r{};
        REQUIRE(omgp::l3::decode_error_resp(a.payload.data(), a.payload.size(), r) ==
                omgp::l3::Status::Ok);
        REQUIRE(r.code == omgp::ERR_UNKNOWN_OPCODE);
    };

    SECTION("a class the script never mentions (IDENTIFY, with only a StatusStep scripted)") {
        expect_unknown_opcode(kNode, omgp::OP_IDENTIFY, 0, 1000);
    }
    SECTION("an opcode no step kind can answer at all (PING)") {
        expect_unknown_opcode(kNode, omgp::OP_PING, 0, 1000);
    }
    SECTION("a node with no script at all") {
        expect_unknown_opcode(kOtherNode, omgp::OP_GET_STATUS, 0, 1000);
    }
}

// --- What the double must NOT answer --------------------------------------------------------

TEST_CASE("a RESPONSE frame on the wire consumes no step and is never answered — the double "
          "would otherwise answer its own answers",
          "[core][mock_l3_node]") {
    FakeClock clock;
    MockL3Node node(clock);
    const L3Step script[] = {L3Step::of(StatusStep{status_block(1, 9)})};
    node.set_script(kNode, script, 1);

    const std::vector<uint8_t> msg =
        response_msg(omgp::OP_GET_STATUS, kNode, 0, 0, nullptr, 0); // a response, not a request
    const std::vector<uint8_t> frame = answer_frame(kNode, 0, msg);
    const uint64_t tx_end = node.transmit(frame.data(), frame.size(), 1000);
    node.advance_to(answer_deadline(tx_end));

    uint8_t b = 0;
    uint64_t start = 0;
    REQUIRE_FALSE(node.receive(b, start));
    REQUIRE(node.bytes_scheduled() == 0);
    REQUIRE(node.requests_seen() == 0);

    // ...and the script is untouched: the StatusStep is still there for the first real request.
    const Answer a = ask(node, kNode, 1, request_msg(omgp::OP_GET_STATUS, kNode, 1), 20000);
    REQUIRE(a.got);
    REQUIRE(a.hdr.opcode == omgp::OP_GET_STATUS);
}

// --- A wildcard is the whole NODE's disposition, not one class's ------------------------------

// contracts/mock-l3-node.md gives SilenceStep as "node/backplane does not answer at all
// (timeout)" and ErrorStep as "an explicit ERROR response" — neither names an opcode, so neither
// is a statement about one class. The consumption rule stays per class (a wildcard is taken, in
// script order, by the first class that reaches it), but a class with NO step of its own kind
// then falls back to the wildcard the node has already taken, not to the unset-class
// ERR_UNKNOWN_OPCODE default. Recorded in docs/OPEN-QUESTIONS.md 2026-09-23 (the superseding
// entry), still Ruling: pending.

TEST_CASE("a node scripted with one SilenceStep is mute to EVERY opcode class, not only to the "
          "first class that reached the step",
          "[core][mock_l3_node][timing:T_resp]") {
    FakeClock clock;
    MockL3Node node(clock);
    const L3Step script[] = {L3Step::of(SilenceStep{})};
    node.set_script(kNode, script, 1);
    Master master(node, clock, omgp::ADDR_host);

    // Four different classes, through the REAL Master: Status (which consumes the wildcard),
    // Identify, Event (whose §3.4 NONE default must not resurrect a dead node either) and PING
    // (no step kind answers it at all).
    const uint8_t opcodes[] = {omgp::OP_GET_STATUS, omgp::OP_IDENTIFY, omgp::OP_GET_EVENT,
                               omgp::OP_PING};
    for (uint8_t i = 0; i < 4; ++i) {
        INFO("opcode " << static_cast<int>(opcodes[i]));
        const TxOutcome out =
            transact(node, master, clock, kNode, request_msg(opcodes[i], kNode, i));
        REQUIRE(out.kind == MasterEvent::Failed);
        REQUIRE(out.reason == MasterEvent::Timeout);
    }

    // "Does not answer at all" (the contract's own words), for every class: not one byte, and
    // the double saw every attempt of every transaction.
    REQUIRE(node.bytes_scheduled() == 0);
    REQUIRE(node.requests_seen() == 4 * (omgp::TRUNK_retries + 1));
}

TEST_CASE("an ErrorStep node answers EVERY opcode class with the step's own code, never a "
          "second class with ERR_UNKNOWN_OPCODE",
          "[core][mock_l3_node]") {
    FakeClock clock;
    MockL3Node node(clock);
    const L3Step script[] = {L3Step::of(ErrorStep{omgp::ERR_BUSY})};
    node.set_script(kNode, script, 1);

    const uint8_t opcodes[] = {omgp::OP_GET_STATUS, omgp::OP_IDENTIFY, omgp::OP_GET_EVENT,
                               omgp::OP_PING};
    uint64_t at = 1000;
    for (uint8_t i = 0; i < 4; ++i) {
        INFO("opcode " << static_cast<int>(opcodes[i]));
        const Answer a = ask(node, kNode, i, request_msg(opcodes[i], kNode, i), at);
        at += 10000;
        REQUIRE(a.got);
        REQUIRE(a.hdr.opcode == omgp::OP_ERROR);
        REQUIRE((a.hdr.flags & omgp::FLAG_error) != 0);
        omgp::l3::ErrorResp r{};
        REQUIRE(omgp::l3::decode_error_resp(a.payload.data(), a.payload.size(), r) ==
                omgp::l3::Status::Ok);
        REQUIRE(r.code == omgp::ERR_BUSY); // the step's code, not the unset-class default
    }
}

TEST_CASE("a consumed wildcard never hijacks a class that has a step of its own kind: only a "
          "class the script never mentions falls back to it",
          "[core][mock_l3_node]") {
    FakeClock clock;
    MockL3Node node(clock);
    const omgp::l3::StatusBlock blk = status_block(3, 99);
    const L3Step script[] = {L3Step::of(ErrorStep{omgp::ERR_BUSY}), L3Step::of(StatusStep{blk})};
    node.set_script(kNode, script, 2);

    // IDENTIFY reaches the wildcard first and consumes it.
    const Answer id = ask(node, kNode, 0, request_msg(omgp::OP_IDENTIFY, kNode, 0), 1000);
    REQUIRE(id.got);
    REQUIRE(id.hdr.opcode == omgp::OP_ERROR);

    // GET_STATUS still gets its OWN step, then that step's steady state — a wildcard taken by
    // another class is a fallback for the unscripted, never an override of the scripted.
    for (uint8_t i = 0; i < 2; ++i) {
        const Answer a = ask(node, kNode, static_cast<uint8_t>(1 + i),
                             request_msg(omgp::OP_GET_STATUS, kNode, static_cast<uint8_t>(1 + i)),
                             10000 + i * 10000);
        REQUIRE(a.got);
        REQUIRE(a.hdr.opcode == omgp::OP_GET_STATUS);
        omgp::l3::StatusBlock got{};
        REQUIRE(omgp::l3::decode_status_block(a.payload.data(), a.payload.size(), got) ==
                omgp::l3::Status::Ok);
        REQUIRE(got.uptime_s == blk.uptime_s);
        REQUIRE(got.active_channel == blk.active_channel);
    }
}

// --- The bit rate in use (trunk §9), not a hard-wired reference rate --------------------------

TEST_CASE("the double schedules at the bit rate in use, so a rig moved to the fallback rate "
          "measures the fallback rate's own instants",
          "[core][mock_l3_node][timing:bit_rate][timing:bit_rate_fallback][timing:T_turn]") {
    FakeClock clock;
    MockL3Node node(clock);
    const L3Step script[] = {L3Step::of(StatusStep{status_block(1, 42)})};
    node.set_script(kNode, script, 1);

    REQUIRE(node.bit_rate() == omgp::TRUNK_bit_rate); // the reference rate until moved
    node.set_bit_rate(omgp::TRUNK_bit_rate_fallback); // trunk §9's own fallback symbol
    REQUIRE(node.bit_rate() == omgp::TRUNK_bit_rate_fallback);

    const uint64_t bt = byte_time_us(omgp::TRUNK_bit_rate_fallback);
    REQUIRE(bt > byte_us()); // the two rates are distinguishable at all, or this pins nothing

    const std::vector<uint8_t> msg = request_msg(omgp::OP_GET_STATUS, kNode, 0);
    const std::vector<uint8_t> frame = request_frame(kNode, 0, msg);
    const uint64_t at = 1000;
    const uint64_t tx_end = node.transmit(frame.data(), frame.size(), at);
    // link/byte_wire.hpp: transmit() reports the last stop bit AT THE RATE IN USE.
    REQUIRE(tx_end == at + static_cast<uint64_t>(frame.size()) * bt);

    node.advance_to(tx_end + omgp::TRUNK_T_turn_min_us + static_cast<uint64_t>(kMaxWire) * bt);
    std::vector<uint64_t> starts;
    uint8_t b = 0;
    uint64_t start = 0;
    while (node.receive(b, start))
        starts.push_back(start);

    REQUIRE(starts.size() > 1);
    // trunk §9: one T_turn after the request, then one byte every byte-time at that same rate.
    REQUIRE(starts.front() == tx_end + omgp::TRUNK_T_turn_min_us);
    for (size_t i = 1; i < starts.size(); ++i)
        REQUIRE(starts[i] - starts[i - 1] == bt);
}

// --- trunk §7: a retry is replayed, never re-scripted -----------------------------------------

TEST_CASE("a retry (same L2 seq, retry bit set) is answered from the node's single-frame replay "
          "buffer, byte-identically, and consumes no further step",
          "[core][mock_l3_node][timing:retries]") {
    FakeClock clock;
    MockL3Node node(clock);
    const omgp::l3::StatusBlock s1 = status_block(1, 11);
    const omgp::l3::StatusBlock s2 = status_block(2, 22);
    const L3Step script[] = {L3Step::of(StatusStep{s1}), L3Step::of(StatusStep{s2})};
    node.set_script(kNode, script, 2);

    const uint8_t l2seq = 0x05;
    const std::vector<uint8_t> msg = request_msg(omgp::OP_GET_STATUS, kNode, 0x07);
    const std::vector<uint8_t> first_frame =
        encode_one(kNode, omgp::ADDR_host, false, false, l2seq, msg);
    const uint64_t first_at = 1000;
    const uint64_t first_end = first_at + first_frame.size() * byte_us();
    const Answer first = exchange(node, first_frame, first_at, answer_deadline(first_end));
    REQUIRE(first.got);

    // docs/trunk-link-layer.md §7: "A node receiving a retry of a sequence it already answered
    // re-sends its previous response (single-frame replay buffer per node)." Same L2 seq, retry
    // bit set — byte-for-byte the frame Master::poll() retransmits.
    const std::vector<uint8_t> retry_frame =
        encode_one(kNode, omgp::ADDR_host, false, true, l2seq, msg);
    const uint64_t retry_at = 100000;
    const uint64_t retry_end = retry_at + retry_frame.size() * byte_us();
    const Answer again = exchange(node, retry_frame, retry_at, answer_deadline(retry_end));
    REQUIRE(again.got);
    REQUIRE(again.wire == first.wire); // the previous response, not the script's next step

    // ...and the second StatusStep is untouched: the next REAL request gets it. CLAUDE.md rule 2
    // — a retransmission at L2 must always be safe.
    const Answer third =
        ask(node, kNode, 0x06, request_msg(omgp::OP_GET_STATUS, kNode, 0x08), 200000);
    REQUIRE(third.got);
    omgp::l3::StatusBlock got{};
    REQUIRE(omgp::l3::decode_status_block(third.payload.data(), third.payload.size(), got) ==
            omgp::l3::Status::Ok);
    REQUIRE(got.uptime_s == s2.uptime_s);
    REQUIRE(got.active_channel == s2.active_channel);

    // The retry was SEEN — it was simply not re-scripted.
    REQUIRE(node.requests_seen() == 3);
}

// --- Two requests in flight: a named refusal, never a silently interleaved wire ---------------

TEST_CASE("a second answer that would overlap one still on the wire is refused by name, never "
          "merged byte-by-byte into an undecodable stream",
          "[core][mock_l3_node]") {
    FakeClock clock;
    MockL3Node node(clock);
    constexpr uint16_t kBlobLen = 200;
    uint8_t blob[kBlobLen];
    for (uint16_t i = 0; i < kBlobLen; ++i)
        blob[i] = static_cast<uint8_t>(i * 3 + 5);
    const L3Step script[] = {L3Step::of(DescChunkStep{blob, kBlobLen})};
    node.set_script(kNode, script, 1);

    // Two READ_DESCs back-to-back in ONE transmit(): not legal trunk traffic (§3 — the host runs
    // one transaction at a time), and the answers are long enough that the second one's window
    // opens while the first is still being clocked out. A double that merged them would hand the
    // rig bytes no Deframer can read, with nothing raised.
    uint8_t req[omgp::LIMIT_max_l3_payload];
    size_t req_len = 0;
    const omgp::l3::ReadDescReq rq{0, kMaxDescChunk};
    REQUIRE(omgp::l3::encode_read_desc_req(rq, req, sizeof req, req_len) == omgp::l3::Status::Ok);
    std::vector<uint8_t> burst =
        request_frame(kNode, 1, request_msg(omgp::OP_READ_DESC, kNode, 1, req, req_len));
    const std::vector<uint8_t> second =
        request_frame(kNode, 2, request_msg(omgp::OP_READ_DESC, kNode, 2, req, req_len));
    burst.insert(burst.end(), second.begin(), second.end());
    node.transmit(burst.data(), burst.size(), 1000);

    REQUIRE(node.requests_seen() == 2);
    const char* fault = node.take_fault();
    REQUIRE(fault != nullptr); // named, the way every other refusal in this double is

    // ...and what IS on the wire is one whole frame — the refused answer was never scheduled,
    // so nothing half-written or interleaved reached the rig.
    node.advance_to(1000 + 4 * static_cast<uint64_t>(kMaxWire) * byte_us());
    Deframer d;
    FrameView v{};
    size_t frames = 0;
    uint8_t b = 0;
    uint64_t start = 0;
    while (node.receive(b, start))
        if (d.feed(b, v))
            ++frames;
    REQUIRE(frames == 1);
}
