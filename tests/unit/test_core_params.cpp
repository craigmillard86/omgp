// CoreEngine::set_param()/get_param() — the ENQUEUE side of spec FR-023/FR-024 (tasks.md T026,
// made to pass by T027). Driven against a real CoreEngine over the real link::Master and
// link::HealthTracker it owns, answered by MockL3Node (R-08) on a FakeClock: no hand-built
// frames, no wall clock (CLAUDE.md rule 3).
// Spec: specs/003-host-core-engine/spec.md FR-023 (a failed parameter operation is REPORTED,
// never dropped), FR-024 (GET_PARAM returns immediately with a correlation tag), FR-021 (no
// callback from run_superframe()); data-model.md §6 (ParamOpItem), §8 (ParamRequestId/
// ParamResult), §8a (the pending-delivery rings); research.md R-07 (three fixed rings), R-10
// (the opaque, reusable request id); contracts/core-cpp.md §Engine (set_param/get_param).
//
// WHAT THIS FILE IS AN ORACLE FOR, AND WHAT IT IS NOT (CLAUDE.md rule 11). Every case below is
// about what the two API calls ACCEPT, REFUSE and QUEUE, and about the request-id pool. None of
// it is about issuing a queued operation onto the wire or decoding its answer: at this head
// nothing drains param_queue_ — core_engine.cpp's issue_demand() says so in as many words
// ("the parameter queue (US2) [is] drained by T029's loop") — so SET_PARAM/GET_PARAM frames,
// LifecycleKind::ParamSetFailed and on_param_result delivery are T029's and T030's to
// demonstrate, and T026's remaining cases land with them. What IS asserted here about the wire
// is the negative FR-024 needs: that accepting an operation writes nothing.
#include "core/core_engine.hpp"

#include "catch_amalgamated.hpp"
#include "fake_clock.hpp"
#include "heap_guard.hpp"
#include "l3/l3_descriptor.hpp"
#include "l3/l3_payload.hpp"
#include "l3/l3_types.hpp"
#include "link/link_types.hpp"
#include "mock_l3_node.hpp"
#include "omgp_protocol.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

using omgp::core::CoreCallbacks;
using omgp::core::CoreEngine;
using omgp::core::CoreStatus;
using omgp::core::DiscoveryState;
using omgp::core::LifecycleEvent;
using omgp::core::ParamOpItem;
using omgp::core::ParamRequestId;
using omgp::core::ParamResult;
using omgp_test::DescChunkStep;
using omgp_test::FakeClock;
using omgp_test::IdentifyStep;
using omgp_test::L3Step;
using omgp_test::MockL3Node;
using omgp_test::SilenceStep;
using omgp_test::SlotMapStep;
using omgp_test::StatusStep;

// The private members these cases read. CoreEngineTestSeam is forward-declared and friended by
// core_engine.hpp precisely so a test can name them without a public setter existing; each test
// binary defines the accessors it needs (core_engine.hpp:82).
namespace omgp::core {
struct CoreEngineTestSeam {
    // The parameter ring itself: its depth is what "refuses, never drops" is asserted against,
    // and popping from it is how a case reaches the state the drain loop (T029) will create —
    // an operation no longer queued whose request id is still outstanding.
    static bool pop_param(CoreEngine& engine, ParamOpItem& out) {
        return engine.param_queue_.pop(out);
    }
    static size_t param_queued(CoreEngine& engine) {
        return engine.param_queue_.size();
    }
    static NodeRecord& node(CoreEngine& engine, uint8_t node_id) {
        return engine.nodes_[CoreEngine::node_index(node_id)];
    }
    // The producer T030 will be: a decoded answer enqueued against its request id. Used here to
    // reach delivery — and so the release of the id — without the issue path T029 owns.
    static bool enqueue_param_result(CoreEngine& engine, ParamRequestId id,
                                     const ParamResult& result) {
        return engine.enqueue_param_result(id, result);
    }
};
} // namespace omgp::core

using Bk = omgp::core::CoreEngineTestSeam;

