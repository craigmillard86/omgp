// CoreEngine::set_param()/get_param() — spec FR-023/FR-024 end to end: the ENQUEUE side
// (tasks.md T026, made to pass by T027) and, below the `--- T030 ---` banner, the REPORTING side
// (T026's remaining cases, made to pass by T030). Driven against a real CoreEngine over the real
// link::Master and link::HealthTracker it owns, answered by MockL3Node (R-08) on a FakeClock: no
// hand-built frames, no wall clock (CLAUDE.md rule 3).
// Spec: specs/003-host-core-engine/spec.md FR-023 (a failed parameter operation is REPORTED,
// never dropped), FR-024 (GET_PARAM returns immediately with a correlation tag), FR-021 (no
// callback from run_superframe()); data-model.md §6 (ParamOpItem), §8 (ParamRequestId/
// ParamResult), §8a (the pending-delivery rings); research.md R-07 (three fixed rings), R-10
// (the opaque, reusable request id); contracts/core-cpp.md §Engine (set_param/get_param).
//
// WHAT THIS FILE IS AN ORACLE FOR, AND WHAT IT IS NOT (CLAUDE.md rule 11). The cases above the
// `--- T030 ---` banner are about what the two API calls ACCEPT, REFUSE and QUEUE, and about the
// request-id pool; what they assert about the wire is the negative FR-024 needs, that accepting an
// operation writes nothing. The cases below that banner are about the OUTCOME of an operation that
// has been issued: one scripted answer per transaction (MockL3Node's ParamStep) or none at all
// (SilenceStep), and the delivery it produces. Still NOT an oracle here: the REQUEST payload's own
// bytes (nothing reads a SET_PARAM off the wire, so a param_id carried wrongly into the request is
// invisible to this file unless the answer's echo disagrees with it — which the `an answer for a
// parameter or scope nobody asked about` case below does pin, since the double echoes the
// request's own fields back); the channel-switch and event paths (US3/US4); and anything about
// L2 retry policy, which is link::Master's and is consumed here, never asserted.
#include "core/core_engine.hpp"

