// Test-support: MockL3Node, this feature's MESSAGE-level scripted test double (spec 003 T012,
// #683) — contracts/mock-l3-node.md, research.md R-08. A scripted in-memory L3 responder that
// answers a REAL omgp::link::Master's transactions with typed L3 responses (IdentifyResp,
// ReadDescResp chunks, StatusBlock, GetEventResp, GetParamResp, BpSlotMapResp, ErrorResp) or
// with silence, over a trivial loopback omgp::link::ByteWire and F2's FakeClock (reused, not
// re-implemented).
//
// NOT F2's MockWire: that one is byte-level, scripts wire faults (Garbage/CrcError/Duplicate/
// Babble/Rate) and stays in link/'s own tests. This one sits at the same place on the stack (a
// ByteWire under a real Master) but scripts MESSAGES, because core/ never sees frame bytes
// either. The distinct name is deliberate (R-08): the roadmap's "MockTransport from F2" does
// not exist, and a later reader must not read this file as an F2 artifact.
//
// Host-only code (tests/support/), so the full language is allowed — but it allocates nothing
// after construction (fixed arrays throughout, no container, no std::function), so a case may
// drive it inside HEAP_FREE_SCOPE, and it never re-implements framing: answers are built with
// omgp::link::encode_frame and requests read with omgp::link::Deframer, exactly as
// mock_wire.cpp does. Time is explicit: advance_to(t) is the only thing that moves the clock
// (CLAUDE.md rule 3, research.md R-11) and every deadline is a TRUNK_T_* symbol (rule 4).
//
// Scope, from the contract's closing paragraph: enough for CoreEngine's own unit tests. No
// fault injection beyond SilenceStep/ErrorStep, no backplane bridging model, no multi-node
// scale test, no scenario YAML — those are F4's.
#pragma once

#include "fake_clock.hpp"
#include "l3/l3_types.hpp"
#include "link/byte_wire.hpp"
#include "link/frame.hpp"
#include "link/link_types.hpp"
#include "omgp_protocol.h"

#include <cstddef>
#include <cstdint>

namespace omgp_test {

// --- Step table (contracts/mock-l3-node.md; names and fields exactly as the contract gives
//     them, so a reader can check this file against it line by line) -------------------------

struct IdentifyStep {
    omgp::l3::IdentifyResp resp;
};
// Answers EVERY READ_DESC against this blob, honouring the request's own offset/max_len.
struct DescChunkStep {
    const uint8_t* blob;
    uint16_t blob_len;
};
struct StatusStep {
    omgp::l3::StatusBlock status;
};
// One queued event; NONE once exhausted. The contract's stated exception to the steady-state
// rule below: a drained queue IS a node's steady state (protocol-l3 §3.4 — GET_EVENT answers
// event_type NONE, 0x00, on an empty queue).
struct EventStep {
    omgp::l3::GetEventResp resp;
};
// Accepts immediately (protocol-l3 §3.2); CHANNEL_SETTLED becomes drainable settle_delay_us
// later, or never if !settles.
struct ChannelStep {
    bool settles;
    uint64_t settle_delay_us;
};
struct ParamStep {
    bool ok;
    uint16_t value;
    uint8_t error_code;
};
struct SlotMapStep {
    uint8_t slot_count;
    uint32_t occupied_bits;
    uint32_t changed_bits;
};
// The node/backplane does not answer at all (the driving Master's own T_resp timeout).
struct SilenceStep {};
// An explicit ERROR response (protocol-l3 §3.1: opcode 0x7F, flags.error set).
struct ErrorStep {
    uint8_t code;
};

// One script entry: a tagged union of the nine step structs. A union rather than std::variant
// so the double stays allocation-free and trivially copyable (see the file header), and a
// static factory per kind rather than designated initialisers so a script reads as the
// contract's own step names: L3Step::of(StatusStep{blk}).
struct L3Step {
    enum class Kind : uint8_t {
        Identify,
        DescChunk,
        Status,
        Event,
        Channel,
        Param,
        SlotMap,
        Silence,
        Error
    };

    Kind kind = Kind::Silence;
    union {
        IdentifyStep identify;
        DescChunkStep desc_chunk;
        StatusStep status;
        EventStep event;
        ChannelStep channel;
        ParamStep param;
        SlotMapStep slot_map;
        SilenceStep silence;
        ErrorStep error;
    };

