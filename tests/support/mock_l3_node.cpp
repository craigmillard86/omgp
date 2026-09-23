// Test-support: MockL3Node (spec 003 T012, #683) — the scripted message-level L3 responder
// tests/unit/test_mock_l3_node.cpp (T013, #684) pins and every later test_core_*.cpp is driven
// against. contracts/mock-l3-node.md; research.md R-08 (why it exists and why it is not named
// MockTransport), R-02 (driven through a real Master), R-09 (READ_DESC chunk size), R-11
// (explicit simulated time).
//
// Every answer is REAL wire bytes: the L3 payload codecs (l3/l3_payload.hpp), the L3 header
// codec (l3/l3_header.hpp) and omgp::link::encode_frame, in that order. Nothing here stuffs a
// byte, computes a CRC or lays out a frame by hand — the contract's "never a shortcut that
// skips framing", and the same rule mock_wire.cpp follows.
//
// The two readings the contract does not state — what an opcode class with NO step of its kind
// ever scripted answers, and how the two class-less kinds (SilenceStep/ErrorStep) are targeted
// — are implemented per docs/OPEN-QUESTIONS.md 2026-09-23 "MockL3Node: an opcode class with no
// step ever scripted, and how SilenceStep/ErrorStep are targeted" and the same-day entry that
// supersedes its (a) half (both ruling PENDING): the two class-less kinds are wildcards consumed
// in script order by the first request of any class that reaches them, a class with no step of
// its own kind falls back to the wildcard the node has already taken, and only a class with
// neither answers ERROR/ERR_UNKNOWN_OPCODE.
#include "mock_l3_node.hpp"

#include "catch_amalgamated.hpp"
#include "l3/l3_header.hpp"
#include "l3/l3_payload.hpp"