#include "catch_amalgamated.hpp"
#include "fake_clock.hpp"
#include "heap_guard.hpp"
#include "l3/l3_descriptor.hpp"
#include "l3/l3_header.hpp"
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
using omgp::core::LifecycleKind;
using omgp::core::ParamOpItem;
using omgp::core::ParamRequestId;
using omgp::core::ParamResult;
using omgp_test::DescChunkStep;
using omgp_test::FakeClock;
using omgp_test::IdentifyStep;
using omgp_test::L3Step;
using omgp_test::MockL3Node;
using omgp_test::ParamStep;
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
    // The producer T030 is on the real path: a decoded answer enqueued against its request id.
    // Used directly by the cases that want to reach delivery — and so the release of the id —
    // without an issued transaction, and to FILL the §8a ring to its capacity.
    static bool enqueue_param_result(CoreEngine& engine, ParamRequestId id,
                                     const ParamResult& result) {
        return engine.enqueue_param_result(id, result);
    }
    // The two §8a rings' depths, read WITHOUT delivering anything. This is what makes spec
    // FR-021's two halves separable: that a superframe ENQUEUED an outcome, and that it invoked
    // no callback while doing so. Reading them by draining would conflate the two.
    static size_t results_pending(CoreEngine& engine) {
        return engine.pending_param_results_.size();
    }
    static size_t lifecycle_pending(CoreEngine& engine, LifecycleKind kind) {
        size_t n = 0;
        for (size_t i = 0; i < engine.pending_lifecycle_.size(); ++i) {
            if (engine.pending_lifecycle_.at(i).kind == kind) {
                ++n;
            }
        }
        return n;
    }
    // The answer decoders themselves (protocol-l3 §3.1), called with bytes no MockL3Node script
    // can produce: the double answers every parameter request with that request's own response
    // opcode or an ERROR, so a truncated message, an unexpected opcode and an echo for the wrong
    // parameter are reachable only here. Same instrument, and the same reason, as
    // CoreEngine::reconcile_slot_map() being public — "for tests that want to hand the engine a
    // response the wire cannot carry" (core_engine.hpp).
    static ParamResult decode_get_answer(CoreEngine& engine, const uint8_t* resp, size_t len,
                                         uint8_t param_id, uint8_t scope) {
        return engine.decode_get_answer(resp, len, param_id, scope);
    }
    static ParamResult decode_set_answer(CoreEngine& engine, const uint8_t* resp, size_t len) {
        return engine.decode_set_answer(resp, len);
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
    void clear() {
        lifecycle.clear();
        result_ids.clear();
        results.clear();
    }
    // Counted by KIND, not by ring depth: a case that silences a backplane to fail a parameter
    // transaction also earns that backplane's own BackplaneSuspect notices (R-03), which are a
    // different report about a different thing. Filtering is what keeps "exactly one
    // ParamSetFailed" an assertion about FR-023 and not about the health stream.
    size_t count(LifecycleKind kind) const {
        size_t n = 0;
        for (const LifecycleEvent& ev : lifecycle) {
            if (ev.kind == kind) {
                ++n;
            }
        }
        return n;
    }
    // The first delivered event of `kind`, or nullptr — so a case names the event it asserts on
    // rather than indexing a position the health stream could shift.
    const LifecycleEvent* first(LifecycleKind kind) const {
        for (const LifecycleEvent& ev : lifecycle) {
            if (ev.kind == kind) {
                return &ev;
            }
        }
        return nullptr;
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
        bp_addr_ = addr;
        std::vector<L3Step>& steps = scripts_[addr];
        steps.clear();
        steps.push_back(L3Step::of(SlotMapStep{slot_count, occupied, occupied}));
        steps.push_back(L3Step::of(StatusStep{ready_status()}));
        steps.push_back(L3Step::of(IdentifyStep{identify_for(descriptor_)}));
        steps.push_back(L3Step::of(DescChunkStep{descriptor_.bytes.data(),
                                                 static_cast<uint16_t>(descriptor_.bytes.size())}));
        node_.set_script(addr, steps.data(), steps.size());
    }

    // Appends the answers this address gives to parameter requests, in order — one ParamStep per
    // SET_PARAM/GET_PARAM request, taken by the Param opcode class's own cursor
    // (contracts/mock-l3-node.md: "steps consumed in order per opcode class"), and the last one
    // repeating once they are spent. Re-installing the script resets every cursor, which is
    // harmless here: install()'s four discovery steps are then re-offered, and a rig whose modules
    // are already Discovered issues no further IDENTIFY or READ_DESC. Deliberately NO wildcard in
    // this script — a SilenceStep or ErrorStep is taken by the first request of ANY class, so the
    // status poll that opens every superframe would consume it before any parameter request could.
    void arm_params(const ParamStep* params, size_t count) {
        std::vector<L3Step>& steps = scripts_[bp_addr_];
        REQUIRE(steps.size() + count <= MockL3Node::kMaxScript);
        for (size_t i = 0; i < count; ++i) {
            steps.push_back(L3Step::of(params[i]));
        }
        node_.set_script(bp_addr_, steps.data(), steps.size());
    }
    void arm_params(const ParamStep& one) {
        arm_params(&one, 1);
    }

    // Makes this backplane answer NOTHING at all, so a transaction addressed to it or to one of
    // its modules ends in link::Master's own retry exhaustion (trunk §7) rather than in an L3
    // answer. A one-step {SilenceStep} script: the wildcard is the NODE's disposition, so every
    // opcode class falls back to it (contracts/mock-l3-node.md, and MockL3Node::set_script()'s
    // own note). Its modules keep their DiscoveryState — nothing un-discovers a node on a failed
    // transaction — so a queued parameter operation is still issued to one.
    void go_silent() {
        scripts_[bp_addr_].assign(1, L3Step::of(SilenceStep{}));
        node_.set_script(bp_addr_, scripts_[bp_addr_].data(), 1);
    }

    // Drives superframes until `done()`, failing by NAME at the budget rather than leaving a
    // later assertion to report the absence. The budget is in superframe steps, not in time: a
    // parameter transaction concludes inside link::Master's own retry window and this rig never
    // waits on a clock reading (CLAUDE.md rule 3).
    template <typename Done> void run_until(const char* what, Done done) {
        for (uint64_t i = 0; i < kSettleSteps; ++i) {
            if (done()) {
                return;
            }
            step(byte_us());
        }
        if (!done()) {
            FAIL("timed out after " << kSettleSteps << " superframes waiting for " << what);
        }
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
    // run_until()'s budget. Generous against the longest wait any case here asks for: a parameter
    // transaction that gets no answer, behind a status poll that got none either, is two full
    // link::Master retry windows (trunk §7, MEASURED ~1270 us each) at one engine call per byte
    // time. Well inside TRUNK_offline_after_suspect_ms, so a silenced backplane in these cases
    // never reaches a declared bus fault — which would stop demand traffic (FR-003) and so stop
    // the very transaction the case is waiting on.
    static constexpr uint64_t kSettleSteps = 4000;

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
    uint8_t bp_addr_ = omgp::ADDR_backplane_min; // the one install() scripted
};

// One backplane, four modules, all Discovered — the parameter target of every case below.
// Takes the Rig by reference rather than returning one: the engine holds a reference to the
// Rig's own MockL3Node, so a Rig that moved after construction would leave it dangling.
void bring_up(Rig& rig) {
    rig.install(omgp::ADDR_backplane_min, 4, 0x0Fu);
    rig.run_until_discovered(4);
}

// bring_up() plus a clean slate: discovery's own NodeDiscovered events delivered and forgotten,
// and nothing left queued on either §8a ring, so what a case then reads off those rings is its
// own parameter traffic and not User Story 1's.
void bring_up_quiet(Rig& rig) {
    bring_up(rig);
    rig.engine().drain_callbacks();
    REQUIRE(rig.engine().dropped_deliveries() == 0u);
    REQUIRE(Bk::results_pending(rig.engine()) == 0u);
    rig.recorder().clear();
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

// === --- T030 --- the REPORTING side: FR-023/FR-024, one outcome per operation, never silence ==

TEST_CASE("FR-024: a Get answered with a GET_PARAM response delivers that value under the id "
          "get_param returned [params][us2]") {
    // spec FR-024's later half (the immediate half is the `the request id is returned by the call
    // itself` case above): "the value or failure is delivered later, tagged for matching".
    // protocol-l3 §3.1: GET_PARAM's response carries param_id, scope and a u16 value, decoded
    // here by l3::decode_get_param_resp and by nothing hand-rolled (R-02).
    Rig rig;
    bring_up_quiet(rig);
    const uint8_t node = rig.a_discovered_node();
    // Distinct from each other, from the param_id and scope asked below, and from 42 (Mull's
    // cxx_assign_const constant), so a field or id carried from the wrong place is visible.
    rig.arm_params(ParamStep{true, 1234, 0});

    ParamRequestId id = 0xA5;
    REQUIRE(rig.engine().get_param(node, 7, 2, id) == CoreStatus::Ok);
    rig.run_until("the GET_PARAM answer to be enqueued",
                  [&] { return Bk::results_pending(rig.engine()) == 1u; });

    // spec FR-021: the superframes that issued and completed this transaction called back zero
    // times. Asserted BEFORE the drain, which is the only thing that can change it.
    CHECK(rig.recorder().deliveries() == 0u);

    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().results.size() == 1u);
    CHECK(rig.recorder().result_ids[0] == id);
    CHECK(rig.recorder().results[0].ok);
    CHECK(rig.recorder().results[0].value == 1234u);
    // A Get's outcome is a ParamResult, never a lifecycle event (data-model.md §7's own note on
    // ParamSetFailed: "Set only; a Get failure is a ParamResult").
    CHECK(rig.recorder().count(LifecycleKind::ParamSetFailed) == 0u);
}

TEST_CASE("FR-023: a Get answered ERROR is delivered as a not-ok result carrying that code, under "
          "the same id [params][us2]") {
    // spec FR-023: a module error response "MUST reach the application naming the operation and
    // the reason". protocol-l3 §3.1: the failure arrives as opcode OP_ERROR with flags.error set,
    // whose code l3::decode_error_resp reads. The code is a generated symbol (CLAUDE.md rule 1),
    // so a YAML renumbering moves this case with it.
    Rig rig;
    bring_up_quiet(rig);
    const uint8_t node = rig.a_discovered_node();
    rig.arm_params(ParamStep{false, 0, omgp::ERR_UNKNOWN_TARGET});

    ParamRequestId id = 0xA5;
    REQUIRE(rig.engine().get_param(node, 3, 0, id) == CoreStatus::Ok);
    rig.run_until("the ERROR answer to be enqueued",
                  [&] { return Bk::results_pending(rig.engine()) == 1u; });
    CHECK(rig.recorder().deliveries() == 0u);

    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().results.size() == 1u);
    CHECK(rig.recorder().result_ids[0] == id);
    CHECK_FALSE(rig.recorder().results[0].ok);
    CHECK(rig.recorder().results[0].reason == omgp::ERR_UNKNOWN_TARGET);
    CHECK(rig.recorder().count(LifecycleKind::ParamSetFailed) == 0u);
}