namespace {

// data-model.md §6/§8a: the parameter ring and the request-id space are both LIMIT_max_nodes
// deep (core_types.hpp's static_assert ties the id type to that capacity). By its symbol, never
// the literal (CLAUDE.md rule 4).
constexpr size_t kParamCapacity = omgp::LIMIT_max_nodes;

uint64_t byte_us() {
    return omgp::link::byte_time_us(omgp::TRUNK_bit_rate);
}

// --- what the application sees -----------------------------------------------------------------

// Records every delivery, so a case can assert that NOTHING arrived during a superframe and
// then that exactly what was expected arrives on the following drain_callbacks() (spec FR-021).
struct Recorder {
    std::vector<LifecycleEvent> lifecycle;
    std::vector<ParamRequestId> result_ids;
    std::vector<ParamResult> results;

    static void on_lifecycle(void* ctx, LifecycleEvent ev) {
        static_cast<Recorder*>(ctx)->lifecycle.push_back(ev);
    }
    static void on_param_result(void* ctx, ParamRequestId id, ParamResult result) {
        Recorder* self = static_cast<Recorder*>(ctx);
        self->result_ids.push_back(id);
        self->results.push_back(result);
    }
    size_t deliveries() const {
        return lifecycle.size() + result_ids.size();
    }
};

// --- the rig -----------------------------------------------------------------------------------

// A descriptor a real module could serve: built with the real DescriptorWriter, so the IDENTIFY
// response this rig scripts advertises a length and CRC the engine's own reassembly accepts.
struct Descriptor {
    std::vector<uint8_t> bytes;
    uint16_t crc = 0;
};

Descriptor make_descriptor() {
    uint8_t buf[omgp::LIMIT_max_descriptor_bytes];
    omgp::l3::DescriptorWriter writer(buf, sizeof buf);
    REQUIRE(writer.add_protocol(omgp::l3::ProtocolRec{1, 0}) == omgp::l3::Status::Ok);
    REQUIRE(writer.add_module_type(omgp::l3::ModuleTypeRec{omgp::MODULE_TYPE_CODES[0]}) ==
            omgp::l3::Status::Ok);
    const char* name = "param-under-test";
    REQUIRE(writer.add_name(omgp::l3::Str{reinterpret_cast<const uint8_t*>(name),
                                          static_cast<uint8_t>(__builtin_strlen(name))}) ==
            omgp::l3::Status::Ok);
    REQUIRE(writer.add_model_id(omgp::l3::ModelIdRec{0x1234, 1, 1}) == omgp::l3::Status::Ok);
    Descriptor d;
    d.bytes.assign(buf, buf + writer.size());
    d.crc = omgp::l3::descriptor_crc(d.bytes.data(), d.bytes.size());
    return d;
}

class Rig {
  public:
    Rig() : node_(clock_), descriptor_(make_descriptor()) {
        callbacks_.ctx = &recorder_;
        callbacks_.on_lifecycle = &Recorder::on_lifecycle;
        callbacks_.on_param_result = &Recorder::on_param_result;
        engine_ = std::make_unique<CoreEngine>(node_, clock_, omgp::ADDR_host, callbacks_);
        // Every trunk address is ABSENT until scripted: an unscripted MockL3Node answers
        // ERR_UNKNOWN_OPCODE to everything, which is a valid L2 answer and would enrol all
        // fifteen addresses (contracts/mock-l3-node.md's unset-class rule).
        for (uint8_t addr = omgp::ADDR_backplane_min; addr <= omgp::ADDR_backplane_max; ++addr) {
            absent_[addr] = L3Step::of(SilenceStep{});
            node_.set_script(addr, &absent_[addr], 1);
        }
    }

    // One backplane with `slots` occupied slots, each answering IDENTIFY and READ_DESC from the
    // same descriptor — so every module reaches Discovered and is a legal parameter target.
    void install(uint8_t addr, uint8_t slot_count, uint32_t occupied) {
        std::vector<L3Step>& steps = scripts_[addr];
        steps.clear();
        steps.push_back(L3Step::of(SlotMapStep{slot_count, occupied, occupied}));
        steps.push_back(L3Step::of(StatusStep{ready_status()}));
        steps.push_back(L3Step::of(IdentifyStep{identify_for(descriptor_)}));
        steps.push_back(L3Step::of(DescChunkStep{descriptor_.bytes.data(),
                                                 static_cast<uint16_t>(descriptor_.bytes.size())}));
        node_.set_script(addr, steps.data(), steps.size());
    }