    static L3Step of(const IdentifyStep& s);
    static L3Step of(const DescChunkStep& s);
    static L3Step of(const StatusStep& s);
    static L3Step of(const EventStep& s);
    static L3Step of(const ChannelStep& s);
    static L3Step of(const ParamStep& s);
    static L3Step of(const SlotMapStep& s);
    static L3Step of(const SilenceStep& s);
    static L3Step of(const ErrorStep& s);
};

// Which of the double's step kinds answers a given opcode — one cursor per class, per
// contracts/mock-l3-node.md's "steps consumed in order per opcode class". `Other` is every
// opcode no step kind can answer on its own (PING, SET_BYPASS, BP_POWER, BP_ROUTE, and
// anything not in the v1 set): only a wildcard SilenceStep/ErrorStep, or the unset-class
// default, reaches it.
enum class OpClass : uint8_t {
    Identify,
    Desc,
    Status,
    Event,
    Channel,
    Param,
    SlotMap,
    Other,
    COUNT
};

// The opcode class a request belongs to (protocol-l3 §3.1). Pure; exposed so a test can state
// the mapping it relies on rather than inferring it from behaviour.
OpClass op_class(uint8_t opcode);

// Scripted L3 responder over a loopback ByteWire. `clock` is shared with the engine(s) under
// test: transmit()/receive() schedule and release RX bytes against it, advance_to() steps it.
class MockL3Node : public omgp::link::ByteWire {
  public:
    explicit MockL3Node(FakeClock& clock);
    // Drains any fault_ raised since the last check (see fault_'s declaration): a fault
    // recorded by the final transmit()/receive() of a case, with no advance_to() after it,
    // would otherwise never fail that case.
    ~MockL3Node();

    // Longest script one trunk address may carry. A fixed bound (no allocation): a longer
    // script is refused by name in set_script() rather than silently truncated.
    static constexpr size_t kMaxScript = 32;
    // Events queued per node: the script's own EventSteps plus any CHANNEL_SETTLED a
    // ChannelStep armed.
    static constexpr size_t kEventQueueCapacity = 8;
    // The longest event detail the queue carries — protocol-l3 §3.4's own bound (u8
    // event_type, u8 remaining_count, then the tail).
    static constexpr size_t kMaxEventDetail = omgp::LIMIT_max_l3_payload - 2;

    // Installs the step script consumed, in order per opcode class, for requests addressed to
    // trunk address `node` (0x00..0x0F, omgp::link::kAddrCount — a backplane, or one of its
    // slots via its own bridging). `steps` must outlive this MockL3Node. Re-installing resets
    // every cursor and the node's event queue.
    //
    // Consumption rules (contracts/mock-l3-node.md, plus the two readings recorded in
    // docs/OPEN-QUESTIONS.md 2026-09-23 "MockL3Node: an opcode class with no step ever
    // scripted, and how SilenceStep/ErrorStep are targeted" — ruling pending):
    //   * each opcode class has its OWN cursor, so a GET_STATUS never consumes an IdentifyStep;
    //   * SilenceStep and ErrorStep are wildcards, taken by the first request of ANY class that
    //     reaches them in script order, and consumed exclusively (one step, one request);
    //   * an EXHAUSTED class answers whatever its own last consumed step specified (steady
    //     state, not silence-by-default);
    //   * GET_EVENT is served by the node's event queue, not by a cursor: EventSteps are queued
    //     when the script is installed, and an empty queue answers NONE (§3.4);
    //   * a class with NO step of its kind ever scripted answers ERROR/ERR_UNKNOWN_OPCODE —
    //     silence stays reserved for SilenceStep.
    void set_script(uint8_t node, const L3Step* steps, size_t count);

    // omgp::link::ByteWire
    uint64_t transmit(const uint8_t* bytes, size_t n, uint64_t now_us) override;
    bool receive(uint8_t& byte, uint64_t& start_us) override;
    uint32_t bit_rate() const override;
    void set_bit_rate(uint32_t bps) override;

    // Test helper (contracts/mock-l3-node.md "Scheduling"): sets the clock only, for cases
    // that read the double's own wire bytes with no engine driving receive().
    void advance_to(uint64_t t);

    // Test helper: sets the clock, then lets `engine` drain receive() via poll(t) — the only
    // receive path; returns whatever poll() returns, so this serves Master::poll (MasterEvent)
    // and any later void-returning engine alike. Mirrors MockWire's own helper.
    template <typename Engine>
    auto advance_to(uint64_t t, Engine& engine) -> decltype(engine.poll(t)) {
        advance_to(t);
        return engine.poll(t);
    }

    // Test helper: how many L3 requests this double has processed (answered, errored or
    // deliberately left unanswered). A SilenceStep case asserts through this that the double
    // SAW the requests it did not answer, rather than never having been addressed.
    size_t requests_seen() const;
    // Test helper: total bytes this double has ever put on the wire. 0 after a SilenceStep
    // transaction is what distinguishes "no answer" from "a late or malformed answer".
    size_t bytes_scheduled() const;

    // Test helper (MockWire's own idiom): the fault recorded since the last call, or nullptr,
    // and clears it — for a case that deliberately drives the double into a named refusal.
    const char* take_fault();

  private:
    // 4 x kMaxWire, as MockWire: enough for an answer plus slack, and bounded so an overrun is
    // a named fault rather than an allocation.
    static constexpr size_t kRxCapacity = 4 * omgp::link::kMaxWire;
    static constexpr size_t kBitmapMax = (omgp::LIMIT_bp_slot_map_max_slots + 7) / 8;