TEST_CASE("FR-023: a Get whose transaction never answers is delivered as a not-ok result, not "
          "left pending for ever [params][us2]") {
    // FR-023's "or exhaustion of the link layer's own retry": link::Master has already retried
    // this transaction per trunk §7 and given up, and no core-level retry exists (that policy is
    // L2's — docs/OPEN-QUESTIONS.md 2026-09-06). The report carries no L3 error code, because
    // there was no answer to read one from.
    Rig rig;
    bring_up_quiet(rig);
    const uint8_t node = rig.a_discovered_node();

    ParamRequestId id = 0xA5;
    REQUIRE(rig.engine().get_param(node, 5, 1, id) == CoreStatus::Ok);
    rig.go_silent(); // after queuing, so the operation is issued before the answers stop
    rig.run_until("the retry-exhausted Get to be reported",
                  [&] { return Bk::results_pending(rig.engine()) == 1u; });
    CHECK(rig.recorder().deliveries() == 0u);

    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().results.size() == 1u);
    CHECK(rig.recorder().result_ids[0] == id);
    CHECK_FALSE(rig.recorder().results[0].ok);
    CHECK(rig.recorder().results[0].reason == omgp::core::kParamFailureNoErrorCode);
    // The reason is NOT a protocol error code dressed up as one: the two vocabularies stay
    // distinguishable, which is what lets an application tell "the module refused" from "the
    // module never answered" (data-model.md §8's "or a link/transaction-level failure").
    CHECK(rig.recorder().results[0].reason != omgp::ERR_BUSY);
    CHECK(rig.recorder().results[0].reason != omgp::ERR_INTERNAL);
}