namespace omgp_test {

namespace {

using omgp::l3::Status;

// The answer payload buffer's capacity everywhere below: protocol-l3 §3's own bound on
// payload_len once the five-byte header is subtracted from a frame's budget.
constexpr size_t kPayloadCap = omgp::LIMIT_max_l3_payload;

// SilenceStep and ErrorStep name no opcode, so they answer ANY class (see the file header).
bool is_wildcard(L3Step::Kind k) {
    return k == L3Step::Kind::Silence || k == L3Step::Kind::Error;
}

// The class a scripted step belongs to. Wildcards are matched by is_wildcard(), never by this;
// they map to Other only so the function is total.
OpClass class_of_kind(L3Step::Kind k) {
    switch (k) {
    case L3Step::Kind::Identify:
        return OpClass::Identify;
    case L3Step::Kind::DescChunk:
        return OpClass::Desc;
    case L3Step::Kind::Status:
        return OpClass::Status;
    case L3Step::Kind::Event:
        return OpClass::Event;
    case L3Step::Kind::Channel:
        return OpClass::Channel;
    case L3Step::Kind::Param:
        return OpClass::Param;
    case L3Step::Kind::SlotMap:
        return OpClass::SlotMap;
    case L3Step::Kind::Silence:
    case L3Step::Kind::Error:
        break;
    }
    return OpClass::Other;
}

size_t index_of(OpClass c) {
    return static_cast<size_t>(c);
}

} // namespace

// --- L3Step factories ------------------------------------------------------------------------

L3Step L3Step::of(const IdentifyStep& s) {
    L3Step st{};
    st.kind = Kind::Identify;
    st.identify = s;
    return st;
}
L3Step L3Step::of(const DescChunkStep& s) {
    L3Step st{};
    st.kind = Kind::DescChunk;
    st.desc_chunk = s;
    return st;
}
L3Step L3Step::of(const StatusStep& s) {
    L3Step st{};
    st.kind = Kind::Status;
    st.status = s;
    return st;
}
L3Step L3Step::of(const EventStep& s) {
    L3Step st{};
    st.kind = Kind::Event;
    st.event = s;
    return st;
}
L3Step L3Step::of(const ChannelStep& s) {
    L3Step st{};
    st.kind = Kind::Channel;
    st.channel = s;
    return st;
}
L3Step L3Step::of(const ParamStep& s) {
    L3Step st{};
    st.kind = Kind::Param;
    st.param = s;
    return st;
}
L3Step L3Step::of(const SlotMapStep& s) {
    L3Step st{};
    st.kind = Kind::SlotMap;
    st.slot_map = s;
    return st;
}
L3Step L3Step::of(const SilenceStep& s) {
    L3Step st{};
    st.kind = Kind::Silence;
    st.silence = s;
    return st;
}
L3Step L3Step::of(const ErrorStep& s) {
    L3Step st{};
    st.kind = Kind::Error;
    st.error = s;
    return st;
}

// protocol-l3 §3.1. PING/SET_BYPASS/BP_POWER/BP_ROUTE (and anything outside the v1 set) have no
// step kind of their own, deliberately: this double exists for the operations CoreEngine drives
// (contracts/mock-l3-node.md "What this double is for"), and widening it for later stories is
// out of scope. They are reachable by a wildcard step, and otherwise take the unset-class
// default.
OpClass op_class(uint8_t opcode) {
    switch (opcode) {
    case omgp::OP_IDENTIFY:
        return OpClass::Identify;
    case omgp::OP_READ_DESC:
        return OpClass::Desc;
    case omgp::OP_GET_STATUS:
        return OpClass::Status;
    case omgp::OP_GET_EVENT:
        return OpClass::Event;
    case omgp::OP_SELECT_CHANNEL:
        return OpClass::Channel;
    case omgp::OP_GET_PARAM:
    case omgp::OP_SET_PARAM:
        return OpClass::Param;
    case omgp::OP_BP_SLOT_MAP:
        return OpClass::SlotMap;
    default:
        return OpClass::Other;
    }
}

// --- lifecycle -------------------------------------------------------------------------------

MockL3Node::MockL3Node(FakeClock& clock) : clock_(clock) {
    for (NodeState& ns : nodes_)
        for (int& l : ns.last)
            l = -1; // "no step of this class consumed yet", so there is no steady state yet
}

MockL3Node::~MockL3Node() {
    // Non-throwing (CHECK, not REQUIRE), as ~MockWire() is: a destructor must not throw while
    // unwinding, and this is the last chance to surface a fault recorded after the case's final
    // advance_to().
    INFO((fault_ != nullptr ? fault_ : ""));
    CHECK(fault_ == nullptr);
}

void MockL3Node::set_script(uint8_t node, const L3Step* steps, size_t count) {
    // Runs on the test's own call stack (rig setup, never inside transmit()), so these are
    // REQUIREs rather than deferred faults — the same treatment MockWire::set_script() gives
    // its own preconditions.
    REQUIRE(node < omgp::link::kAddrCount);
    REQUIRE(count <= kMaxScript);
    NodeState& ns = nodes_[node];
    ns = NodeState{}; // every cursor, consumed flag and queued event back to its initial state
    for (int& l : ns.last)
        l = -1;
    ns.steps = steps;
    ns.count = count;

    for (size_t i = 0; i < count; ++i) {
        const L3Step& s = steps[i];
        if (s.kind == L3Step::Kind::DescChunk)
            REQUIRE((s.desc_chunk.blob != nullptr || s.desc_chunk.blob_len == 0));
        if (s.kind != L3Step::Kind::Event)
            continue;
        // contracts/mock-l3-node.md's EventStep is "one queued event": an EventStep seeds the
        // node's event QUEUE when the script is installed rather than being consumed by a
        // GET_EVENT cursor, which is what makes "NONE once exhausted" (§3.4's own answer for an
        // empty queue) the steady state instead of the last step repeating. Marked consumed so
        // no other class's wildcard scan can take it.
        const omgp::l3::GetEventResp& r = s.event.resp;
        REQUIRE(static_cast<size_t>(r.detail.len) <= kMaxEventDetail);
        QueuedEvent e{};
        e.event_type = r.event_type;
        e.remaining_count = r.remaining_count;
        e.detail_len = r.detail.len;
        for (uint8_t j = 0; j < r.detail.len; ++j)
            e.detail[j] = r.detail.data[j];
        e.due_us = 0; // already drainable
        queue_event(ns, e);
        ns.consumed[i] = true;
    }
}

// --- script consumption ----------------------------------------------------------------------

const L3Step* MockL3Node::take_step(NodeState& ns, OpClass cls) {
    const size_t c = index_of(cls);
    for (size_t i = ns.cursor[c]; i < ns.count; ++i) {
        if (ns.consumed[i])
            continue;
        const L3Step& s = ns.steps[i];
        if (!is_wildcard(s.kind) && class_of_kind(s.kind) != cls)
            continue; // another class's step: that class has its own cursor and will take it
        ns.consumed[i] = true;
        ns.cursor[c] = i + 1;
        ns.last[c] = static_cast<int>(i);
        if (is_wildcard(s.kind))
            // A wildcard names no opcode, so consuming it says something about the whole NODE,
            // not only about the class that happened to reach it first: it also becomes the
            // fallback for every class with no step of its own kind (see answer()).
            ns.last_wildcard = static_cast<int>(i);
        return &ns.steps[i];
    }
    // Exhausted. contracts/mock-l3-node.md: "an exhausted script answers the next request with
    // whatever the last step of its kind specified (steady-state, not silence-by-default)" —
    // applied PER CLASS, so one class going quiet or erroring never hijacks another's answers.
    ns.cursor[c] = ns.count;
    return ns.last[c] >= 0 ? &ns.steps[static_cast<size_t>(ns.last[c])] : nullptr;
}

// --- payload builders ------------------------------------------------------------------------

Status MockL3Node::build_error(uint8_t code, uint8_t* out, size_t& n) {
    omgp::l3::ErrorResp r{};
    r.code = code;
    r.detail.data = nullptr;
    r.detail.len = 0;
    return omgp::l3::encode_error_resp(r, out, kPayloadCap, n);
}

Status MockL3Node::build_desc_chunk(const DescChunkStep& s, const omgp::l3::Bytes& req,
                                    uint8_t* out, size_t& n, bool& bad_request) {
    omgp::l3::ReadDescReq rq{};
    if (omgp::l3::decode_read_desc_req(req.data, req.len, rq) != Status::Ok) {
        bad_request = true;
        return Status::Ok;
    }
    // The request's OWN max_len, capped at what the response layout can carry: `u16 offset, u8
    // len, u8[len]` inside LIMIT_max_l3_payload (protocol-l3 §3.1, research.md R-09 — the symbol,
    // never the literal, which has already moved once).
    const size_t layout_cap = kPayloadCap - 3;
    const size_t asked = static_cast<size_t>(rq.max_len);
    const size_t want = asked < layout_cap ? asked : layout_cap;
    const size_t avail = rq.offset >= s.blob_len ? 0u : static_cast<size_t>(s.blob_len - rq.offset);
    const size_t take = want < avail ? want : avail;
    omgp::l3::ReadDescResp r{};
    r.offset = rq.offset;
    // `blob` itself, not blob + offset, when nothing is taken: an offset at or past the end
    // answers a ZERO-LENGTH chunk (the safe default recorded in docs/OPEN-QUESTIONS.md
    // 2026-09-23, ruling pending — never an invented error response), and forming a pointer
    // past the end of the array to say so would be undefined behaviour for nothing.
    r.bytes.data = take > 0 ? s.blob + rq.offset : s.blob;
    r.bytes.len = static_cast<uint8_t>(take);
    return omgp::l3::encode_read_desc_resp(r, out, kPayloadCap, n);
}

Status MockL3Node::build_param(const ParamStep& s, const omgp::l3::Header& req,
                               const omgp::l3::Bytes& payload, uint8_t* out, size_t& n,
                               bool& is_error, bool& bad_request) {
    if (req.opcode == omgp::OP_GET_PARAM) {
        omgp::l3::GetParamReq rq{};
        if (omgp::l3::decode_get_param_req(payload.data, payload.len, rq) != Status::Ok) {
            bad_request = true;
            return Status::Ok;
        }
        if (!s.ok) {
            is_error = true;
            return build_error(s.error_code, out, n);
        }
        omgp::l3::GetParamResp r{};
        r.param_id = rq.param_id; // echoed from the request: the step names only the value
        r.scope = rq.scope;
        r.value = s.value;
        return omgp::l3::encode_get_param_resp(r, out, kPayloadCap, n);
    }
    // SET_PARAM (protocol-l3 §3.1: "Responses to SELECT_CHANNEL, SET_BYPASS and SET_PARAM carry
    // an empty payload (acceptance); failures arrive as ERROR").
    omgp::l3::SetParamReq rq{};
    if (omgp::l3::decode_set_param(payload.data, payload.len, rq) != Status::Ok) {
        bad_request = true;
        return Status::Ok;
    }
    if (!s.ok) {
        is_error = true;
        return build_error(s.error_code, out, n);
    }
    n = 0;
    return Status::Ok;
}

Status MockL3Node::build_slot_map(const SlotMapStep& s, uint8_t* out, size_t& n) {
    if (static_cast<uint32_t>(s.slot_count) > omgp::LIMIT_bp_slot_map_max_slots)
        return Status::OutOfRange; // a script over the wire cap: a named fault, not a truncation
    uint8_t occupied[kBitmapMax] = {};
    uint8_t changed[kBitmapMax] = {};
    // protocol-l3 §3.1: bit (i % 8) of byte i/8, LSB first, is slot i. Bounded by slot_count, so
    // bits at index >= slot_count are left clear (the "SHOULD leave them clear" side of the
    // forward-compatibility rule) — and a uint32 step field describes at most the first 32
    // slots, the rest of a larger backplane's bitmap staying clear.
    for (uint8_t i = 0; i < s.slot_count && i < 32; ++i) {
        const uint8_t bit = static_cast<uint8_t>(1u << (i % 8));
        if (((s.occupied_bits >> i) & 1u) != 0u)
            occupied[i / 8] = static_cast<uint8_t>(occupied[i / 8] | bit);
        if (((s.changed_bits >> i) & 1u) != 0u)
            changed[i / 8] = static_cast<uint8_t>(changed[i / 8] | bit);
    }
    const uint8_t len = static_cast<uint8_t>((static_cast<size_t>(s.slot_count) + 7) / 8);
    omgp::l3::BpSlotMapResp r{};
    r.slot_count = s.slot_count;
    r.occupied.data = occupied;
    r.occupied.len = len;
    r.changed.data = changed;
    r.changed.len = len;
    // #678/T007's own codec, not a hand-assembled payload.
    return omgp::l3::encode_bp_slot_map_resp(r, out, kPayloadCap, n);
}

Status MockL3Node::build_channel(NodeState& ns, const ChannelStep& s, const omgp::l3::Header& req,
                                 const omgp::l3::Bytes& payload, uint64_t tx_end,
                                 bool& bad_request) {
    omgp::l3::SelectChannelReq rq{};
    if (omgp::l3::decode_select_channel(payload.data, payload.len, rq) != Status::Ok) {
        bad_request = true;
        return Status::Ok;
    }
    if (s.settles) {
        QueuedEvent e{};
        e.event_type = omgp::EVT_CHANNEL_SETTLED;
        // The double never recomputes remaining_count: for an EventStep it is script data, and
        // for the one event the double synthesises itself there is nothing else it knows of.
        e.remaining_count = 0;
        // protocol-l3 §3.4: CHANNEL_SETTLED's detail is `u8 channel, u8 seq` — the channel that
        // finished settling and the SELECT_CHANNEL request's own seq.
        e.detail[0] = rq.channel;
        e.detail[1] = req.seq;
        e.detail_len = 2;
        // Measured from the instant the request finished arriving, which is also the instant the
        // acceptance is composed from — a module starts its mute/switch/settle sequence when it
        // has the request, not when its answer lands.
        e.due_us = tx_end + s.settle_delay_us;
        queue_event(ns, e);
    }
    // §3.2: answered immediately with `accepted` — an empty payload. Completion is the event
    // above, never this response.
    return Status::Ok;
}

Status MockL3Node::build_event(NodeState& ns, uint64_t at_us, uint8_t* out, size_t& n) {
    omgp::l3::GetEventResp r{};
    for (size_t i = 0; i < ns.event_count; ++i) {
        if (ns.events[i].due_us > at_us)
            continue; // not settled yet; a later already-due event is still drainable
        const QueuedEvent e = ns.events[i];
        for (size_t j = i + 1; j < ns.event_count; ++j)
            ns.events[j - 1] = ns.events[j];
        --ns.event_count;
        r.event_type = e.event_type;
        r.remaining_count = e.remaining_count;
        r.detail.data = e.detail;
        r.detail.len = e.detail_len;
        return omgp::l3::encode_get_event_resp(r, out, kPayloadCap, n);
    }
    // protocol-l3 §3.4: NONE (0x00) is the answer to a GET_EVENT on an empty queue — the
    // steady state of a drained node, and the reason GET_EVENT is exempt from the unset-class
    // ERR_UNKNOWN_OPCODE default (docs/OPEN-QUESTIONS.md 2026-09-23): it is the one opcode the
    // protocol gives a "nothing to report" answer of its own.
    r.event_type = omgp::EVT_NONE;
    r.remaining_count = 0;
    r.detail.data = nullptr;
    r.detail.len = 0;
    return omgp::l3::encode_get_event_resp(r, out, kPayloadCap, n);
}

void MockL3Node::queue_event(NodeState& ns, const QueuedEvent& e) {
    if (ns.event_count >= kEventQueueCapacity) {
        record_fault("MockL3Node: event queue capacity exceeded (kEventQueueCapacity)");
        return;
    }
    ns.events[ns.event_count++] = e;
}

// --- answering -------------------------------------------------------------------------------

void MockL3Node::answer(uint8_t node, const omgp::l3::Header& req, const omgp::l3::Bytes& payload,
                        const omgp::link::FrameFields& frame, uint64_t tx_end) {
    NodeState& ns = nodes_[node];
    const OpClass cls = op_class(req.opcode);
    const L3Step* step = take_step(ns, cls);
    if (step == nullptr && ns.last_wildcard >= 0)
        // No step of this class's own kind was ever scripted, but the node HAS taken a wildcard.
        // contracts/mock-l3-node.md gives SilenceStep as "node/backplane does not answer at all"
        // and ErrorStep as "an explicit ERROR response" — neither is scoped to an opcode, so a
        // node scripted {SilenceStep} is mute to every class, not to whichever class reached the
        // step first while the other seven answered ERR_UNKNOWN_OPCODE. A class WITH steps of its
        // own kind never gets here, so this is a fallback for the unscripted, never an override
        // of the scripted (docs/OPEN-QUESTIONS.md 2026-09-23, the superseding entry).
        step = &ns.steps[static_cast<size_t>(ns.last_wildcard)];

    uint8_t out[kPayloadCap] = {};
    size_t n = 0;
    bool is_error = false;
    bool bad_request = false;
    Status st = Status::Ok;

    if (step != nullptr && step->kind == L3Step::Kind::Silence) {
        // No bytes at all, so the driving Master reaches its own T_resp deadline from its own
        // clock arithmetic (trunk §3) and retries per §7 — a real timeout, not a synthesised one.
        return;
    }
    if (step != nullptr && step->kind == L3Step::Kind::Error) {
        is_error = true;
        st = build_error(step->error.code, out, n);
    } else if (step == nullptr && cls != OpClass::Event) {
        // The unset class (docs/OPEN-QUESTIONS.md 2026-09-23, ruling pending): an opcode class
        // this script never mentions is answered ERR_UNKNOWN_OPCODE, because silence is reserved
        // for SilenceStep — a second silent path would make an unscripted class
        // indistinguishable from a deliberately mute node.
        is_error = true;
        st = build_error(omgp::ERR_UNKNOWN_OPCODE, out, n);
    } else if (cls == OpClass::Event) {
        // step is null here: EventSteps are queued by set_script(), so the Event class's cursor
        // only ever yields a wildcard — and both wildcards were handled above.
        st = build_event(ns, tx_end, out, n);
    } else {
        switch (step->kind) {
        case L3Step::Kind::Identify:
            st = omgp::l3::encode_identify_resp(step->identify.resp, out, kPayloadCap, n);
            break;
        case L3Step::Kind::DescChunk:
            st = build_desc_chunk(step->desc_chunk, payload, out, n, bad_request);
            break;
        case L3Step::Kind::Status:
            st = omgp::l3::encode_status_block(step->status.status, out, kPayloadCap, n);
            break;
        case L3Step::Kind::Channel:
            st = build_channel(ns, step->channel, req, payload, tx_end, bad_request);
            break;
        case L3Step::Kind::Param:
            st = build_param(step->param, req, payload, out, n, is_error, bad_request);
            break;
        case L3Step::Kind::SlotMap:
            st = build_slot_map(step->slot_map, out, n);
            break;
        case L3Step::Kind::Event:
        case L3Step::Kind::Silence:
        case L3Step::Kind::Error:
            // Unreachable by construction: EventSteps never leave set_script()'s queue-seeding
            // path, and both wildcards were handled before this switch. Named rather than
            // silently dropped, so a future change that breaks the argument is loud.
            record_fault("MockL3Node: a step kind reached the per-class switch that cannot");
            return;
        }
    }

    if (bad_request) {
        // A request payload the L3 codecs refuse (a malformed READ_DESC/SELECT_CHANNEL/param
        // request): protocol-l3 §3.1's ERR_BAD_PAYLOAD, which is what a conforming node answers.
        // Not a fault: the double is modelling a node, and the malformed request came from the
        // engine under test.
        is_error = true;
        st = build_error(omgp::ERR_BAD_PAYLOAD, out, n);
    }
    if (st != Status::Ok) {
        // A SCRIPT the codecs refuse (a StatusBlock state outside NODE_STATE_CODES, a param
        // value over LIMIT_param_value_max, an ErrorStep code that is not a protocol error code,
        // a slot_count over the wire cap). Named on the deferred-fault path rather than dropped,
        // which would be indistinguishable from a SilenceStep.
        record_fault("MockL3Node: the L3 codecs refused a scripted answer (check the step's own "
                     "field values against protocol-l3 §3.1)");
        return;
    }
    schedule_answer(frame, req, is_error ? omgp::OP_ERROR : req.opcode, is_error, out, n,
                    tx_end + omgp::TRUNK_T_turn_min_us);
}

void MockL3Node::schedule_answer(const omgp::link::FrameFields& frame, const omgp::l3::Header& req,
                                 uint8_t opcode, bool is_error, const uint8_t* payload, size_t len,
                                 uint64_t start_us) {
    omgp::l3::Header h{};
    h.opcode = opcode;
    h.node_id = req.node_id; // §3: "target for requests, source for responses" — echoed back
    h.seq = req.seq;
    h.flags = static_cast<uint8_t>(omgp::FLAG_response | (is_error ? omgp::FLAG_error : 0));
    h.payload_len = static_cast<uint8_t>(len);

    uint8_t msg[omgp::LIMIT_max_l3_message];
    size_t head = 0;
    if (omgp::l3::encode_header(h, msg, sizeof msg, head) != Status::Ok) {
        record_fault("MockL3Node: encode_header refused the answer's own L3 header");
        return;
    }
    for (size_t i = 0; i < len; ++i)
        msg[head + i] = payload[i];

    omgp::link::FrameFields r{};
    r.dst = frame.src; // mirrored, trunk §4
    r.src = frame.dst;
    r.response = true;
    r.retry = false;
    r.seq = frame.seq; // trunk §7: the answer carries the request's own seq
    r.len = static_cast<uint8_t>(head + len);
    r.payload = msg;

    uint8_t buf[omgp::link::kMaxWire];
    size_t written = 0;
    if (omgp::link::encode_frame(r, buf, sizeof buf, written) != omgp::link::Status::Ok) {
        record_fault("MockL3Node: encode_frame refused a scripted answer");
        return;
    }

    // Arm trunk §7's replay buffer with these exact bytes: a retry of this L2 seq re-sends them
    // unchanged. frame.dst was bounds-checked by transmit() before answer() was called.
    NodeState& ns = nodes_[frame.dst];
    ns.replay_valid = true;
    ns.replay_seq = frame.seq;
    ns.replay_len = written;
    for (size_t i = 0; i < written; ++i)
        ns.replay_frame[i] = buf[i];

    enqueue_frame(buf, written, start_us);
}

// --- ByteWire ---------------------------------------------------------------------------------

uint64_t MockL3Node::transmit(const uint8_t* bytes, size_t n, uint64_t now_us) {
    using omgp::link::byte_time_us;
    const uint64_t tx_end = now_us + static_cast<uint64_t>(n) * byte_time_us(bit_rate_);

    omgp::link::FrameView view{};
    // Act on each decoded frame INSIDE the feed loop: view.f.payload points into parser_'s own
    // accumulator and is valid only until the next feed() call (link/frame.hpp), and the L3
    // payload view taken from it has the same lifetime.
    for (size_t i = 0; i < n; ++i) {
        const uint64_t byte_us = now_us + static_cast<uint64_t>(i) * byte_time_us(bit_rate_);
        if (!parser_.feed(bytes[i], view))
            continue;
        const uint64_t frame_tx_end = byte_us + byte_time_us(bit_rate_);

        // A response frame is the host's own traffic (or this double's answer looping back in a
        // later rig): it consumes no step and is never answered — a double that answered its own
        // answers would never stop.
        if (view.f.response)
            continue;

        // trunk §5: only 0x00..0x0F are node addresses. Silence is the faithful wire behaviour
        // (no such node), but a mis-addressed request is a bug the double exists to expose, so
        // it is also named.
        if (view.f.dst >= omgp::link::kAddrCount) {
            record_fault("MockL3Node: request addressed to dst >= kAddrCount (not a node)");
            continue;
        }

        omgp::l3::Header hdr{};
        omgp::l3::Bytes payload{};
        if (omgp::l3::decode_message(view.f.payload, view.f.len, hdr, payload) != Status::Ok) {
            // This double answers at the MESSAGE level, so a frame whose payload is not a
            // decodable L3 message is a rig bug, not a node behaviour to model (byte-level
            // corruption is MockWire's job, and F4's).
            record_fault("MockL3Node: frame payload is not a decodable L3 message");
            continue;
        }
        if ((hdr.flags & omgp::FLAG_response) != 0) {
            record_fault("MockL3Node: request frame carries an L3 response header (flags bit0)");
            continue;
        }

        ++requests_seen_;

        // trunk §7: "A node receiving a retry of a sequence it already answered re-sends its
        // previous response (single-frame replay buffer per node)." Replaying rather than
        // re-running the script is what makes the double obey CLAUDE.md rule 2 — a
        // retransmission at L2 is safe — instead of handing the retry the script's NEXT step and
        // so answering one request two different ways.
        NodeState& ns = nodes_[view.f.dst];
        if (view.f.retry && ns.replay_valid && ns.replay_seq == view.f.seq) {
            const uint64_t replay_at = frame_tx_end + omgp::TRUNK_T_turn_min_us; // trunk §9
            enqueue_frame(ns.replay_frame, ns.replay_len, replay_at);
            continue;
        }
        // Anything else is a new transaction, which supersedes the buffer: a node that has since
        // gone quiet (a SilenceStep's steady state) must not replay an answer from an older
        // transaction that happened to reuse this seq.
        ns.replay_valid = false;
        answer(view.f.dst, hdr, payload, view.f, frame_tx_end);
    }
    return tx_end;
}

bool MockL3Node::receive(uint8_t& byte, uint64_t& start_us) {
    if (rx_count_ == 0)
        return false;
    // rx_queue_ is kept sorted by start_us (enqueue()), so index 0 is always the
    // earliest-start-instant pending byte — byte-wire-and-clock.md's release order.
    const QueuedByte& front = rx_queue_[0];
    if (front.start_us > clock_.now_us())
        return false; // in the future: stays queued
    byte = front.byte;
    start_us = front.start_us;
    for (size_t i = 1; i < rx_count_; ++i)
        rx_queue_[i - 1] = rx_queue_[i];
    --rx_count_;
    return true;
}

uint32_t MockL3Node::bit_rate() const {
    return bit_rate_;
}

void MockL3Node::set_bit_rate(uint32_t bps) {
    bit_rate_ = bps;
}

void MockL3Node::enqueue(uint8_t byte, uint64_t start_us) {
    if (rx_count_ >= kRxCapacity) {
        record_fault("MockL3Node: RX queue capacity exceeded (4 * kMaxWire)");
        return;
    }
    // A plain append: the queue is ascending in start_us by construction (enqueue_frame() lays
    // one answer's bytes out at increasing instants and refuses an answer that would overlap the
    // bytes already queued), which is what lets receive() always take index 0.
    rx_queue_[rx_count_] = QueuedByte{byte, start_us};
    ++rx_count_;
    ++bytes_scheduled_;
}

void MockL3Node::enqueue_frame(const uint8_t* buf, size_t written, uint64_t t0) {
    // Two answers in flight at once would otherwise be merged byte-by-byte in time order into a
    // stream no Deframer can read, with nothing raised. That is not a node behaviour to model:
    // trunk §3 has the host run one transaction at a time, so a second answer opening inside the
    // first one's bytes is a RIG bug, and this double's idiom for a rig bug is a named refusal
    // (as for kRxCapacity, a dst that is not a node, or an undecodable request).
    if (written > 0 && rx_count_ > 0 && rx_queue_[rx_count_ - 1].start_us >= t0) {
        record_fault("MockL3Node: an answer would overlap one still on the wire (two requests in "
                     "flight at once)");
        return;
    }
    for (size_t i = 0; i < written; ++i)
        enqueue(buf[i], t0 + static_cast<uint64_t>(i) * omgp::link::byte_time_us(bit_rate_));
}

void MockL3Node::record_fault(const char* what) {
    if (fault_ == nullptr)
        fault_ = what;
}

// --- test helpers ----------------------------------------------------------------------------

void MockL3Node::advance_to(uint64_t t) {
    // Drains a fault recorded on transmit()'s (engine) call stack; this call always runs on the
    // test's own stack, so REQUIRE-ing here is safe.
    INFO((fault_ != nullptr ? fault_ : ""));
    REQUIRE(fault_ == nullptr);
    // byte-wire-and-clock.md: the clock is monotonic. Moving it backwards would also make
    // already-due RX bytes future again.
    REQUIRE(t >= clock_.now_us());
    clock_.set(t);
}

size_t MockL3Node::requests_seen() const {
    return requests_seen_;
}

size_t MockL3Node::bytes_scheduled() const {
    return bytes_scheduled_;
}

const char* MockL3Node::take_fault() {
    const char* f = fault_;
    fault_ = nullptr;
    return f;
}

} // namespace omgp_test