    // Advance the simulated clock, let the double release what is due, hand the engine the same
    // instant. The only place time moves in this file (CLAUDE.md rule 3).
    void step(uint64_t step_us) {
        now_us_ += step_us;
        node_.advance_to(now_us_);
        engine_->run_superframe(now_us_);
    }

    // Drives until `want` modules are Discovered, or fails naming what it got. Bounded by a
    // stall check rather than a flat step budget: a rig that stops making progress fails here,
    // at the stall, instead of spending the whole budget first.
    void run_until_discovered(size_t want) {
        size_t seen = discovered_count();
        uint64_t idle = 0;
        while (seen < want) {
            step(byte_us());
            const size_t now = discovered_count();
            if (now != seen) {
                seen = now;
                idle = 0;
            } else if (++idle >= kStallSteps) {
                FAIL("discovery stalled with " << seen << " of " << want << " modules discovered");
            }
        }
    }

    size_t discovered_count() const {
        size_t n = 0;
        for (unsigned id = omgp::ADDR_module_min; id <= omgp::ADDR_module_max; ++id) {
            if (engine_->discovery_state(static_cast<uint8_t>(id)) == DiscoveryState::Discovered) {
                ++n;
            }
        }
        return n;
    }

    // The first Discovered module id, which the cases use as their parameter target.
    uint8_t a_discovered_node() const {
        for (unsigned id = omgp::ADDR_module_min; id <= omgp::ADDR_module_max; ++id) {
            if (engine_->discovery_state(static_cast<uint8_t>(id)) == DiscoveryState::Discovered) {
                return static_cast<uint8_t>(id);
            }
        }
        FAIL("no module reached Discovered");
        return 0;
    }

    // An id inside the module window that no slot is occupying, so it is a legal uint8_t the
    // engine must refuse as NotDiscovered rather than a malformed one.
    uint8_t an_undiscovered_node() const {
        for (unsigned id = omgp::ADDR_module_min; id <= omgp::ADDR_module_max; ++id) {
            if (engine_->discovery_state(static_cast<uint8_t>(id)) != DiscoveryState::Discovered) {
                return static_cast<uint8_t>(id);
            }
        }
        FAIL("every module id is Discovered; this rig cannot name an undiscovered one");
        return 0;
    }

    CoreEngine& engine() {
        return *engine_;
    }
    Recorder& recorder() {
        return recorder_;
    }
    MockL3Node& node() {
        return node_;
    }
    uint64_t now_us() const {
        return now_us_;
    }

  private:
    // A generous bound on run_until_discovered()'s no-progress window: three status polls, a
    // handful of demand items and one enrolment probe, at one engine call per byte time.
    static constexpr uint64_t kStallSteps = 1200;

    static omgp::l3::StatusBlock ready_status() {
        omgp::l3::StatusBlock b{};
        b.state = omgp::STATE_READY; // protocol-l3 §3.3: a value the codec's range check accepts
        b.active_channel = 0;
        b.bypass = 0;
        b.fault_code = 0;
        b.uptime_s = 1;
        b.event_pending = 0;
        return b;
    }

    static omgp::l3::IdentifyResp identify_for(const Descriptor& d) {
        omgp::l3::IdentifyResp r{};
        r.major = 1;
        r.minor = 0;
        r.module_type = omgp::MODULE_TYPE_CODES[0];
        r.desc_len = static_cast<uint16_t>(d.bytes.size());
        r.desc_crc = d.crc;
        return r;
    }

    FakeClock clock_;
    MockL3Node node_;
    Descriptor descriptor_;
    Recorder recorder_;
    CoreCallbacks callbacks_{};
    std::unique_ptr<CoreEngine> engine_;
    std::vector<L3Step> scripts_[omgp::link::kAddrCount];
    L3Step absent_[omgp::link::kAddrCount];
    uint64_t now_us_ = 0;
};

// One backplane, four modules, all Discovered — the parameter target of every case below.
// Takes the Rig by reference rather than returning one: the engine holds a reference to the
// Rig's own MockL3Node, so a Rig that moved after construction would leave it dangling.
void bring_up(Rig& rig) {
    rig.install(omgp::ADDR_backplane_min, 4, 0x0Fu);
    rig.run_until_discovered(4);
}

} // namespace