TEST_CASE("FR-023: a Set answered ERROR enqueues exactly one ParamSetFailed naming the parameter "
          "and the reason [params][us2]") {
    // data-model.md §7: a Set's failure is a LifecycleEvent (ParamSetFailed, failed_param_id +
    // failed_reason), because a Set carries no request id to tag a ParamResult with (§6).
    Rig rig;
    bring_up_quiet(rig);
    const uint8_t node = rig.a_discovered_node();
    rig.arm_params(ParamStep{false, 0, omgp::ERR_NOT_PERMITTED});

    REQUIRE(rig.engine().set_param(node, 9, 1, 300) == CoreStatus::Ok);
    rig.run_until("the failed Set to be reported", [&] {
        return Bk::lifecycle_pending(rig.engine(), LifecycleKind::ParamSetFailed) == 1u;
    });
    CHECK(rig.recorder().deliveries() == 0u);
    // A Set takes no request id, so its failure must not arrive on the result ring under some
    // id the caller never saw.
    CHECK(Bk::results_pending(rig.engine()) == 0u);

    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::ParamSetFailed) == 1u);
    const LifecycleEvent* ev = rig.recorder().first(LifecycleKind::ParamSetFailed);
    REQUIRE(ev != nullptr);
    CHECK(ev->node_id == node);
    CHECK(ev->failed_param_id == 9u);
    CHECK(ev->failed_reason == omgp::ERR_NOT_PERMITTED);
    CHECK(rig.recorder().results.empty());
}

TEST_CASE("FR-023: a Set whose transaction never answers is reported too, with no L3 code "
          "[params][us2]") {
    // The second of FR-023's two routes for a Set, and the one a module cannot produce: trunk
    // §7's retries are spent inside link::Master and the transaction's own Failed event is the
    // only notice of it.
    Rig rig;
    bring_up_quiet(rig);
    const uint8_t node = rig.a_discovered_node();

    REQUIRE(rig.engine().set_param(node, 11, 0, 7) == CoreStatus::Ok);
    rig.go_silent();
    rig.run_until("the retry-exhausted Set to be reported", [&] {
        return Bk::lifecycle_pending(rig.engine(), LifecycleKind::ParamSetFailed) == 1u;
    });
    CHECK(rig.recorder().deliveries() == 0u);

    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::ParamSetFailed) == 1u);
    const LifecycleEvent* ev = rig.recorder().first(LifecycleKind::ParamSetFailed);
    REQUIRE(ev != nullptr);
    CHECK(ev->node_id == node);
    CHECK(ev->failed_param_id == 11u);
    CHECK(ev->failed_reason == omgp::core::kParamFailureNoErrorCode);
    CHECK(rig.recorder().results.empty());
}

TEST_CASE("protocol-l3 §3.1: a Set accepted with the empty response is SILENT — nothing is "
          "delivered for it at all [params][us2]") {
    // §3.1: "a SET_PARAM success response is empty (acceptance); failures arrive as ERROR". A
    // Set carries no request id (data-model.md §6), so success has nothing to report against and
    // must report nothing — the other half of FR-023, which is about failures only.
    //
    // The DISCRIMINATING part is the Get queued behind it: without it, "no delivery yet" and "no
    // delivery ever" are the same observation. The Get's own result arrives only after the Set's
    // transaction has concluded (R-07's ring is FIFO), so one delivery in total is what proves
    // the accepted Set contributed none.
    Rig rig;
    bring_up_quiet(rig);
    const uint8_t node = rig.a_discovered_node();
    const ParamStep answers[] = {ParamStep{true, 0, 0},    // the Set: accepted
                                 ParamStep{true, 555, 0}}; // the Get behind it
    rig.arm_params(answers, 2);

    ParamRequestId id = 0xA5;
    REQUIRE(rig.engine().set_param(node, 6, 0, 2048) == CoreStatus::Ok);
    REQUIRE(rig.engine().get_param(node, 6, 0, id) == CoreStatus::Ok);
    rig.run_until("the Get behind the accepted Set to be answered",
                  [&] { return Bk::results_pending(rig.engine()) == 1u; });

    CHECK(Bk::lifecycle_pending(rig.engine(), LifecycleKind::ParamSetFailed) == 0u);
    rig.engine().drain_callbacks();
    CHECK(rig.recorder().count(LifecycleKind::ParamSetFailed) == 0u);
    REQUIRE(rig.recorder().results.size() == 1u);
    CHECK(rig.recorder().result_ids[0] == id);
    CHECK(rig.recorder().results[0].value == 555u);
    CHECK(rig.recorder().deliveries() == 1u); // the Get's, and nothing else
}

