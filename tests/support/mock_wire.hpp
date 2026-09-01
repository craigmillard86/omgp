// Test-support: scripted omgp::link::ByteWire ("MockWire") that Master/Responder engine
// tests are driven against, plus the FakeClock they share (spec 002 T010).
// contracts/mock-wire.md, contracts/byte-wire-and-clock.md, research.md R-07.
//
// Host-only (tests/support/) but allocation-free, per contracts/mock-wire.md, so F4's
// virtual wire can seed itself from the same code. Only Kind::Respond and Kind::Silence
// are implemented here; Garbage/CrcError/Duplicate/Babble/Rate are declared so T011/T028
// can reference the enum, with their behaviour landing in T030 (tasks.md) — see the
// switch in mock_wire.cpp.
#pragma once

#include "fake_clock.hpp"
#include "link/byte_wire.hpp"
#include "link/frame.hpp"
#include "link/link_types.hpp"
#include "omgp_protocol.h"

#include <cstddef>
#include <cstdint>

namespace omgp_test {

enum class Kind : uint8_t { Respond, Silence, Garbage, CrcError, Duplicate, Babble, Rate };

// contracts/mock-wire.md "Step table". Scripts are consumed in order via MockWire::
// set_script(); node == 0xFF is the shared fallback script drawn on by any node once its
// own script (if any) is exhausted.
struct Step {
    uint8_t node;
    Kind kind;
    uint32_t delay_us;
    uint16_t count;
    uint32_t seed;
};

// research R-07: all script-driven randomness (Garbage/Babble byte content, T030) goes
// through this xorshift32 so a given Step::seed reproduces the same byte stream.
// `state` must be nonzero (xorshift32's fixed point); seed it from Step::seed.
uint32_t xorshift32_next(uint32_t& state);

// Scripted ByteWire (contracts/mock-wire.md). `clock` is shared with the engine(s) under
// test: transmit()/receive() schedule and release RX bytes against it, and advance_to()
// is the test-side helper that steps it.
class MockWire : public omgp::link::ByteWire {
  public:
    explicit MockWire(FakeClock& clock);

    // Configure the step script consumed, in order, for one node's requests
    // (0x00..0x0F, per omgp::link::kAddrCount) — or, for node == 0xFF, the shared
    // fallback script. `steps` must outlive this MockWire; an exhausted or unset script
    // behaves as Respond with delay_us == TRUNK_T_turn_min_us (contracts/mock-wire.md).
    void set_script(uint8_t node, const Step* steps, size_t count);

    // omgp::link::ByteWire
    uint64_t transmit(const uint8_t* bytes, size_t n, uint64_t now_us) override;
    bool receive(uint8_t& byte, uint64_t& start_us) override;
    uint32_t bit_rate() const override;
    void set_bit_rate(uint32_t bps) override;

    // Test helper (contracts/mock-wire.md "Scheduling"): sets the clock only. For tests
    // that inspect the mock directly, with no engine yet to drive receive() (T011, Phase
    // 2, before Master/Responder exist).
    void advance_to(uint64_t t);

    // Test helper: sets the clock, then lets `engine` drain receive() via poll(t) — the
    // only receive path (data-model.md §4, analysis F1); returns whatever poll()
    // returns, so this works for both Master::poll (MasterEvent) and Responder::poll
    // (void).
    template <typename Engine>
    auto advance_to(uint64_t t, Engine& engine) -> decltype(engine.poll(t)) {
        advance_to(t);
        return engine.poll(t);
    }

    // Transcript (contracts/mock-wire.md "What the tests assert through the mock"):
    // every transmitted frame's fields and tx_start_us, in transmission order.
    struct TxRecord {
        uint8_t dst, src;
        bool response, retry;
        uint8_t seq;
        uint8_t len;
        uint8_t payload[omgp::LIMIT_max_l3_payload];
        uint64_t tx_start_us;
    };
    size_t transcript_size() const;
    const TxRecord& transcript(size_t i) const;

  private:
    struct QueuedByte {
        uint8_t byte;
        uint64_t start_us;
    };

    // 4 x kMaxWire (contracts/mock-wire.md "Capacity"): enough for a response, a
    // duplicate and a babble burst (T030).
    static constexpr size_t kRxCapacity = 4 * omgp::link::kMaxWire;
    // Generous fixed bound for the transcript, sized well above what any single test's
    // transaction sequence needs (a full 3-attempt retry to every one of the 16 nodes is
    // 48 frames); overflow is a REQUIRE failure, matching the RX queue's no-silent-drop
    // rule, rather than an unbounded/allocating container.
    static constexpr size_t kTranscriptCapacity = 128;

    const Step* next_step(uint8_t node);
    void schedule_respond(const omgp::link::FrameFields& request, uint64_t tx_end,
                          uint32_t delay_us);
    void enqueue(uint8_t byte, uint64_t start_us);
    void record_transcript(const omgp::link::FrameFields& f, uint64_t tx_start_us);

    FakeClock& clock_;
    uint32_t bit_rate_ = omgp::TRUNK_bit_rate;

    // Persists across transmit() calls (byte-at-a-time parser state), not a per-call
    // local: a frame split across two transmit() calls must still be recognised. A local
    // Deframer would silently drop it (no transcript entry, no scheduled response, no
    // diagnostic) since its Hunting/InFrame/Escaped state would reset every call.
    omgp::link::Deframer parser_;

    // Set instead of REQUIRE-ing at the point of failure: transmit() runs on the call
    // stack of the engine under test (Master/Responder, T028/T031), which link/CMakeLists.txt
    // builds with -fno-exceptions — a REQUIRE thrown there would unwind through those
    // frames, which is not defined behaviour, losing the diagnostic exactly when it fires.
    // Drained by a REQUIRE in advance_to(), which only ever runs on the test's own call
    // stack (tests/support and test TUs build with exceptions enabled).
    const char* fault_ = nullptr;

    const Step* scripts_[omgp::link::kAddrCount] = {};
    size_t script_len_[omgp::link::kAddrCount] = {};
    size_t script_pos_[omgp::link::kAddrCount] = {};
    const Step* wildcard_script_ = nullptr;
    size_t wildcard_len_ = 0;
    // One cursor per node (not one shared cursor): contracts/mock-wire.md "Steps with
    // node == 0xFF apply to every node" means each node draws its own sequence of
    // fallback effects, not that the rig-wide first taker exhausts it for everyone else.
    size_t wildcard_pos_[omgp::link::kAddrCount] = {};

    // Sorted by start_us (ascending), not a FIFO: byte-wire-and-clock.md requires
    // receive() to release the earliest-start-instant byte first, and interleaved delays
    // (a late Respond queued behind an on-time one from a different node) make insertion
    // order and start-instant order diverge. enqueue() insertion-sorts; receive() always
    // takes rx_queue_[0].
    QueuedByte rx_queue_[kRxCapacity] = {};
    size_t rx_count_ = 0;

    TxRecord transcript_[kTranscriptCapacity] = {};
    size_t transcript_count_ = 0;
};

} // namespace omgp_test