    struct QueuedByte {
        uint8_t byte;
        uint64_t start_us;
    };

    // One drainable event (protocol-l3 §3.4). The detail bytes are COPIED in, unlike the rest
    // of a script (which the caller keeps alive): a CHANNEL_SETTLED event the double
    // synthesises itself has no script memory to point at, and one storage rule for both
    // sources is simpler than two.
    struct QueuedEvent {
        uint8_t event_type;
        uint8_t remaining_count;
        uint8_t detail[kMaxEventDetail];
        uint8_t detail_len;
        // The instant from which this event is drainable: 0 for a scripted EventStep (already
        // queued), request-end + settle_delay_us for a ChannelStep's CHANNEL_SETTLED.
        uint64_t due_us;
    };

    struct NodeState {
        const L3Step* steps = nullptr;
        size_t count = 0;
        bool consumed[kMaxScript] = {};
        size_t cursor[static_cast<size_t>(OpClass::COUNT)] = {};
        // Index of the last step each class consumed, or -1 — the steady state an exhausted
        // class answers with.
        int last[static_cast<size_t>(OpClass::COUNT)] = {};
        QueuedEvent events[kEventQueueCapacity] = {};
        size_t event_count = 0;
    };

    // The next step for `cls`: the first unconsumed step in script order that is either of that
    // class's own kind or a wildcard (SilenceStep/ErrorStep); else the class's last consumed
    // step (steady state); else nullptr (never scripted).
    const L3Step* take_step(NodeState& ns, OpClass cls);

    // Composes and schedules the answer to one decoded request, or schedules nothing at all
    // (SilenceStep). `tx_end` is the instant the request's last stop bit left the wire; the
    // answer starts one T_turn after it (trunk §9).
    void answer(uint8_t node, const omgp::l3::Header& req, const omgp::l3::Bytes& payload,
                const omgp::link::FrameFields& frame, uint64_t tx_end);

    // Payload builders, each returning an l3::Status so a script the codecs refuse (an
    // out-of-range param value, a slot_count over the wire cap, an ErrorStep code that is not
    // a protocol error code) becomes a named fault instead of a silently wrong frame.
    omgp::l3::Status build_desc_chunk(const DescChunkStep& s, const omgp::l3::Bytes& req,
                                      uint8_t* out, size_t& n, bool& bad_request);
    omgp::l3::Status build_param(const ParamStep& s, const omgp::l3::Header& req,
                                 const omgp::l3::Bytes& payload, uint8_t* out, size_t& n,
                                 bool& is_error, bool& bad_request);
    omgp::l3::Status build_slot_map(const SlotMapStep& s, uint8_t* out, size_t& n);
    // Accepts the channel switch (empty payload, §3.2) and, when the step settles, queues the
    // CHANNEL_SETTLED event due settle_delay_us after `tx_end`.
    omgp::l3::Status build_channel(NodeState& ns, const ChannelStep& s, const omgp::l3::Header& req,
                                   const omgp::l3::Bytes& payload, uint64_t tx_end,
                                   bool& bad_request);
    // The next event due at `at_us`, or NONE (§3.4). Pops what it delivers.
    omgp::l3::Status build_event(NodeState& ns, uint64_t at_us, uint8_t* out, size_t& n);
    omgp::l3::Status build_error(uint8_t code, uint8_t* out, size_t& n);

    void queue_event(NodeState& ns, const QueuedEvent& e);
    // Wraps `payload` in an L3 response header (§3) and that in a trunk frame (§4) mirroring
    // `frame`, and schedules its bytes from `start_us`. Real codecs only.
    void schedule_answer(const omgp::link::FrameFields& frame, const omgp::l3::Header& req,
                         uint8_t opcode, bool is_error, const uint8_t* payload, size_t len,
                         uint64_t start_us);
    void enqueue(uint8_t byte, uint64_t start_us);
    void enqueue_frame(const uint8_t* buf, size_t written, uint64_t t0);
    void record_fault(const char* what);

    FakeClock& clock_;
    uint32_t bit_rate_ = omgp::TRUNK_bit_rate;

    // Persists across transmit() calls (byte-at-a-time parser state), same reason as MockWire's:
    // a frame split across two calls must still be recognised.
    omgp::link::Deframer parser_;

    // Set instead of REQUIRE-ing at the point of failure: transmit() runs on the call stack of
    // the engine under test, which link/CMakeLists.txt builds -fno-exceptions — a Catch2
    // exception thrown there would unwind through those frames. Drained by advance_to() and the
    // destructor, which only ever run on the test's own stack.
    const char* fault_ = nullptr;

    NodeState nodes_[omgp::link::kAddrCount];

    // Sorted by start_us ascending (not a FIFO): byte-wire-and-clock.md requires receive() to
    // release the earliest-start-instant byte first.
    QueuedByte rx_queue_[kRxCapacity] = {};
    size_t rx_count_ = 0;

    size_t requests_seen_ = 0;
    size_t bytes_scheduled_ = 0;
};

} // namespace omgp_test