TEST_CASE("FR-021/R-04: a Set failure and a Get value complete inside superframes that call back "
          "zero times, and idle superframes deliver nothing more [params][us2]") {
    // The FR-021 case above this banner is explicit that it guards only the engine body that
    // existed before a queued operation could complete, and that "their own tests must re-assert
    // it once a queued operation can complete". This is that re-assertion, with both report
    // channels in flight at once.
    Rig rig;
    bring_up_quiet(rig);
    const uint8_t node = rig.a_discovered_node();
    const ParamStep answers[] = {ParamStep{false, 0, omgp::ERR_INTERNAL}, ParamStep{true, 77, 0}};
    rig.arm_params(answers, 2);

    ParamRequestId id = 0xA5;
    REQUIRE(rig.engine().set_param(node, 8, 0, 1) == CoreStatus::Ok);
    REQUIRE(rig.engine().get_param(node, 8, 0, id) == CoreStatus::Ok);
    rig.run_until("both parameter operations to be reported", [&] {
        return Bk::results_pending(rig.engine()) == 1u &&
               Bk::lifecycle_pending(rig.engine(), LifecycleKind::ParamSetFailed) == 1u;
    });
    CHECK(rig.recorder().deliveries() == 0u);

    rig.engine().drain_callbacks();
    CHECK(rig.recorder().count(LifecycleKind::ParamSetFailed) == 1u);
    REQUIRE(rig.recorder().results.size() == 1u);
    CHECK(rig.recorder().results[0].value == 77u);

    // And the delivery count does not grow with idle superframes: one outcome per operation, not
    // one per superframe that observes the operation's record.
    const size_t delivered = rig.recorder().deliveries();
    for (int i = 0; i < 40; ++i) {
        rig.step(byte_us());
    }
    rig.engine().drain_callbacks();
    CHECK(rig.recorder().deliveries() == delivered);
}

TEST_CASE("R-10: two Gets outstanding for the same node, parameter and scope are delivered under "
          "their own ids with their own values [params][us2]") {
    // The delivery half of the `two outstanding requests ... get distinct ids` case above: an
    // opaque id is only useful if the VALUE it carries is the one its own request asked for.
    // Keyed by (node_id, param_id) these two would be indistinguishable on delivery, which is
    // research.md R-10's whole reason for existing.
    Rig rig;
    bring_up_quiet(rig);
    const uint8_t node = rig.a_discovered_node();
    const ParamStep answers[] = {ParamStep{true, 111, 0}, ParamStep{true, 222, 0}};
    rig.arm_params(answers, 2);

    ParamRequestId first = 0, second = 0;
    REQUIRE(rig.engine().get_param(node, 7, 0, first) == CoreStatus::Ok);
    REQUIRE(rig.engine().get_param(node, 7, 0, second) == CoreStatus::Ok);
    REQUIRE(first != second);
    rig.run_until("both Gets to be answered",
                  [&] { return Bk::results_pending(rig.engine()) == 2u; });
    CHECK(rig.recorder().deliveries() == 0u);

    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().results.size() == 2u);
    // R-07's ring is FIFO and the double answers in script order, so the first request's own
    // answer is 111 and the second's is 222 — the correlation, not merely two deliveries.
    CHECK(rig.recorder().result_ids[0] == first);
    CHECK(rig.recorder().results[0].value == 111u);
    CHECK(rig.recorder().result_ids[1] == second);
    CHECK(rig.recorder().results[1].value == 222u);
}

TEST_CASE("FR-023: an ERR_BUSY answer is REPORTED, not retried, and leaves the parameter ring "
          "empty [params][us2]") {
    // trunk §8/§10.5 leave the persistent-busy bound at L2 (docs/OPEN-QUESTIONS.md 2026-09-06),
    // and no core-level retry, backoff or coalescing policy exists. So ERR_BUSY is an ANSWER
    // here: one report, and the operation is over. A re-queue would be visible as a non-empty
    // ring, and would be the start of a core-level retry policy nobody ruled.
    Rig rig;
    bring_up_quiet(rig);
    const uint8_t node = rig.a_discovered_node();
    rig.arm_params(ParamStep{false, 0, omgp::ERR_BUSY});

    ParamRequestId id = 0xA5;
    REQUIRE(rig.engine().get_param(node, 4, 0, id) == CoreStatus::Ok);
    rig.run_until("the ERR_BUSY answer to be reported",
                  [&] { return Bk::results_pending(rig.engine()) == 1u; });

    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().results.size() == 1u);
    CHECK_FALSE(rig.recorder().results[0].ok);
    CHECK(rig.recorder().results[0].reason == omgp::ERR_BUSY);
    CHECK(Bk::param_queued(rig.engine()) == 0u);

    SECTION("and stays empty: no later superframe re-issues it") {
        for (int i = 0; i < 200; ++i) {
            rig.step(byte_us());
        }
        CHECK(Bk::param_queued(rig.engine()) == 0u);
        rig.engine().drain_callbacks();
        CHECK(rig.recorder().results.size() == 1u);
    }
}