TEST_CASE("set_param/get_param: an operation naming a node that is not Discovered is refused, "
          "and nothing is queued [params][us2]") {
    // spec FR-023's precondition and contracts/core-cpp.md's CoreStatus::NotDiscovered: a
    // parameter operation is addressed to a module whose descriptor is known. Refused, not
    // queued against an id whose meaning may change before the item is issued.
    Rig rig;
    bring_up(rig);
    const uint8_t absent = rig.an_undiscovered_node();
    const size_t queued_before = Bk::param_queued(rig.engine());

    ParamRequestId id = 0;
    CHECK(rig.engine().set_param(absent, 1, 0, 100) == CoreStatus::NotDiscovered);
    CHECK(rig.engine().get_param(absent, 1, 0, id) == CoreStatus::NotDiscovered);
    CHECK(Bk::param_queued(rig.engine()) == queued_before);

    SECTION("an id outside the module window is refused the same way, not indexed into the table") {
        // CLAUDE.md rule 7 at the API boundary: every uint8_t is an input here, including the
        // backplane addresses below the module window and the broadcast address above it.
        CHECK(rig.engine().set_param(omgp::ADDR_backplane_min, 1, 0, 100) ==
              CoreStatus::NotDiscovered);
        CHECK(rig.engine().set_param(0xFF, 1, 0, 100) == CoreStatus::NotDiscovered);
        CHECK(rig.engine().get_param(0x00, 1, 0, id) == CoreStatus::NotDiscovered);
        CHECK(Bk::param_queued(rig.engine()) == queued_before);
    }
}

TEST_CASE("set_param: a value the protocol cannot carry is refused at the API boundary, not "
          "queued to fail later [params][us2]") {
    // protocol/omgp-protocol.yaml gives SET_PARAM's value `max: 4095` and l3::encode_set_param
    // returns Status::OutOfRange above it. Refusing at the boundary is what makes the refusal
    // REACH the caller: an item queued with such a value could only fail at issue time, where
    // the sole report channel is FR-023's ParamSetFailed — a report about the application's own
    // programming error, delivered superframes later. The bound is the generated symbol
    // (CLAUDE.md rule 4), so a YAML change moves this case with it.
    Rig rig;
    bring_up(rig);
    const uint8_t node = rig.a_discovered_node();
    const size_t queued_before = Bk::param_queued(rig.engine());

    CHECK(rig.engine().set_param(node, 1, 0, omgp::LIMIT_param_value_max + 1) ==
          CoreStatus::InvalidValue);
    CHECK(Bk::param_queued(rig.engine()) == queued_before);
    CHECK(rig.engine().set_param(node, 1, 0, 0xFFFFu) == CoreStatus::InvalidValue);
    CHECK(Bk::param_queued(rig.engine()) == queued_before);

    SECTION(
        "the greatest value the protocol CAN carry is accepted, so the bound is not off by one") {
        CHECK(rig.engine().set_param(node, 1, 0, omgp::LIMIT_param_value_max) == CoreStatus::Ok);
        CHECK(Bk::param_queued(rig.engine()) == queued_before + 1);
    }
}

TEST_CASE("get_param: the request id is returned by the call itself, before any superframe and "
          "with nothing written to the wire [params][us2]") {
    // spec FR-024: "A GET_PARAM call MUST return immediately having queued the request". The
    // discriminating half is the wire: a call that transacted would have written bytes, so the
    // byte count taken either side of it is what tells "queued" from "sent".
    Rig rig;
    bring_up(rig);
    const uint8_t node = rig.a_discovered_node();
    const size_t requests_before = rig.node().requests_seen();

    ParamRequestId id = 0xA5; // a value the call must overwrite, not leave as it found it
    const CoreStatus st = rig.engine().get_param(node, 2, 0, id);

    CHECK(st == CoreStatus::Ok);
    CHECK(Bk::param_queued(rig.engine()) == 1u);
    CHECK(rig.node().requests_seen() == requests_before);
    // The id space is the FIFO's capacity (data-model.md §8), so every issued id is addressable
    // within it — the property a delivery correlating against the id pool depends on.
    CHECK(id < kParamCapacity);

    SECTION("set_param likewise queues without transacting") {
        CHECK(rig.engine().set_param(node, 2, 0, 1) == CoreStatus::Ok);
        CHECK(Bk::param_queued(rig.engine()) == 2u);
        CHECK(rig.node().requests_seen() == requests_before);
    }
}

