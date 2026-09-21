// Test-support: MockWire (spec 002 T010/T030) — the scripted step kinds plus the RX
// queue/transcript/PRNG machinery every later link/ engine test builds on.
// contracts/mock-wire.md. Respond/Silence landed with the T010 skeleton;
// CrcError/Duplicate with T029's own slice (test_link_master.cpp needs exactly those two,
// neither of which reads Step::count); Garbage/Babble/Rate with T030 (#48), against the
// tests in tests/unit/test_mock_wire.cpp (T028/#46). The Step::count widening the
// 2026-09-03 ruling on issue #48 directed was done by the maintainer 2026-09-14 (uint32_t,
// all four artefacts at once), which is what lets Kind::Rate read count as a bit rate.
//
// Respond/CrcError/Duplicate answer through the addressed node's registered
// omgp::link::RequestHandler, as contracts/mock-wire.md:16 specifies — build_response()
// below is the one place that is decided, and it invokes the handler once per REQUEST.
// The echo answer this file used to fabricate unconditionally was an interim default
// ratified 2026-09-03 (docs/OPEN-QUESTIONS.md 2026-09-01 item 1), taken only because no
// RequestHandler type existed at T010; since #147 that ruling covers just the no-handler
// fallback, whose wire bytes are unchanged.
#include "mock_wire.hpp"

#include "catch_amalgamated.hpp"
#include "link/crc16.hpp"
#include "link/frame.hpp"

#include <cstring>

namespace omgp_test {

namespace {
// Whether a raw (unstuffed) byte needs FLAG/ESCAPE byte-stuffing on the wire.
constexpr bool needs_stuffing(uint8_t b) {
    return b == omgp::TRUNK_flag_byte || b == omgp::TRUNK_escape_byte;
}

// The corrupted CRC high byte for Kind::CrcError: definitely wrong, and on the same side
// of the FLAG/ESCAPE stuffing boundary as `real_hi` (PR #137 review, MEDIUM). A bare XOR
// 0xFF crosses that boundary for exactly four values (0x7E/0x7D <-> 0x81/0x82), which would
// silently change the corrupted frame's wire length relative to the real response's and
// break every timing assertion built on that length (tests/unit/test_link_master.cpp
// computes expected instants from the UNCORRUPTED response's own encode_frame length).
// File-local: only encode_crc_corrupted() below (declared in mock_wire.hpp for reuse by
// tests/unit/test_link_loop.cpp) needs to call this.
uint8_t corrupt_crc_hi(uint8_t real_hi) {
    const uint8_t naive = static_cast<uint8_t>(real_hi ^ 0xFF);
    if (needs_stuffing(naive) == needs_stuffing(real_hi))
        return naive;
    if (needs_stuffing(real_hi))
        // real_hi is FLAG or ESCAPE: the other of the two also needs stuffing, and is a
        // different (still wrong) CRC byte.
        return real_hi == omgp::TRUNK_flag_byte ? omgp::TRUNK_escape_byte : omgp::TRUNK_flag_byte;
    // real_hi is 0x81 or 0x82 (naive would land on FLAG/ESCAPE): flip a low bit instead -
    // still a different, still non-stuffing byte.
    return static_cast<uint8_t>(real_hi ^ 0x01);
}
} // namespace

// Kind::CrcError (contracts/mock-wire.md): "the real response with its last CRC byte
// XOR 0xFF" (approximately - see corrupt_crc_hi() above for the wire-length-preserving
// exception). Duplicates encode_frame's unstuffed-build-then-stuff steps (link/frame.cpp)
// rather than post-processing its output, so every byte other than the corrupted CRC high
// byte is stuffed exactly as a real response's would be.
//
// This used to say encode_frame's refusals "do not apply here: `request` is a frame this same
// MockWire just decoded". That premise died when the symbol gained external linkage and a
// caller that builds FrameFields by hand (review @ef1ec22): PayloadTooLong is now enforced
// below, and the surviving divergence is ReservedAddress -- a `dst` of 0xFF is accepted here
// and refused by encode_frame. That is deliberate for a fault injector, which must be able to
// put a frame on the wire that the codec would not produce, and it is stated rather than
// resting on the retired premise.
size_t encode_crc_corrupted(const omgp::link::FrameFields& f, uint8_t* out, size_t cap) {
    using namespace omgp::link;
    // Same worst-case bound as encode_frame (link/frame.cpp), checked up front: this runs
    // on the engine-under-test's call stack (transmit() -> schedule_crc_error()), so a
    // capacity refusal must be a return value here, not a REQUIRE (see fault_'s
    // declaration in mock_wire.hpp for why this file never throws off that stack).
    // encode_frame's OTHER refusal, which this used to skip on the grounds that `request` was
    // "a frame this same MockWire just decoded" -- no longer true since the symbol gained
    // external linkage and gained a caller that builds FrameFields by hand (review @c69679c).
    // Without it, f.len = 200 with a 512-byte `out` passes the capacity check below (414 <=
    // 512) and then writes 206 bytes into unstuffed[kMaxUnstuffed], which is 70: a stack
    // overflow. Every in-repo caller happens to pass cap <= kMaxWire, which bounds f.len to
    // 64 -- but that is a property of the repo's current contents, a control, not a guarantee
    // this function may rely on.
    if (f.len > omgp::LIMIT_max_l3_message)
        return 0;
    const size_t needed = 2 + 2 * (kHeaderLen + static_cast<size_t>(f.len) + kCrcLen);
    if (cap < needed)
        return 0;
    uint8_t unstuffed[kMaxUnstuffed];
    size_t n = 0;
    unstuffed[n++] = f.dst;
    unstuffed[n++] = f.src;
    unstuffed[n++] = static_cast<uint8_t>((f.response ? 0x01 : 0x00) | (f.retry ? 0x02 : 0x00) |
                                          static_cast<uint8_t>((f.seq & 0x0F) << 4));
    unstuffed[n++] = f.len;
    for (uint8_t i = 0; i < f.len; ++i)
        unstuffed[n++] = f.payload[i];
    const uint16_t c = omgp::crc16_ccitt_false(unstuffed, n);
    unstuffed[n++] = static_cast<uint8_t>(c & 0xFF);
    unstuffed[n++] = corrupt_crc_hi(static_cast<uint8_t>((c >> 8) & 0xFF)); // the corruption

    size_t w = 0;
    out[w++] = omgp::TRUNK_flag_byte;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = unstuffed[i];
        if (b == omgp::TRUNK_flag_byte) {
            out[w++] = omgp::TRUNK_escape_byte;
            out[w++] = static_cast<uint8_t>(omgp::TRUNK_flag_byte ^ omgp::TRUNK_escape_xor);
        } else if (b == omgp::TRUNK_escape_byte) {
            out[w++] = omgp::TRUNK_escape_byte;
            out[w++] = static_cast<uint8_t>(omgp::TRUNK_escape_byte ^ omgp::TRUNK_escape_xor);
        } else {
            out[w++] = b;
        }
    }
    out[w++] = omgp::TRUNK_flag_byte;
    return w;
}