TEST_CASE("data-model.md §8a: a full result ring refuses the NEWEST arrival and counts it, never "
          "evicting a queued delivery [params][us2]") {
    // §8a's drop-newest rule on the reporting side. Filled through the seam rather than by 128
    // real transactions, because what is under test is the enqueue point's behaviour on a full
    // ring, not the wire. The ARRIVAL is a real one: a Get answered over the wire, so the
    // refusal happens where T030's own enqueue is.
    Rig rig;
    bring_up_quiet(rig);
    const uint8_t node = rig.a_discovered_node();
    rig.arm_params(ParamStep{true, 1234, 0});

    for (size_t i = 0; i < kParamCapacity; ++i) {
        ParamResult filler{};
        filler.ok = true;
        filler.value = static_cast<uint16_t>(0x100u + i); // distinct from the real answer's 1234
        REQUIRE(Bk::enqueue_param_result(rig.engine(), static_cast<ParamRequestId>(i), filler));
    }
    REQUIRE(Bk::results_pending(rig.engine()) == kParamCapacity);
    const uint32_t dropped_before = rig.engine().dropped_deliveries();

    ParamRequestId id = 0xA5;
    REQUIRE(rig.engine().get_param(node, 7, 2, id) == CoreStatus::Ok);
    rig.run_until("the answered Get to be refused by the full ring",
                  [&] { return rig.engine().dropped_deliveries() == dropped_before + 1u; });

    // Nothing was evicted to make room: the ring is still exactly as deep as it was, and the
    // OLDEST filler is still the first thing delivered.
    CHECK(Bk::results_pending(rig.engine()) == kParamCapacity);
    rig.engine().drain_callbacks(1);
    REQUIRE(rig.recorder().results.size() == 1u);
    CHECK(rig.recorder().results[0].value == 0x100u);
}