TEST_CASE("get_param: two outstanding requests for the same node, parameter and scope get "
          "distinct ids [params][us2]") {
    // research.md R-10's whole reason for an opaque id: keying by (node_id, param_id) would make
    // two outstanding calls for the same pair indistinguishable to the caller on delivery.
    Rig rig;
    bring_up(rig);
    const uint8_t node = rig.a_discovered_node();

    ParamRequestId first = 0, second = 0;
    REQUIRE(rig.engine().get_param(node, 7, 0, first) == CoreStatus::Ok);
    REQUIRE(rig.engine().get_param(node, 7, 0, second) == CoreStatus::Ok);

    CHECK(first != second);
    CHECK(Bk::param_queued(rig.engine()) == 2u);

    SECTION("and each queued item carries its own id, in arrival order (R-07 FIFO)") {
        ParamOpItem item{};
        REQUIRE(Bk::pop_param(rig.engine(), item));
        CHECK(item.kind == ParamOpItem::Kind::Get);
        CHECK(item.node_id == node);
        CHECK(item.param_id == 7u);
        CHECK(item.scope == 0u);
        CHECK(item.request_id == first);
        REQUIRE(Bk::pop_param(rig.engine(), item));
        CHECK(item.request_id == second);
    }
}

TEST_CASE("set_param: the queued item carries every field the caller passed, and no other "
          "[params][us2]") {
    // What the drain loop (T029) will encode is this item, so each field is asserted against a
    // value distinct from the others AND from 42 — the constant Mull's cxx_assign_const stores,
    // which an assertion against a field's default or against 42 itself could not tell apart
    // from the real assignment.
    Rig rig;
    bring_up(rig);
    const uint8_t node = rig.a_discovered_node();

    REQUIRE(rig.engine().set_param(node, 7, 3, 1234) == CoreStatus::Ok);

    ParamOpItem item{};
    REQUIRE(Bk::pop_param(rig.engine(), item));
    CHECK(item.node_id == node);
    CHECK(item.kind == ParamOpItem::Kind::Set);
    CHECK(item.param_id == 7u);
    CHECK(item.scope == 3u);
    CHECK(item.value == 1234u);
    // data-model.md §6: a Set carries no request id, so nothing was taken from R-10's pool —
    // the property the "every id outstanding" case below relies on to keep Sets admissible.
    CHECK(item.request_id == 0u);

    SECTION("two identical calls queue two identically-encoding items (CLAUDE.md rule 2)") {
        // L3 sets are ABSOLUTE: the queued value is the caller's, never a delta computed from a
        // value read back, so a retry at any layer is safe.
        REQUIRE(rig.engine().set_param(node, 7, 3, 1234) == CoreStatus::Ok);
        REQUIRE(rig.engine().set_param(node, 7, 3, 1234) == CoreStatus::Ok);
        ParamOpItem a{}, b{};
        REQUIRE(Bk::pop_param(rig.engine(), a));
        REQUIRE(Bk::pop_param(rig.engine(), b));
        CHECK(a.node_id == b.node_id);
        CHECK(a.param_id == b.param_id);
        CHECK(a.scope == b.scope);
        CHECK(a.value == b.value);
        CHECK(a.kind == b.kind);
    }
}