namespace {
// The response FrameFields a conforming node sends back to `request` — dst/src swapped,
// response bit set, seq echoed. Shared by all three answering Kinds: they differ only in
// what bytes reach the wire and when, not in whom the response is addressed to. The
// payload starts as the request's own (the no-handler echo); build_response() below
// replaces it with the node's RequestHandler's answer when one is registered.
omgp::link::FrameFields response_fields(const omgp::link::FrameFields& request) {
    omgp::link::FrameFields r{};
    r.dst = request.src;
    r.src = request.dst;
    r.response = true;
    r.retry = false;
    r.seq = request.seq;
    r.len = request.len;
    r.payload = request.payload;
    return r;
}

// Whether a fresh REAL Deframer delivers any frame from `bytes`. contracts/mock-wire.md's
// Garbage row promises bytes "never containing a valid frame — checked at generation", and
// the parser the engine under test will actually run is the only honest checker of that.
bool contains_frame(const uint8_t* bytes, size_t n) {
    omgp::link::Deframer d;
    omgp::link::FrameView view{};
    for (size_t i = 0; i < n; ++i)
        if (d.feed(bytes[i], view))
            return true;
    return false;
}

// How long the wrong-rate branch of Kind::Rate babbles for. contracts/mock-wire.md's Rate row
// defers to the Garbage row but has no field left to carry a LENGTH — `count` is the bit rate
// and `seed` selects the branch — and neither it nor research.md R-07 names one. One full
// frame's worth of line noise is what a receiver mis-sampling a foreign-rate transmission sees
// over a frame time, and it fits the RX queue (kMaxBurst is 4 x kMaxWire). Recorded, with the
// options weighed, in docs/OPEN-QUESTIONS.md 2026-09-15 "Kind::Rate's wrong-rate Garbage branch
// names no burst LENGTH" — ruling pending, and tests/unit/test_mock_wire.cpp deliberately does
// not assert this length, so a different ruling changes this constant and no test.
constexpr uint32_t kWrongRateNoiseBytes = static_cast<uint32_t>(omgp::link::kMaxWire);

// How many times schedule_noise() re-draws a burst that came out containing a valid frame
// before giving up and recording a fault. Bounded so a pathological seed cannot spin on the
// engine-under-test's call stack; see schedule_noise() for how likely the re-draw is at all.
constexpr unsigned kNoiseRedrawLimit = 8;
} // namespace