TEST_CASE("CLAUDE.md rule 7: an answer that is not this request's own response yields a not-ok "
          "result, never a fabricated value [params][us2]") {
    // TOTAL for any bytes (spec FR-030's forward-compatibility rule): the decoders below are
    // called with messages MockL3Node cannot produce — it answers every parameter request with
    // that request's own response opcode or an ERROR. Run under the native preset's ASan/UBSan,
    // so an over-read on a truncated or arbitrary message fails the case rather than passing it.
    Rig rig;
    bring_up_quiet(rig);
    CoreEngine& engine = rig.engine();

    // A well-formed GET_PARAM response, built by the real encoders, as the control: the same
    // bytes that DO decode to an ok result are then mutated below, so each case's failure is
    // attributable to its own mutation and not to the message being wrong all along.
    uint8_t good[omgp::LIMIT_max_l3_message] = {};
    size_t good_len = 0;
    {
        omgp::l3::GetParamResp r{};
        r.param_id = 7;
        r.scope = 2;
        r.value = 1234;
        uint8_t payload[omgp::LIMIT_max_l3_payload] = {};
        size_t plen = 0;
        REQUIRE(omgp::l3::encode_get_param_resp(r, payload, sizeof payload, plen) ==
                omgp::l3::Status::Ok);
        omgp::l3::Header h{};
        h.opcode = omgp::OP_GET_PARAM;
        h.node_id = rig.a_discovered_node();
        h.seq = 1;
        h.flags = omgp::FLAG_response;
        h.payload_len = static_cast<uint8_t>(plen);
        REQUIRE(omgp::l3::encode_header(h, good, sizeof good, good_len) == omgp::l3::Status::Ok);
        for (size_t i = 0; i < plen; ++i) {
            good[good_len + i] = payload[i];
        }
        good_len += plen;
    }
    const ParamResult control = Bk::decode_get_answer(engine, good, good_len, 7, 2);
    REQUIRE(control.ok);
    REQUIRE(control.value == 1234u);

    SECTION("an answer for a parameter or scope nobody asked about is not that request's value") {
        const ParamResult wrong_param = Bk::decode_get_answer(engine, good, good_len, 8, 2);
        CHECK_FALSE(wrong_param.ok);
        CHECK(wrong_param.value == 0u);
        CHECK(wrong_param.reason == omgp::core::kParamFailureNoErrorCode);
        const ParamResult wrong_scope = Bk::decode_get_answer(engine, good, good_len, 7, 3);
        CHECK_FALSE(wrong_scope.ok);
        CHECK(wrong_scope.value == 0u);
    }

    SECTION("a truncated message at every length short of the whole is not ok") {
        for (size_t n = 0; n < good_len; ++n) {
            const ParamResult r = Bk::decode_get_answer(engine, good, n, 7, 2);
            CHECK_FALSE(r.ok);
            CHECK(r.value == 0u);
        }
    }

    SECTION("an unexpected opcode is not ok, and a GET_PARAM response is not a Set acceptance") {
        // The decoders are each scoped to their OWN request's opcode: a Get's answer read as a
        // Set's acceptance, or the reverse, is an answer to a question that was not asked.
        CHECK_FALSE(Bk::decode_set_answer(engine, good, good_len).ok);
        uint8_t empty_set[omgp::LIMIT_max_l3_message] = {};
        size_t empty_len = 0;
        omgp::l3::Header h{};
        h.opcode = omgp::OP_SET_PARAM;
        h.node_id = rig.a_discovered_node();
        h.seq = 2;
        h.flags = omgp::FLAG_response;
        h.payload_len = 0;
        REQUIRE(omgp::l3::encode_header(h, empty_set, sizeof empty_set, empty_len) ==
                omgp::l3::Status::Ok);
        CHECK(Bk::decode_set_answer(engine, empty_set, empty_len).ok);
        CHECK_FALSE(Bk::decode_get_answer(engine, empty_set, empty_len, 7, 2).ok);
    }

    SECTION("a message with the response flag CLEAR is not an answer, whatever its opcode") {
        // protocol-l3 §3: flags bit0 is the response bit, and an answer to a host request IS a
        // response. core/ is the only layer that can test this — L2 is opaque to L3 by the
        // architecture invariant, and l3::decode_message validates length, not direction — so a
        // request's own bytes arriving where an answer belongs must be refused here. Each message
        // below differs from one that decodes to an ok/coded result ONLY in that flags byte.
        uint8_t req_shaped[omgp::LIMIT_max_l3_message] = {};
        size_t req_len = 0;
        {
            omgp::l3::GetParamResp r{};
            r.param_id = 7;
            r.scope = 2;
            r.value = 1234;
            uint8_t payload[omgp::LIMIT_max_l3_payload] = {};
            size_t plen = 0;
            REQUIRE(omgp::l3::encode_get_param_resp(r, payload, sizeof payload, plen) ==
                    omgp::l3::Status::Ok);
            omgp::l3::Header h{};
            h.opcode = omgp::OP_GET_PARAM;
            h.node_id = rig.a_discovered_node();
            h.seq = 1;
            h.flags = 0; // the only difference from `good`, whose result is the control above
            h.payload_len = static_cast<uint8_t>(plen);
            REQUIRE(omgp::l3::encode_header(h, req_shaped, sizeof req_shaped, req_len) ==
                    omgp::l3::Status::Ok);
            for (size_t i = 0; i < plen; ++i) {
                req_shaped[req_len + i] = payload[i];
            }
            req_len += plen;
        }
        const ParamResult got = Bk::decode_get_answer(engine, req_shaped, req_len, 7, 2);
        CHECK_FALSE(got.ok);
        CHECK(got.value == 0u);
        CHECK(got.reason == omgp::core::kParamFailureNoErrorCode);

        // The same question on the Set side: §3.1's empty acceptance payload, flag clear, is not
        // an acceptance — otherwise a flags-cleared SET_PARAM would report a write as accepted.
        uint8_t set_shaped[omgp::LIMIT_max_l3_message] = {};
        size_t set_len = 0;
        omgp::l3::Header hs{};
        hs.opcode = omgp::OP_SET_PARAM;
        hs.node_id = rig.a_discovered_node();
        hs.seq = 2;
        hs.flags = 0;
        hs.payload_len = 0;
        REQUIRE(omgp::l3::encode_header(hs, set_shaped, sizeof set_shaped, set_len) ==
                omgp::l3::Status::Ok);
        const ParamResult set_got = Bk::decode_set_answer(engine, set_shaped, set_len);
        CHECK_FALSE(set_got.ok);
        CHECK(set_got.reason == omgp::core::kParamFailureNoErrorCode);

        // And an ERROR with the flag clear is not a failure REPORT either (§3.1: ERROR is N→H
        // with flags.error set on a response), so its code is not read back as this request's
        // reason — the failure is still reported, with the one core constant instead.
        uint8_t err_shaped[omgp::LIMIT_max_l3_message] = {};
        size_t err_len = 0;
        uint8_t err_payload[omgp::LIMIT_max_l3_payload] = {};
        size_t err_plen = 0;
        omgp::l3::ErrorResp err{};
        err.code = omgp::ERR_BAD_PAYLOAD;
        err.detail.data = nullptr;
        err.detail.len = 0;
        REQUIRE(omgp::l3::encode_error_resp(err, err_payload, sizeof err_payload, err_plen) ==
                omgp::l3::Status::Ok);
        omgp::l3::Header he{};
        he.opcode = omgp::OP_ERROR;
        he.node_id = rig.a_discovered_node();
        he.seq = 3;
        he.flags = omgp::FLAG_error; // an error, but not marked a response
        he.payload_len = static_cast<uint8_t>(err_plen);
        REQUIRE(omgp::l3::encode_header(he, err_shaped, sizeof err_shaped, err_len) ==
                omgp::l3::Status::Ok);
        for (size_t i = 0; i < err_plen; ++i) {
            err_shaped[err_len + i] = err_payload[i];
        }
        err_len += err_plen;
        const ParamResult err_got = Bk::decode_get_answer(engine, err_shaped, err_len, 7, 2);
        CHECK_FALSE(err_got.ok);
        CHECK(err_got.reason == omgp::core::kParamFailureNoErrorCode);
    }

    SECTION("an ERROR answer carries its own code through, and an unallocated one does not") {
        uint8_t msg[omgp::LIMIT_max_l3_message] = {};
        size_t len = 0;
        uint8_t payload[omgp::LIMIT_max_l3_payload] = {};
        size_t plen = 0;
        omgp::l3::ErrorResp err{};
        err.code = omgp::ERR_BAD_PAYLOAD;
        err.detail.data = nullptr;
        err.detail.len = 0;
        REQUIRE(omgp::l3::encode_error_resp(err, payload, sizeof payload, plen) ==
                omgp::l3::Status::Ok);
        omgp::l3::Header h{};
        h.opcode = omgp::OP_ERROR;
        h.node_id = rig.a_discovered_node();
        h.seq = 3;
        h.flags = static_cast<uint8_t>(omgp::FLAG_response | omgp::FLAG_error);
        h.payload_len = static_cast<uint8_t>(plen);
        REQUIRE(omgp::l3::encode_header(h, msg, sizeof msg, len) == omgp::l3::Status::Ok);
        for (size_t i = 0; i < plen; ++i) {
            msg[len + i] = payload[i];
        }
        len += plen;
        const ParamResult reported = Bk::decode_get_answer(engine, msg, len, 7, 2);
        CHECK_FALSE(reported.ok);
        CHECK(reported.reason == omgp::ERR_BAD_PAYLOAD);
        CHECK(Bk::decode_set_answer(engine, msg, len).reason == omgp::ERR_BAD_PAYLOAD);

        // An ERROR whose code this protocol version does not allocate: l3::decode_error_resp
        // refuses it (OutOfRange), so there is no code to report and the failure falls back to
        // the one core constant. Forward compatibility, not a crash (CLAUDE.md rule 7).
        msg[len - plen] = 0x5A; // over the encoded code byte; no code in ERROR_CODES
        const ParamResult unknown = Bk::decode_get_answer(engine, msg, len, 7, 2);
        CHECK_FALSE(unknown.ok);
        CHECK(unknown.reason == omgp::core::kParamFailureNoErrorCode);
    }

    SECTION("arbitrary bytes at every length never decode to an ok result") {
        // A deterministic sweep, not a random one (rule 3's sibling: nothing here reads a clock
        // or a seed). One xorshift sequence over every length a response can have — the fuzz
        // targets in tests/ own the deeper search; this is the engine-level bound.
        uint32_t x = 0x12345678u;
        uint8_t buf[omgp::LIMIT_max_l3_message] = {};
        for (size_t len = 0; len <= sizeof buf; ++len) {
            for (int trial = 0; trial < 8; ++trial) {
                for (size_t i = 0; i < len; ++i) {
                    x ^= x << 13;
                    x ^= x >> 17;
                    x ^= x << 5;
                    buf[i] = static_cast<uint8_t>(x);
                }
                const ParamResult g = Bk::decode_get_answer(engine, buf, len, 7, 2);
                if (g.ok) {
                    // Not a failure in itself — arbitrary bytes CAN form a well-formed GET_PARAM
                    // response for exactly this request, and refusing one would be wrong. What
                    // must hold is that an ok result's value came from the MESSAGE: checked here
                    // with the same l3 codecs, independently of the engine's own call.
                    omgp::l3::Header hdr{};
                    omgp::l3::Bytes pl{};
                    REQUIRE(omgp::l3::decode_message(buf, len, hdr, pl) == omgp::l3::Status::Ok);
                    REQUIRE(hdr.opcode == omgp::OP_GET_PARAM);
                    // ...and that it was a RESPONSE (protocol-l3 §3, flags bit0), not a request's
                    // bytes read as one. Reserved bits are not policed: §4's skip-the-unknown
                    // rule applies to them, so only bit0 is required of an answer.
                    CHECK((hdr.flags & omgp::FLAG_response) != 0u);
                    omgp::l3::GetParamResp r{};
                    REQUIRE(omgp::l3::decode_get_param_resp(pl.data, pl.len, r) ==
                            omgp::l3::Status::Ok);
                    CHECK(r.param_id == 7u);
                    CHECK(r.scope == 2u);
                    CHECK(g.value == r.value);
                }
                const ParamResult s = Bk::decode_set_answer(engine, buf, len);
                // A Set acceptance is an EMPTY SET_PARAM response, which arbitrary bytes can
                // legitimately form — what must never happen is a crash or an over-read, which
                // the sanitizers decide. So this half asserts only that the value is not
                // fabricated (a Set answer carries none).
                CHECK(s.value == 0u);
            }
        }
    }
}

TEST_CASE("CLAUDE.md rule 5: completing and reporting a parameter operation allocates nothing "
          "[params][us2]") {
    // rule 5 across the whole reporting path — the superframes that issue the transaction,
    // decode its answer and enqueue the delivery — in the same shape
    // tests/unit/test_link_master.cpp:149 uses one layer down. Separate from the API-boundary
    // case below, which covers only the two calls.
    Rig rig;
    bring_up_quiet(rig);
    const uint8_t node = rig.a_discovered_node();
    const ParamStep answers[] = {ParamStep{false, 0, omgp::ERR_INTERNAL}, ParamStep{true, 99, 0}};
    rig.arm_params(answers, 2);

    ParamRequestId id = 0;
    REQUIRE(rig.engine().set_param(node, 2, 0, 5) == CoreStatus::Ok);
    REQUIRE(rig.engine().get_param(node, 2, 0, id) == CoreStatus::Ok);
    HEAP_FREE_SCOPE({
        rig.run_until("both operations to be reported", [&] {
            return Bk::results_pending(rig.engine()) == 1u &&
                   Bk::lifecycle_pending(rig.engine(), LifecycleKind::ParamSetFailed) == 1u;
        });
    });
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