TEST_CASE("get_param: a request id is released when its result is delivered, and only then "
          "[params][us2]") {
    // R-10's "reused once its result has been delivered", which is the whole reason the pool is
    // a uint8_t sized against the FIFO rather than a growing counter. The release point is
    // drain_callbacks(), so this case exhausts the pool, delivers ONE result through the §8a
    // ring (the producer T030 will be), and shows the freed id — and only it — comes back.
    Rig rig;
    bring_up(rig);
    const uint8_t node = rig.a_discovered_node();
    // Discovery's own NodeDiscovered events are still queued, and drain_callbacks() ALTERNATES
    // between the two §8a rings — so a budget of one spent while the lifecycle ring is
    // non-empty may serve that ring instead. Emptied here so the single delivery below is
    // unambiguously the parameter result's.
    rig.engine().drain_callbacks(kParamCapacity * 2u);
    REQUIRE(rig.engine().dropped_deliveries() == 0u);

    std::vector<ParamRequestId> ids;
    for (size_t i = 0; i < kParamCapacity; ++i) {
        ParamRequestId id = 0;
        REQUIRE(rig.engine().get_param(node, 3, 0, id) == CoreStatus::Ok);
        ids.push_back(id);
        ParamOpItem item{};
        REQUIRE(Bk::pop_param(rig.engine(), item)); // in flight: dequeued, result still pending
    }
    ParamRequestId fresh = 0xA5;
    REQUIRE(rig.engine().get_param(node, 3, 0, fresh) == CoreStatus::RequestIdReused);

    // Queued but NOT yet delivered: the id is still outstanding, so the pool is still empty.
    ParamResult result{};
    result.ok = true;
    result.value = 7;
    REQUIRE(Bk::enqueue_param_result(rig.engine(), ids[3], result));
    CHECK(rig.engine().get_param(node, 3, 0, fresh) == CoreStatus::RequestIdReused);

    // Delivered: now that id, and no other, is available again.
    rig.recorder().result_ids.clear();
    rig.engine().drain_callbacks(1);
    REQUIRE(rig.recorder().result_ids.size() == 1u);
    CHECK(rig.recorder().result_ids[0] == ids[3]);

    REQUIRE(rig.engine().get_param(node, 3, 0, fresh) == CoreStatus::Ok);
    CHECK(fresh == ids[3]);
    // And the pool is empty again: one released, one taken.
    ParamRequestId again = 0xA5;
    CHECK(rig.engine().get_param(node, 3, 0, again) == CoreStatus::RequestIdReused);
    CHECK(again == 0xA5);
}

TEST_CASE("get_param: a ring that is full of gets is refused as QueueFull, not RequestIdReused "
          "[params][us2]") {
    // The two refusals are reachable together — every queued Get holds an id, so a ring filled
    // with Gets exhausts the pool at the same moment — and the ring is checked FIRST, so that
    // is the status the caller sees. Pinned because the alternative order is observationally
    // different only in this state, and it is the state a parameter burst actually reaches.
    Rig rig;
    bring_up(rig);
    const uint8_t node = rig.a_discovered_node();

    for (size_t i = 0; i < kParamCapacity; ++i) {
        ParamRequestId id = 0;
        REQUIRE(rig.engine().get_param(node, 3, 0, id) == CoreStatus::Ok);
    }
    REQUIRE(Bk::param_queued(rig.engine()) == kParamCapacity);

    ParamRequestId refused = 0xA5;
    CHECK(rig.engine().get_param(node, 3, 0, refused) == CoreStatus::QueueFull);
    CHECK(refused == 0xA5);
}

TEST_CASE("set_param: a full parameter ring refuses the next operation and queues nothing "
          "[params][us2]") {
    // data-model.md §6/§8a's drop-newest rule at the API boundary: CoreStatus::QueueFull is a
    // refusal the caller can act on, where a silent drop would make FR-023's "never dropped"
    // false before the operation ever reached the wire. Fills by its generated capacity symbol.
    Rig rig;
    bring_up(rig);
    const uint8_t node = rig.a_discovered_node();

    for (size_t i = 0; i < kParamCapacity; ++i) {
        REQUIRE(rig.engine().set_param(node, 1, 0, 1) == CoreStatus::Ok);
    }
    REQUIRE(Bk::param_queued(rig.engine()) == kParamCapacity);

    CHECK(rig.engine().set_param(node, 1, 0, 1) == CoreStatus::QueueFull);
    CHECK(Bk::param_queued(rig.engine()) == kParamCapacity);

    SECTION("get_param is refused by the same ring, and takes no request id when it is") {
        // The id pool must not leak on a refused call: an id claimed and then abandoned is one
        // the pool never gets back, since R-10 releases an id only when its result is delivered.
        ParamRequestId id = 0;
        CHECK(rig.engine().get_param(node, 1, 0, id) == CoreStatus::QueueFull);
        CHECK(Bk::param_queued(rig.engine()) == kParamCapacity);

        ParamOpItem item{};
        REQUIRE(Bk::pop_param(rig.engine(), item));
        CHECK(rig.engine().get_param(node, 1, 0, id) == CoreStatus::Ok);
        CHECK(id < kParamCapacity);
    }
}