uint32_t xorshift32_next(uint32_t& state) {
    // xorshift32 has a fixed point at state == 0 (every subsequent call returns 0). A
    // Step left with the default seed == 0 (every step authored so far) would otherwise
    // yield a constant all-zero stream once Garbage/Babble (T030) start consuming this —
    // deterministic, but degenerate, and seed != 0 is also given a separate meaning for
    // Kind::Rate (contracts/mock-wire.md). Re-seed to a fixed nonzero constant instead.
    // 0xFFFFFFFF, not a "known good" PRNG seed constant (unlike e.g. the golden ratio),
    // so a script author picking their own nonzero seed is unlikely to land on the exact
    // value this repair uses and get an accidental collision with the unset (0) case —
    // see mock_wire.hpp.
    if (state == 0)
        state = 0xFFFFFFFFu;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

MockWire::MockWire(FakeClock& clock) : clock_(clock) {}

MockWire::~MockWire() {
    // Non-throwing (CHECK, not REQUIRE): a destructor is the last chance to catch a fault
    // recorded by a transmit()/receive() that ran after the test's final advance_to() —
    // transcript_size()/transcript() only see faults raised before they're called, and
    // nothing else drains fault_ once the test stops calling advance_to(). Throwing here
    // would risk std::terminate if already unwinding, so this reports rather than aborts.
    INFO((fault_ != nullptr ? fault_ : ""));
    CHECK(fault_ == nullptr);
    // #148: injected bytes the engine never consumed. A case that plants a fault with
    // inject_bytes() and lets the wire die before receive() releases those bytes asserted
    // nothing about the fault it named, while passing (contracts/mock-wire.md "Scheduling":
    // the no-silent-loss rule its capacity bullet states for DROPPED bytes, extended to
    // undelivered ones). Non-throwing for the same reason as the fault_ drain above — a
    // destructor must not throw while unwinding. take_pending_injected() is how a case that
    // means to leave residue says so; the count is in the message either way.
    INFO("MockWire: " << injected_pending_
                      << " injected RX byte(s) never delivered to the engine (inject_bytes() "
                         "residue; advance far enough to consume them, or acknowledge with "
                         "take_pending_injected())");
    CHECK(injected_pending_ == 0);
}

void MockWire::set_script(uint8_t node, const Step* steps, size_t count) {
    // Step::node documents which node a step was authored for; require it to agree with
    // the array's registered node so a mismatch (e.g. a Step{.node = 5, ...} registered
    // via set_script(3, ...)) fails loudly here instead of the step being silently
    // unreachable with no compiler or runtime diagnostic.
    for (size_t i = 0; i < count; ++i)
        REQUIRE(steps[i].node == node);
    if (node == 0xFF) {
        wildcard_script_ = steps;
        wildcard_len_ = count;
        for (size_t& pos : wildcard_pos_)
            pos = 0;
        return;
    }
    REQUIRE(node < omgp::link::kAddrCount);
    scripts_[node] = steps;
    script_len_[node] = count;
    script_pos_[node] = 0;
}

void MockWire::set_handler(uint8_t node, omgp::link::RequestHandler& handler) {
    // Runs on the test's own call stack (a rig-setup call, never inside transmit()), so an
    // out-of-range address is REQUIRE'd here rather than deferred through fault_ — same
    // treatment set_script() gives it. There is no 0xFF wildcard seat: contracts/mock-wire.md
    // gives Respond's answer to "the node's" handler, one node at a time.
    REQUIRE(node < omgp::link::kAddrCount);
    handlers_[node] = &handler;
}

const Step* MockWire::next_step(uint8_t node) {
    if (node < omgp::link::kAddrCount && scripts_[node] != nullptr &&
        script_pos_[node] < script_len_[node]) {
        return &scripts_[node][script_pos_[node]++];
    }
    // Own script exhausted or never set: draw from the shared 0xFF fallback script next,
    // each node at its own cursor; only once that is also exhausted (or unset) does the
    // default Respond apply. contracts/mock-wire.md's "an exhausted script behaves as
    // Respond with the default delay" is read here as the combined own+wildcard sequence
    // for a node, not "own-script exhaustion skips straight past the wildcard".
    if (node < omgp::link::kAddrCount && wildcard_script_ != nullptr &&
        wildcard_pos_[node] < wildcard_len_) {
        return &wildcard_script_[wildcard_pos_[node]++];
    }
    return nullptr; // exhausted/unset: default Respond, TRUNK_T_turn_min_us delay
}

void MockWire::enqueue(uint8_t byte, uint64_t start_us, bool injected) {
    // contracts/mock-wire.md "Capacity": never a SILENT drop — but this runs on the
    // engine-under-test's call stack (transmit() -> schedule_respond()), so the failure
    // is recorded and REQUIRE'd later, at the test's own advance_to() call, rather than
    // thrown here (see fault_'s declaration in mock_wire.hpp).
    if (rx_count_ >= kRxCapacity) {
        if (fault_ == nullptr)
            fault_ = "MockWire: RX queue capacity exceeded (4 * kMaxWire)";
        // Returns BEFORE injected_pending_ is bumped (#148): a byte that never reached the
        // queue is a DROPPED byte, already reported by the fault above, not an undelivered
        // one. Counting it here too would fail the same byte twice, and would leave residue
        // the test cannot drain by advancing (it is not on the wire to release).
        return;
    }
    // Insertion-sort by start_us (stable: only strictly-later entries shift right) so
    // receive() can always take index 0 in start-instant order, per byte-wire-and-clock.md,
    // even when a later transmit() schedules an earlier-firing response (e.g. a short
    // default delay for one node queued after a long "late response" delay for another).
    size_t pos = rx_count_;
    while (pos > 0 && rx_queue_[pos - 1].start_us > start_us) {
        rx_queue_[pos] = rx_queue_[pos - 1];
        --pos;
    }
    rx_queue_[pos] = QueuedByte{byte, start_us, injected};
    ++rx_count_;
    if (injected)
        ++injected_pending_;
}

void MockWire::record_transcript(const omgp::link::FrameFields& f, uint64_t tx_start_us) {
    // Same deferred-fault reasoning as enqueue(): this runs on transmit()'s (engine) call
    // stack, so a capacity overrun is recorded, not thrown, here.
    if (transcript_count_ >= kTranscriptCapacity) {
        if (fault_ == nullptr)
            fault_ = "MockWire: transcript capacity exceeded (kTranscriptCapacity)";
        return;
    }
    TxRecord& rec = transcript_[transcript_count_++];
    rec.dst = f.dst;
    rec.src = f.src;
    rec.response = f.response;
    rec.retry = f.retry;
    rec.seq = f.seq;
    rec.len = f.len;
    if (f.len > 0)
        std::memcpy(rec.payload, f.payload, f.len);
    rec.tx_start_us = tx_start_us;
}

void MockWire::enqueue_frame(const uint8_t* buf, size_t written, uint64_t t0, bool injected) {
    for (size_t i = 0; i < written; ++i)
        enqueue(buf[i], t0 + static_cast<uint64_t>(i) * omgp::link::byte_time_us(bit_rate_),
                injected);
}

bool MockWire::build_response(const omgp::link::FrameFields& request,
                              uint8_t (&payload_buf)[omgp::LIMIT_max_l3_message],
                              omgp::link::FrameFields& out) {
    out = response_fields(request);

    // transmit() already refuses a request whose dst is not a trunk §5 node address, so the
    // bound below is not what keeps this index safe today — it is restated here so this
    // function is safe to index with on its own terms, whoever calls it next.
    omgp::link::RequestHandler* handler =
        request.dst < omgp::link::kAddrCount ? handlers_[request.dst] : nullptr;
    if (handler == nullptr)
        // No handler for this node: the interim echo, which is what response_fields() already
        // built. Ratified 2026-09-03 (docs/OPEN-QUESTIONS.md 2026-09-01) and since #147 this
        // fallback is the ONLY thing that ruling still covers — the wire bytes here are
        // byte-for-byte what they were before the handler seat existed, which is why
        // tests/unit/test_link_master.cpp's response-length timing assertions are untouched.
        return true;

    // contracts/link-cpp.md "Responder engine": handle(req, len, resp, cap) returns how many
    // bytes of `resp` it wrote. `cap` is the protocol's own payload bound, the largest answer
    // a frame can carry (docs/protocol-l3.md; LIMIT_max_l3_message).
    const size_t n =
        handler->handle(request.payload, request.len, payload_buf, omgp::LIMIT_max_l3_message);
    if (n > omgp::LIMIT_max_l3_message) {
        // A handler claiming more than a frame can carry. Neither truncating (which would put
        // a silently-wrong answer on the wire and make the engine under test assert against
        // it) nor dropping quietly (indistinguishable from Kind::Silence): the answer is
        // refused and named, on the same deferred-fault path as every other transmit()-stack
        // failure here (see fault_'s declaration in mock_wire.hpp).
        if (fault_ == nullptr)
            fault_ = "MockWire: RequestHandler returned a response longer than "
                     "LIMIT_max_l3_message";
        return false;
    }
    // n == 0 is a legitimate answer: a zero-payload response FRAME, not silence. Silence
    // stays reachable only through Kind::Silence.
    out.len = static_cast<uint8_t>(n);
    out.payload = payload_buf;
    return true;
}

void MockWire::schedule_respond(const omgp::link::FrameFields& request, uint64_t tx_end,
                                uint32_t delay_us) {
    using omgp::link::encode_frame;
    using omgp::link::kMaxWire;
    using omgp::link::Status;

    // contracts/mock-wire.md:16: "the node's RequestHandler ... answers". build_response()
    // invokes it once, or falls back to the interim echo for a node with none registered.
    uint8_t payload_buf[omgp::LIMIT_max_l3_message];
    omgp::link::FrameFields response{};
    if (!build_response(request, payload_buf, response))
        return; // fault already recorded; nothing goes on the wire
    uint8_t buf[kMaxWire];
    size_t written = 0;
    // response.dst == request.src. A well-formed request from a real Master/Responder
    // never carries src == 0xFF (trunk addresses are 0x00..0x0F, kAddrCount), so
    // encode_frame is not expected to refuse this response. This still runs on
    // transmit()'s (engine) call stack though, so a refusal is recorded via fault_
    // rather than REQUIRE'd here — see fault_'s declaration in mock_wire.hpp — turning a
    // script/engine bug that violates the assumption into a loud failure (at the next
    // advance_to()) instead of a silent, Silence-indistinguishable no-response
    // (contracts/mock-wire.md "Capacity": dropped bytes are never silent here either).
    if (encode_frame(response, buf, sizeof buf, written) != Status::Ok) {
        if (fault_ == nullptr)
            fault_ = "MockWire: encode_frame refused a Respond answer (response.dst == 0xFF?)";
        return;
    }
    enqueue_frame(buf, written, tx_end + delay_us);
}

void MockWire::schedule_crc_error(const omgp::link::FrameFields& request, uint64_t tx_end,
                                  uint32_t delay_us) {
    using omgp::link::kMaxWire;

    // "the real response with its last CRC byte replaced" — and since #147 the real response
    // is the node's RequestHandler's, built by the one invocation below, not an echo.
    uint8_t payload_buf[omgp::LIMIT_max_l3_message];
    omgp::link::FrameFields response{};
    if (!build_response(request, payload_buf, response))
        return;
    uint8_t buf[kMaxWire];
    const size_t written = encode_crc_corrupted(response, buf, sizeof buf);
    if (written == 0) {
        if (fault_ == nullptr)
            fault_ = "MockWire: encode_crc_corrupted refused a CrcError answer (buffer too small?)";
        return;
    }
    enqueue_frame(buf, written, tx_end + delay_us);
}

void MockWire::schedule_duplicate(const omgp::link::FrameFields& request, uint64_t tx_end,
                                  uint32_t delay_us) {
    using omgp::link::encode_frame;
    using omgp::link::kMaxWire;
    using omgp::link::Status;

    // ONE invocation for the request, two copies of its answer on the wire (contracts/
    // mock-wire.md's Duplicate row: "the real response, then the same bytes again"). A
    // per-copy invocation would hand a stateful handler — a real Responder's replay buffer,
    // the eventual consumer of this seat — a second, spurious transaction to account for.
    uint8_t payload_buf[omgp::LIMIT_max_l3_message];
    omgp::link::FrameFields response{};
    if (!build_response(request, payload_buf, response))
        return;
    uint8_t buf[kMaxWire];
    size_t written = 0;
    if (encode_frame(response, buf, sizeof buf, written) != Status::Ok) {
        if (fault_ == nullptr)
            fault_ = "MockWire: encode_frame refused a Duplicate answer (response.dst == 0xFF?)";
        return;
    }
    // contracts/mock-wire.md: "the real response, then the same bytes again delay_us
    // after the first ends" — the first copy is the ordinary prompt answer (like Respond,
    // at the default turnaround: this row never restates "first byte at request_end +
    // delay_us" the way Respond/CrcError's rows do), and delay_us instead names the GAP
    // before the repeated copy — the one this Kind exists to plant as a late duplicate.
    const uint64_t first_t0 = tx_end + omgp::TRUNK_T_turn_min_us;
    enqueue_frame(buf, written, first_t0);
    const uint64_t first_end =
        first_t0 + static_cast<uint64_t>(written) * omgp::link::byte_time_us(bit_rate_);
    enqueue_frame(buf, written, first_end + delay_us);
}

void MockWire::schedule_noise(uint64_t t0, uint32_t count, uint32_t seed) {
    // A zero-length burst is indistinguishable on the wire from Kind::Silence, so a step
    // asking for one is a script bug rather than a legitimate no-op — named, on the same
    // deferred-fault path as every other transmit()-stack failure here (see fault_'s
    // declaration in mock_wire.hpp), not quietly honoured.
    if (count == 0) {
        if (fault_ == nullptr)
            fault_ = "MockWire: Kind::Garbage/Babble step with count == 0 (use Kind::Silence)";
        return;
    }
    if (count > kMaxBurst) {
        if (fault_ == nullptr)
            fault_ = "MockWire: Kind::Garbage/Babble count exceeds the RX queue capacity";
        return;
    }

    uint8_t buf[kMaxBurst];
    uint32_t state = seed;
    // "never containing a valid frame — checked at generation" (contracts/mock-wire.md). The
    // check is the real Deframer's; on a hit the burst is re-drawn from the ALREADY-ADVANCED
    // PRNG state, which is the rule data-model.md §11 gives torture.py for a corruption that
    // still parses ("regenerated with the next sub-seed ... never emitted") and stays
    // deterministic for a fixed seed either way.
    //
    // Honest scope (CLAUDE.md rule 11): for random bytes to deliver a frame they must carry
    // two FLAGs bracketing a well-formed, CRC-valid body — order 2^-16 per candidate even
    // once the FLAGs land — so no script in this repo is expected to take the re-draw, and
    // nothing here demonstrates that branch. What the loop establishes is that the promise is
    // VERIFIED at generation rather than merely overwhelmingly likely; the guarantee that the
    // emitted burst is frame-free is proved by construction (it is exactly the condition
    // enqueue_frame() below is reached under).
    for (unsigned attempt = 0; attempt < kNoiseRedrawLimit; ++attempt) {
        for (uint32_t i = 0; i < count; ++i)
            buf[i] = static_cast<uint8_t>(xorshift32_next(state) & 0xFF);
        if (!contains_frame(buf, count)) {
            enqueue_frame(buf, count, t0);
            return;
        }
    }
    if (fault_ == nullptr)
        fault_ = "MockWire: no frame-free Garbage/Babble burst for this seed within the "
                 "re-draw limit";
}

void MockWire::fire_foreign_babble(uint8_t addressed, uint64_t tx_end) {
    // contracts/mock-wire.md's Babble row: "`count` PRNG bytes at `request_end + delay_us`
    // regardless of addressee (also emitted when a different node is polled, i.e. outside any
    // window)". That is the whole point of the Kind — docs/trunk-link-layer.md §3 forbids a
    // node transmitting outside its own response window, and a step that only ever fired when
    // its own node was polled could not stage the violation at all.
    for (uint8_t n = 0; n < static_cast<uint8_t>(omgp::link::kAddrCount); ++n) {
        // The addressed node's own head step is transmit()'s switch to consume; firing it
        // here too would emit the burst twice.
        if (n == addressed)
            continue;
        // Per-node scripts only, NOT the 0xFF wildcard fallback: each node draws the wildcard
        // at its own cursor (next_step()), so a wildcard Babble step scanned here would fire
        // kAddrCount-1 bursts for a single request — a rig-wide storm nobody authored. A
        // wildcard Babble is therefore not scriptable at all: transmit()'s Kind::Babble arm
        // refuses it by name rather than let it reach the ADDRESSED node alone (which would be
        // Kind::Garbage under another name). docs/OPEN-QUESTIONS.md 2026-09-15 "A `Kind::Babble`
        // step in the 0xFF wildcard script" holds the question.
        if (scripts_[n] == nullptr || script_pos_[n] >= script_len_[n])
            continue;
        const Step& s = scripts_[n][script_pos_[n]];
        if (s.kind != Kind::Babble)
            continue;
        // A node deafened by a Kind::Rate step did not hear THIS request either, so its script
        // does not advance here any more than it does for the addressed node three lines below
        // transmit()'s call to this — the Babble step stays at the head, waiting for the first
        // request the node does hear. The Babble row's independence is of the ADDRESSEE;
        // deafness is a different axis and no artefact speaks to it, so both readings (and why
        // this one) are in docs/OPEN-QUESTIONS.md 2026-09-15 "A node deafened by `Kind::Rate`:
        // does its pending `Kind::Babble` step still fire?". Without this check the Kind became
        // addressee-DEPENDENT for exactly the nodes carrying both — a burst for another node's
        // poll, silence for its own (PR #610 red-team round 1, finding 1).
        if (!hears_current_rate(n))
            continue;
        ++script_pos_[n]; // consumed: a babbler emits once per step, it does not latch
        schedule_noise(tx_end + s.delay_us, s.count, s.seed);
    }
}

bool MockWire::hears_current_rate(uint8_t node) const {
    const uint32_t hears = node_rate_[node];
    // 0 is the "no Kind::Rate step seen" sentinel (mock_wire.hpp): the node hears whatever the
    // wire uses, which is every node's state until a Rate step arms it.
    return hears == 0 || hears == bit_rate_;
}

bool MockWire::deaf_to_current_rate(uint8_t node, uint64_t tx_end) {
    if (hears_current_rate(node))
        return false;
    // contracts/mock-wire.md's Rate row: "requests at another rate behave as `Silence` (or
    // `Garbage` if `seed != 0`)". The seed and delay are the ARMING step's — this request has
    // no step of its own to read them from (docs/OPEN-QUESTIONS.md 2026-09-15).
    if (node_rate_seed_[node] != 0)
        schedule_noise(tx_end + node_rate_delay_[node], kWrongRateNoiseBytes,
                       node_rate_seed_[node]);
    return true;
}

void MockWire::inject_bytes(const uint8_t* bytes, size_t n, uint64_t start_us) {
    // Same RX-queue path a scheduled response takes (enqueue_frame -> enqueue's insertion
    // sort), so injected bytes interleave with any scripted traffic in start-instant order.
    // No parser_, no transcript, no next_step: this is raw wire from another station, not a
    // request MockWire should answer.
    // injected = true: these bytes, and only these, are the ones ~MockWire() requires a case
    // to have consumed or acknowledged (#148).
    enqueue_frame(bytes, n, start_us, /*injected=*/true);
}

uint64_t MockWire::transmit(const uint8_t* bytes, size_t n, uint64_t now_us) {
    using omgp::link::byte_time_us;
    using omgp::link::FrameView;

    const uint64_t tx_end = now_us + static_cast<uint64_t>(n) * byte_time_us(bit_rate_);

    FrameView view{};
    // Act on each decoded frame immediately, inside the feed loop: view.f.payload points
    // into parser_'s own accumulator and is valid only until the next feed() call
    // (link/frame.hpp), so deferring to after the loop would read a payload already
    // overwritten (or, for a call carrying several concatenated frames, would silently
    // drop every frame but the last — no transcript entry, no scheduled response).
    for (size_t i = 0; i < n; ++i) {
        const uint64_t byte_us = now_us + static_cast<uint64_t>(i) * byte_time_us(bit_rate_);
        const bool is_flag = (bytes[i] == omgp::TRUNK_flag_byte);
        // The instant of whichever FLAG most recently opened the accumulation now in
        // progress — persisted in open_flag_us_ across transmit() calls (mock_wire.hpp),
        // so a frame whose opening FLAG arrived in an earlier call still gets that call's
        // instant as its tx_start_us, not this call's.
        const uint64_t frame_tx_start = open_flag_us_;

        const bool delivered = parser_.feed(bytes[i], view);

        // on_flag() (link/frame.cpp) unconditionally resets accumulation on every FLAG
        // byte, whichever branch it takes, so this FLAG now opens the next frame either
        // way — update after feed() so frame_tx_start above still reads the *previous*
        // opening FLAG's instant for a frame delivered on this very byte.
        if (is_flag)
            open_flag_us_ = byte_us;

        if (!delivered)
            continue;

        const uint64_t frame_tx_end = byte_us + byte_time_us(bit_rate_);

        record_transcript(view.f, frame_tx_start);

        // contracts/mock-wire.md scopes every Kind to "the next request addressed to
        // node"; a response frame transmitted by the engine under test (Responder, from
        // T031) must not itself consume a script step or be echoed an answer.
        if (view.f.response)
            continue;

        // docs/trunk-link-layer.md §5: only 0x00..0x0F (kAddrCount) are node addresses.
        // encode_frame/Deframer reject only dst == 0xFF, so a request mis-addressed to
        // 0x10..0xFE would otherwise still resolve via next_step()'s default-Respond
        // fallback (next_step only checks node < kAddrCount to *look up* a script, not
        // whether the address is real) and get answered as if that node existed — hiding
        // exactly the mis-addressing bug the mock exists to expose. Silence is the
        // faithful wire behaviour (no such node to answer); also surfaced via fault_ so a
        // test can't pass by relying on it.
        if (view.f.dst >= omgp::link::kAddrCount) {
            if (fault_ == nullptr)
                fault_ = "MockWire: request addressed to dst >= kAddrCount (not a node)";
            continue;
        }

        // contracts/mock-wire.md's Babble row fires "regardless of addressee": another
        // node's pending Babble step babbles over THIS request, whoever it was addressed
        // to. Before the addressed node's own step is even looked up, so a babbler is heard
        // whatever that step turns out to be (including a refusal that records a fault).
        fire_foreign_babble(view.f.dst, frame_tx_end);

        // Kind::Rate's standing effect, checked BEFORE next_step(): a node that cannot hear
        // the wire's current rate never received this request at all, so its script must not
        // advance — whatever step it has queued is still waiting for a request it does hear.
        if (deaf_to_current_rate(view.f.dst, frame_tx_end))
            continue;

        const Step* step = next_step(view.f.dst);
        const Kind kind = step != nullptr ? step->kind : Kind::Respond;
        const uint32_t delay_us = step != nullptr ? step->delay_us : omgp::TRUNK_T_turn_min_us;
        // Read here rather than at each use so the Garbage/Babble/Rate arms below need no
        // nullptr dance: `step` is non-null whenever `kind` is one of theirs (the default
        // that makes it null is Kind::Respond), but that is an argument, not something the
        // arms should each restate.
        const uint32_t count = step != nullptr ? step->count : 0;
        const uint32_t seed = step != nullptr ? step->seed : 0;

        switch (kind) {
        case Kind::Respond:
            schedule_respond(view.f, frame_tx_end, delay_us);
            break;
        case Kind::Silence:
            break;
        case Kind::CrcError:
            schedule_crc_error(view.f, frame_tx_end, delay_us);
            break;
        case Kind::Duplicate:
            schedule_duplicate(view.f, frame_tx_end, delay_us);
            break;
        case Kind::Garbage:
            // "count PRNG bytes ... starting at request_end + delay_us, then nothing": no
            // answer is scheduled alongside, which is what separates this from CrcError.
            schedule_noise(frame_tx_end + delay_us, count, seed);
            break;
        case Kind::Babble:
            // A wildcard step (set_script() requires Step::node == 0xFF for every step in the
            // 0xFF script, so this is exactly "drawn from the wildcard") is refused by name
            // rather than honoured as the narrower thing the mock could carry out.
            // contracts/mock-wire.md:25 makes a wildcard step "apply to every node" and :21
            // makes Babble fire "regardless of addressee"; together they would put one burst
            // on the wire per node per request — kAddrCount-1 bursts nobody authored, whose
            // total overruns the RX queue once count exceeds kRxCapacity / (kAddrCount - 1),
            // i.e. 37 bytes (below that they fit, so the overrun is a property of the count the
            // script picked, not of kAddrCount) — and neither artefact names a count, so the
            // author cannot see which of the two they are writing. Honouring it only here
            // would instead make the step reach the ADDRESSED node alone, i.e. Kind::Garbage
            // wearing another Kind's name: the one property the Babble row states, silently
            // absent. Neither reading is written down, so per CLAUDE.md the ambiguity is
            // recorded, not resolved here — docs/OPEN-QUESTIONS.md 2026-09-15 "A `Kind::Babble`
            // step in the 0xFF wildcard script" — and the step is refused on the same
            // deferred-fault path as schedule_noise()'s two refusals (PR #610 review round 2,
            // finding 1). Per-node Babble scripts are unaffected.
            if (step != nullptr && step->node == 0xFF) {
                if (fault_ == nullptr)
                    fault_ = "MockWire: Kind::Babble in the 0xFF wildcard script (script it per "
                             "node; see docs/OPEN-QUESTIONS.md 2026-09-15)";
                break;
            }
            // Otherwise the same burst Garbage emits — what makes a step Babble is that
            // fire_foreign_babble() above also fires it from a node that was NOT addressed.
            // Reaching it here means the babbler happens to be this request's addressee too.
            schedule_noise(frame_tx_end + delay_us, count, seed);
            break;
        case Kind::Rate:
            // "the node NOW hears only at `count` interpreted as bit rate": standing state,
            // kept per node (mock_wire.hpp), not a one-request effect.
            if (count == 0) {
                // The sentinel for "no Rate step seen" (deaf_to_current_rate()), and not a
                // rate any node could hear at: a script asking for it is a bug, and letting
                // it through would silently DISARM the node instead.
                if (fault_ == nullptr)
                    fault_ = "MockWire: Kind::Rate step with count == 0 (count is a bit rate)";
                break;
            }
            node_rate_[view.f.dst] = count;
            node_rate_seed_[view.f.dst] = seed;
            node_rate_delay_[view.f.dst] = delay_us;
            // ...and it governs this very request too ("NOW"). The check above ran before
            // this step was known, so it is repeated here against the rate just armed.
            if (!deaf_to_current_rate(view.f.dst, frame_tx_end))
                // Heard after all — the step names the rate the wire is already using — so
                // the request is answered like Kind::Respond, at this step's own delay.
                schedule_respond(view.f, frame_tx_end, delay_us);
            break;
        }
    }

    return tx_end;
}

bool MockWire::receive(uint8_t& byte, uint64_t& start_us) {
    if (rx_count_ == 0)
        return false;
    // rx_queue_ is kept sorted by start_us (enqueue()), so index 0 is always the
    // earliest-start-instant pending byte, matching byte-wire-and-clock.md's release order.
    const QueuedByte& front = rx_queue_[0];
    if (front.start_us > clock_.now_us())
        return false; // "in the future": stays queued (byte-wire-and-clock.md)
    byte = front.byte;
    start_us = front.start_us;
    // Delivered, so it is no longer outstanding (#148). The `> 0` conjunct is belt and braces,
    // not what makes the count safe: injected_pending_ is incremented for exactly the bytes
    // whose flag is set, and take_pending_injected() clears both together, so front.injected
    // implies injected_pending_ >= 1 and the conjunct is never the reason this is skipped
    // (proved by construction — those are the only two places either is written).
    if (front.injected && injected_pending_ > 0)
        --injected_pending_;
    for (size_t i = 1; i < rx_count_; ++i)
        rx_queue_[i - 1] = rx_queue_[i];
    --rx_count_;
    return true;
}

uint32_t MockWire::bit_rate() const {
    return bit_rate_;
}

void MockWire::set_bit_rate(uint32_t bps) {
    bit_rate_ = bps;
}

size_t MockWire::pending_injected() const {
    return injected_pending_;
}

size_t MockWire::take_pending_injected() {
    const size_t n = injected_pending_;
    injected_pending_ = 0;
    // Clear the per-byte flags too, not just the total: the bytes stay queued and stay
    // releasable, and a later receive() of one of them must not take the count below zero
    // (receive() guards that as well — this is the other half of the same invariant). Bytes
    // injected AFTER this call are tracked again from zero, so an acknowledgement covers the
    // residue outstanding when it was made and nothing later.
    for (size_t i = 0; i < rx_count_; ++i)
        rx_queue_[i].injected = false;
    return n;
}

const char* MockWire::take_fault() {
    const char* f = fault_;
    fault_ = nullptr;
    return f;
}

void MockWire::advance_to(uint64_t t) {
    // Drains a fault recorded by transmit()'s call stack (see fault_'s declaration in
    // mock_wire.hpp) — this call, unlike transmit(), always runs on the test's own stack,
    // so REQUIRE-ing here is safe.
    INFO((fault_ != nullptr ? fault_ : ""));
    REQUIRE(fault_ == nullptr);
    // byte-wire-and-clock.md: the clock is monotonic and never goes backwards; setting it
    // earlier than its current instant would also silently make already-due RX bytes
    // future again (receive() gates on start_us <= now_us()).
    REQUIRE(t >= clock_.now_us());
    clock_.set(t);
}

size_t MockWire::transcript_size() const {
    // Drains fault_ here too, not just in advance_to(): this and transcript() are the
    // conclusion-drawing surfaces a test actually asserts through, so a fault raised by
    // the last transmit() of a test (with no advance_to() afterward) must still fail the
    // test case here rather than passing silently (see fault_'s declaration).
    INFO((fault_ != nullptr ? fault_ : ""));
    REQUIRE(fault_ == nullptr);
    return transcript_count_;
}

const MockWire::TxRecord& MockWire::transcript(size_t i) const {
    INFO((fault_ != nullptr ? fault_ : ""));
    REQUIRE(fault_ == nullptr);
    REQUIRE(i < transcript_count_);
    return transcript_[i];
}

} // namespace omgp_test