TEST_CASE("get_param: with every request id outstanding the next call is refused, and an id is "
          "reusable once its result has been delivered [params][us2]") {
    // R-10: the id space is scoped to OUTSTANDING requests and sized against the FIFO's
    // capacity, so it can only be exhausted once items have left the ring while their results
    // are still undelivered — the state the drain loop (T029) creates and this case reaches by
    // popping the ring through the seam. CoreStatus::RequestIdReused is that refusal.
    Rig rig;
    bring_up(rig);
    const uint8_t node = rig.a_discovered_node();

    std::vector<ParamRequestId> ids;
    for (size_t i = 0; i < kParamCapacity; ++i) {
        ParamRequestId id = 0;
        REQUIRE(rig.engine().get_param(node, 3, 0, id) == CoreStatus::Ok);
        ids.push_back(id);
        ParamOpItem item{};
        REQUIRE(Bk::pop_param(rig.engine(), item)); // in flight: dequeued, result still pending
    }
    // Every id is distinct, which is what makes the pool exhausted rather than merely busy.
    for (size_t i = 0; i < ids.size(); ++i) {
        for (size_t j = i + 1; j < ids.size(); ++j) {
            REQUIRE(ids[i] != ids[j]);
        }
    }
    REQUIRE(Bk::param_queued(rig.engine()) == 0u);

    ParamRequestId refused = 0xA5;
    CHECK(rig.engine().get_param(node, 3, 0, refused) == CoreStatus::RequestIdReused);
    CHECK(refused == 0xA5); // untouched on refusal
    CHECK(Bk::param_queued(rig.engine()) == 0u);

    SECTION("a set_param is still accepted: it takes no id, so the pool does not gate it") {
        CHECK(rig.engine().set_param(node, 3, 0, 1) == CoreStatus::Ok);
    }
}

TEST_CASE("FR-021: accepting a parameter operation and running superframes invokes no callback "
          "[params][us2]") {
    // spec FR-021 / data-model.md §8a: run_superframe() enqueues, drain_callbacks() delivers.
    // This is asserted here for the engine body at THIS head — with a parameter ring that
    // nothing drains yet — and is not a standing guard for the bodies T029/T030 add: their own
    // tests must re-assert it once a queued operation can complete.
    Rig rig;
    bring_up(rig);
    const uint8_t node = rig.a_discovered_node();
    rig.recorder().lifecycle.clear();
    rig.recorder().result_ids.clear();
    rig.recorder().results.clear();

    ParamRequestId id = 0;
    REQUIRE(rig.engine().set_param(node, 4, 0, 9) == CoreStatus::Ok);
    REQUIRE(rig.engine().get_param(node, 4, 0, id) == CoreStatus::Ok);
    for (int i = 0; i < 20; ++i) {
        rig.step(byte_us());
    }

    CHECK(rig.recorder().deliveries() == 0u);
}

TEST_CASE("CLAUDE.md rule 5: neither API call allocates [params][us2]") {
    // rule 5 for the embedded path, demonstrated rather than asserted in prose: the counting
    // heap guard fails the case on any malloc/new reaching the wrapped allocators while the two
    // calls run — including the refusal paths, where an early return is the easiest place for a
    // container to have been built.
    Rig rig;
    bring_up(rig);
    const uint8_t node = rig.a_discovered_node();
    const uint8_t absent = rig.an_undiscovered_node();

    HEAP_FREE_SCOPE({
        ParamRequestId id = 0;
        REQUIRE(rig.engine().set_param(node, 5, 0, 3) == CoreStatus::Ok);
        REQUIRE(rig.engine().get_param(node, 5, 0, id) == CoreStatus::Ok);
        REQUIRE(rig.engine().set_param(absent, 5, 0, 3) == CoreStatus::NotDiscovered);
        REQUIRE(rig.engine().set_param(node, 5, 0, omgp::LIMIT_param_value_max + 1) ==
                CoreStatus::InvalidValue);
    });
}
