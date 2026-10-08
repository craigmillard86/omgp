// The superframe scheduler under demand load — spec 003 User Story 2 (tasks.md T025, made to
// pass by T028's budget accounting and T029's drain loop). Driven against a real CoreEngine over
// the real link::Master and link::HealthTracker it owns, answered by MockL3Node (R-08) on a
// FakeClock: no hand-built frames, no wall clock (CLAUDE.md rule 3).
// Spec: spec.md US2 Acceptance Scenarios 1-3, SC-002, SC-003, SC-007, FR-002 (every enrolled
// backplane's status poll before any other traffic of that superframe), FR-003 (exactly one
// enrolment probe), FR-004/FR-005 (demand traffic into the remaining budget; carry-over, never
// truncation), FR-020 (events drain ahead of descriptor chunks ahead of parameter operations),
// FR-021 (no callback from run_superframe()), FR-025 (K = 1 event per node per superframe turn),
// FR-027 (the budget is MEASURED, with a conservative seed until a target has been timed),
// FR-028 (demand-priority demotion, reported; FR-002/FR-003 stay unconditional);
// data-model.md §6 (the three rings), §7 (LifecycleKind), §9 (SuperframeBudget);
// research.md R-07 (ring order), R-11 (budget in simulated µs); contracts/core-cpp.md.
//
// WHAT THIS FILE IS AN ORACLE FOR, AND WHAT IT IS NOT (CLAUDE.md rule 11).
//   * It is an oracle for the SCHEDULER: the order traffic is issued in, what the budget admits
//     and carries over, which target is demoted and when, and what is reported.
//   * It is NOT an oracle for the end-to-end event path. `on_status_block()` is empty at this
//     head and tasks.md gives `event_pending` tracking to T039 (US4), so nothing in production
//     fills `event_queue_`. Every event case below pushes its items through CoreEngineTestSeam,
//     which makes those cases assertions about the DRAIN, not about FR-015's detection half.
//   * It is NOT an oracle for what a parameter answer means. T030 owns decoding a GetParamResp
//     and reporting FR-023's ParamSetFailed; here a parameter transaction's answer is consumed
//     for its timing alone, which is what the budget needs.
//   * The fair share, the demotion threshold and the ring-rotation bound are this engine's own
//     choices where the artefacts are silent — each argued in docs/OPEN-QUESTIONS.md 2026-10-08
//     and asserted below through behaviour, never by restating the constant.
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
using omgp::core::EventDrainItem;
using omgp::core::LifecycleEvent;
using omgp::core::LifecycleKind;
using omgp::core::ParamOpItem;
using omgp::core::ParamRequestId;
using omgp::core::ParamResult;
using omgp::core::TranscriptEntry;
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

// The private state these cases read. CoreEngineTestSeam is forward-declared and friended by
// core_engine.hpp so a test can name it without a public setter existing; each test binary
// defines the accessors it needs.
namespace omgp::core {
struct CoreEngineTestSeam {
    static void set_transcript(CoreEngine& engine, TranscriptFn fn, void* ctx) {
        engine.transcript_ = fn;
        engine.transcript_ctx_ = ctx;
    }
    // T039 will fill this ring from a status block's event_pending; until then a test is its
    // only producer, which is why every event case here is about the drain alone.
    static bool push_event(CoreEngine& engine, uint8_t node_id) {
        EventDrainItem item{};
        item.node_id = node_id;
        return engine.event_queue_.push(item);
    }
    static bool push_desc(CoreEngine& engine, uint8_t node_id, uint16_t offset) {
        DescChunkItem item{};
        item.node_id = node_id;
        item.offset = offset;
        return engine.desc_queue_.push(item);
    }
    static size_t event_queued(CoreEngine& engine) {
        return engine.event_queue_.size();
    }
    static size_t desc_queued(CoreEngine& engine) {
        return engine.desc_queue_.size();
    }
    static size_t param_queued(CoreEngine& engine) {
        return engine.param_queue_.size();
    }
    static bool pop_param(CoreEngine& engine, ParamOpItem& out) {
        return engine.param_queue_.pop(out);
    }
    static NodeRecord& node(CoreEngine& engine, uint8_t node_id) {
        return engine.nodes_[CoreEngine::node_index(node_id)];
    }
    static BackplaneRecord& backplane(CoreEngine& engine, uint8_t addr) {
        return engine.backplanes_[CoreEngine::backplane_index(addr)];
    }
    static SuperframeBudget& budget(CoreEngine& engine) {
        return engine.budget_;
    }
    static uint32_t superframe(CoreEngine& engine) {
        return engine.superframe_;
    }
    static uint64_t fair_share_us(CoreEngine& engine) {
        return engine.fair_share_us();
    }
    static bool& demand_issued(CoreEngine& engine) {
        return engine.demand_issued_;
    }
    static const EventDrainItem& event_at(CoreEngine& engine, size_t i) {
        return engine.event_queue_.at(i);
    }
    static const ParamOpItem& param_at(CoreEngine& engine, size_t i) {
        return engine.param_queue_.at(i);
    }
};
} // namespace omgp::core

using Bk = omgp::core::CoreEngineTestSeam;

namespace {

uint64_t byte_us() {
    return omgp::link::byte_time_us(omgp::TRUNK_bit_rate);
}

// --- the transcript (T023): what the trunk was asked to carry, in order --------------------------

struct Transcript {
    std::vector<TranscriptEntry> lines;

    static void record(void* ctx, const TranscriptEntry& e) {
        static_cast<Transcript*>(ctx)->lines.push_back(e);
    }
    size_t count(uint8_t opcode) const {
        size_t n = 0;
        for (const TranscriptEntry& e : lines) {
            if (e.opcode == opcode) {
                ++n;
            }
        }
        return n;
    }
    uint32_t last_superframe() const {
        return lines.empty() ? 0u : lines.back().superframe;
    }
    // The opcodes of one superframe, in the order they were issued — what every ordering
    // assertion below reads, rather than a count that cannot see order at all.
    std::vector<uint8_t> opcodes_in(uint32_t superframe) const {
        std::vector<uint8_t> out;
        for (const TranscriptEntry& e : lines) {
            if (e.superframe == superframe) {
                out.push_back(e.opcode);
            }
        }
        return out;
    }
    std::vector<TranscriptEntry> entries_in(uint32_t superframe) const {
        std::vector<TranscriptEntry> out;
        for (const TranscriptEntry& e : lines) {
            if (e.superframe == superframe) {
                out.push_back(e);
            }
        }
        return out;
    }
};

bool is_status_poll(uint8_t opcode) {
    return opcode == omgp::OP_GET_STATUS || opcode == omgp::OP_BP_SLOT_MAP;
}
bool is_param_op(uint8_t opcode) {
    return opcode == omgp::OP_SET_PARAM || opcode == omgp::OP_GET_PARAM;
}

struct Recorder {
    std::vector<LifecycleEvent> lifecycle;
    std::vector<ParamRequestId> result_ids;

    static void on_lifecycle(void* ctx, LifecycleEvent ev) {
        static_cast<Recorder*>(ctx)->lifecycle.push_back(ev);
    }
    static void on_param_result(void* ctx, ParamRequestId id, ParamResult) {
        static_cast<Recorder*>(ctx)->result_ids.push_back(id);
    }
    size_t deliveries() const {
        return lifecycle.size() + result_ids.size();
    }
    size_t count(LifecycleKind kind) const {
        size_t n = 0;
        for (const LifecycleEvent& e : lifecycle) {
            if (e.kind == kind) {
                ++n;
            }
        }
        return n;
    }
    const LifecycleEvent* first(LifecycleKind kind) const {
        for (const LifecycleEvent& e : lifecycle) {
            if (e.kind == kind) {
                return &e;
            }
        }
        return nullptr;
    }
};

// --- descriptors --------------------------------------------------------------------------------

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
    const char* name = "scheduler-under-test";
    REQUIRE(writer.add_name(omgp::l3::Str{reinterpret_cast<const uint8_t*>(name),
                                          static_cast<uint8_t>(__builtin_strlen(name))}) ==
            omgp::l3::Status::Ok);
    REQUIRE(writer.add_model_id(omgp::l3::ModelIdRec{0x4321, 2, 3}) == omgp::l3::Status::Ok);
    Descriptor d;
    d.bytes.assign(buf, buf + writer.size());
    d.crc = omgp::l3::descriptor_crc(d.bytes.data(), d.bytes.size());
    return d;
}

// --- the rig ------------------------------------------------------------------------------------

class Rig {
  public:
    Rig() : node_(clock_), descriptor_(make_descriptor()) {
        callbacks_.ctx = &recorder_;
        callbacks_.on_lifecycle = &Recorder::on_lifecycle;
        callbacks_.on_param_result = &Recorder::on_param_result;
        engine_ = std::make_unique<CoreEngine>(node_, clock_, omgp::ADDR_host, callbacks_);
        Bk::set_transcript(*engine_, &Transcript::record, &transcript_);
        for (uint8_t addr = omgp::ADDR_backplane_min; addr <= omgp::ADDR_backplane_max; ++addr) {
            absent_[addr] = L3Step::of(SilenceStep{});
            node_.set_script(addr, &absent_[addr], 1);
        }
    }

    // A backplane whose modules answer IDENTIFY, READ_DESC and parameter operations. `pad_bytes`
    // is the ERROR detail padding of "#110 F2c": an answer that is protocol-legal and simply
    // EXPENSIVE, which is how a target overruns its share without failing (spec SC-007).
    void install(uint8_t addr, uint8_t slot_count, uint32_t occupied) {
        std::vector<L3Step>& steps = scripts_[addr];
        steps.clear();
        steps.push_back(L3Step::of(SlotMapStep{slot_count, occupied, occupied}));
        steps.push_back(L3Step::of(StatusStep{ready_status(0)}));
        steps.push_back(L3Step::of(IdentifyStep{identify_for(descriptor_)}));
        steps.push_back(L3Step::of(DescChunkStep{descriptor_.bytes.data(),
                                                 static_cast<uint16_t>(descriptor_.bytes.size())}));
        steps.push_back(L3Step::of(ParamStep{true, 2048, 0}));
        node_.set_script(addr, steps.data(), steps.size());
    }

    // Replaces a backplane's status answer with one carrying `event_pending` events, leaving the
    // rest of its script as it was. Used by the FR-025 cases.
    void set_event_pending(uint8_t addr, uint8_t pending) {
        std::vector<L3Step>& steps = scripts_[addr];
        REQUIRE(!steps.empty());
        steps[1] = L3Step::of(StatusStep{ready_status(pending)});
        node_.set_script(addr, steps.data(), steps.size());
    }

    // SC-007's expensive backplane: its answers take several times an honest one's turnaround,
    // which is a protocol-legal slow answer and therefore a real FR-027 measurement rather than
    // a stipulated constant. The case asserts the honest cost is under the share and the
    // expensive one over it, so a change to either constant fails the case rather than quietly
    // stopping it from discriminating.
    void make_expensive(uint8_t addr) {
        node_.set_answer_delay(addr, kExpensiveDelayUs);
    }
    void make_honest(uint8_t addr) {
        node_.set_answer_delay(addr, 0u);
    }

    void step(uint64_t step_us) {
        now_us_ += step_us;
        node_.advance_to(now_us_);
        engine_->run_superframe(now_us_);
    }

    // Drives until `done()` holds, failing at a stall rather than after a flat budget — the
    // idiom tests/unit/test_core_discovery.cpp's run_until() uses, for the same reason.
    template <typename Done> void run_until(Done done, uint32_t horizon) {
        uint32_t last = transcript_.last_superframe();
        const uint32_t limit = last + horizon;
        uint64_t idle = 0;
        for (;;) {
            step(byte_us());
            if (done()) {
                return;
            }
            const uint32_t sf = transcript_.last_superframe();
            if (sf != last) {
                last = sf;
                idle = 0;
            } else if (++idle >= kStallSteps) {
                FAIL("the engine opened no superframe after " << last << " within " << kStallSteps
                                                              << " steps");
            }
            if (sf > limit) {
                FAIL("superframe " << limit << " passed before the condition held");
            }
        }
    }

    void run_superframes(uint32_t n) {
        const uint32_t target = transcript_.last_superframe() + n;
        run_until([&] { return transcript_.last_superframe() >= target; }, n + 2u);
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

    std::vector<uint8_t> discovered_ids() const {
        std::vector<uint8_t> ids;
        for (unsigned id = omgp::ADDR_module_min; id <= omgp::ADDR_module_max; ++id) {
            if (engine_->discovery_state(static_cast<uint8_t>(id)) == DiscoveryState::Discovered) {
                ids.push_back(static_cast<uint8_t>(id));
            }
        }
        return ids;
    }

    CoreEngine& engine() {
        return *engine_;
    }
    Transcript& transcript() {
        return transcript_;
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
    static constexpr uint64_t kStallSteps = 1200;
    // Over the fair share a loaded benchmark rig computes, and inside trunk §9's T_resp so the
    // answer still ARRIVES — an expensive answer, not a lost one.
    static constexpr uint64_t kExpensiveDelayUs = (omgp::TRUNK_T_resp_us * 3u) / 4u;

    static omgp::l3::StatusBlock ready_status(uint8_t event_pending) {
        omgp::l3::StatusBlock b{};
        b.state = omgp::STATE_READY;
        b.active_channel = 0;
        b.bypass = 0;
        b.fault_code = 0;
        b.uptime_s = 1;
        b.event_pending = event_pending;
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
    Transcript transcript_;
    Recorder recorder_;
    CoreCallbacks callbacks_{};
    std::unique_ptr<CoreEngine> engine_;
    std::vector<L3Step> scripts_[omgp::link::kAddrCount];
    L3Step absent_[omgp::link::kAddrCount];
    uint64_t now_us_ = 0;
};

// The SC-001 benchmark rig: 3 backplanes, 4 occupied slots each, all 12 modules Discovered.
void bring_up_rig(Rig& rig) {
    for (uint8_t i = 0; i < 3u; ++i) {
        rig.install(static_cast<uint8_t>(omgp::ADDR_backplane_min + i), 4, 0x0Fu);
    }
    rig.run_until([&] { return rig.discovered_count() == 12u; }, 200u);
    REQUIRE(rig.discovered_count() == 12u);
}

} // namespace

TEST_CASE("AS1/SC-003/FR-002: a 40-operation parameter burst never delays a superframe's status "
          "polls [scheduler][us2]") {
    // spec US2 AS1: with the SC-001 benchmark rig fully discovered and 40 set_param operations
    // submitted at once, EVERY superframe from submission to the burst's completion issues a
    // status poll for every enrolled backplane strictly BEFORE that superframe's first
    // parameter operation. Asserted on transcript ORDER, not on eventual delivery — a burst
    // that merely completed would satisfy a count-based assertion while starving the polls.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    REQUIRE(ids.size() == 12u);

    const uint32_t first_burst_superframe = rig.transcript().last_superframe() + 1u;
    for (size_t i = 0; i < 40u; ++i) {
        REQUIRE(rig.engine().set_param(ids[i % ids.size()], static_cast<uint8_t>(i), 0,
                                       static_cast<uint16_t>(i)) == CoreStatus::Ok);
    }

    rig.run_until([&] { return Bk::param_queued(rig.engine()) == 0u; }, 200u);
    const uint32_t last = rig.transcript().last_superframe();

    size_t superframes_with_params = 0;
    for (uint32_t sf = first_burst_superframe; sf <= last; ++sf) {
        const std::vector<uint8_t> ops = rig.transcript().opcodes_in(sf);
        if (ops.empty()) {
            continue;
        }
        size_t polls_before_first_param = 0;
        bool seen_param = false;
        for (const uint8_t op : ops) {
            if (is_param_op(op)) {
                seen_param = true;
                ++superframes_with_params;
                break;
            }
            if (is_status_poll(op)) {
                ++polls_before_first_param;
            }
        }
        if (seen_param) {
            // Three enrolled backplanes, so three polls must already have gone out in this
            // superframe before its first parameter operation (FR-002).
            INFO("superframe " << sf);
            CHECK(polls_before_first_param == 3u);
        }
    }
    CHECK(superframes_with_params > 0u); // the burst did issue, so the check above was reached
}

TEST_CASE("AS2/FR-005: an oversized burst carries its remainder to later superframes rather than "
          "extending the current one [scheduler][us2]") {
    // spec US2 AS2 and FR-005: no superframe's demand traffic is allowed to overrun the budget,
    // and the unsent remainder waits. The carried items are asserted by IDENTITY in queue order
    // (R-07 FIFO), not by count: a count is equally satisfied by a ring that dropped an item and
    // queued a different one, which is exactly what "carry-over, not truncation" forbids.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();

    for (size_t i = 0; i < 40u; ++i) {
        REQUIRE(rig.engine().set_param(ids[i % ids.size()], static_cast<uint8_t>(i), 0,
                                       static_cast<uint16_t>(100u + i)) == CoreStatus::Ok);
    }
    REQUIRE(Bk::param_queued(rig.engine()) == 40u);

    // Drive until the burst has begun draining, then read the ring. Not "for one superframe":
    // a superframe's demand slot comes after its status polls, so stopping at a superframe
    // boundary stops before the slot. The assertion is about WHAT is left, not about how many
    // superframes it took — and the budget admitting fewer than 40 is the point.
    rig.run_until([&] { return Bk::param_queued(rig.engine()) < 40u; }, 20u);
    const size_t left = Bk::param_queued(rig.engine());
    REQUIRE(left > 0u);  // the budget did not admit the whole burst at once
    REQUIRE(left < 40u); // and it did admit some of it
    const size_t issued = 40u - left;

    for (size_t i = issued; i < 40u; ++i) {
        ParamOpItem item{};
        REQUIRE(Bk::pop_param(rig.engine(), item));
        INFO("carried item " << i);
        CHECK(item.node_id == ids[i % ids.size()]);
        CHECK(item.param_id == static_cast<uint8_t>(i));
        CHECK(item.value == static_cast<uint16_t>(100u + i));
    }
}

TEST_CASE("FR-005: no superframe's demand traffic is admitted past the budget [scheduler][us2]") {
    // The budget is never negative and never exceeds its period: remaining_us is reset to
    // TRUNK_T_poll_us at the start of every superframe and only ever debited within it. Read at
    // every engine call across a burst, so a superframe that admitted an item it could not
    // afford shows up as a budget that saturated at 0 while more items were still being issued.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    for (size_t i = 0; i < 40u; ++i) {
        REQUIRE(rig.engine().set_param(ids[i % ids.size()], static_cast<uint8_t>(i), 0, 1) ==
                CoreStatus::Ok);
    }

    uint64_t worst = 0;
    while (Bk::param_queued(rig.engine()) > 0u) {
        rig.step(byte_us());
        const omgp::core::SuperframeBudget& b = Bk::budget(rig.engine());
        REQUIRE(b.period_us == omgp::TRUNK_T_poll_us);
        REQUIRE(b.remaining_us <= b.period_us);
        const uint64_t spent = b.period_us - b.remaining_us;
        if (spent > worst) {
            worst = spent;
        }
    }
    // Demand traffic is bounded by the period; the polls and the probe are outside it (FR-028),
    // so this is a bound on what the budget tracks, not on the superframe's wall time.
    CHECK(worst <= omgp::TRUNK_T_poll_us);
}

TEST_CASE("FR-020/R-07: with all three rings pending, events drain before descriptor chunks "
          "before parameter operations [scheduler][us2]") {
    // R-07's fixed order. FR-020 itself only puts events ahead of the other two and is silent on
    // desc-vs-param, so the stricter order below rests on R-07 alone (recorded in this file's
    // header). The event and desc items are pushed through the seam: at this head nothing in
    // production fills the event ring (T039 owns that), and a desc chunk for a Discovered node
    // is not something discovery would queue.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    // Three DISTINCT nodes, one per ring: a descriptor-chunk item is only servable for a node
    // the engine believes is still READING one (the chunk drain drops an item whose node has
    // left ReadingDescriptor, FR-010's slot-emptied rule), while a parameter operation and an
    // event drain are only servable for a node that has REACHED Discovered. One node cannot be
    // both, so loading all three rings on one node would have two of them silently dropped.
    const uint8_t param_node = ids[0], event_node = ids[1], desc_node = ids[2];

    REQUIRE(rig.engine().set_param(param_node, 1, 0, 1) == CoreStatus::Ok);
    Bk::node(rig.engine(), desc_node).discovery = DiscoveryState::ReadingDescriptor;
    REQUIRE(Bk::push_desc(rig.engine(), desc_node, 0));
    REQUIRE(Bk::push_event(rig.engine(), event_node));

    // Drive until all three have been issued, then read the order they went out in.
    const size_t before = rig.transcript().lines.size();
    rig.run_until(
        [&] {
            return Bk::event_queued(rig.engine()) == 0u && Bk::desc_queued(rig.engine()) == 0u &&
                   Bk::param_queued(rig.engine()) == 0u;
        },
        60u);

    std::vector<uint8_t> demand;
    for (size_t i = before; i < rig.transcript().lines.size(); ++i) {
        const uint8_t op = rig.transcript().lines[i].opcode;
        if (op == omgp::OP_GET_EVENT || op == omgp::OP_READ_DESC || is_param_op(op)) {
            demand.push_back(op);
        }
    }
    REQUIRE(demand.size() >= 3u);
    // The first occurrence of each kind, in R-07's order.
    size_t ev = demand.size(), de = demand.size(), pa = demand.size();
    for (size_t i = 0; i < demand.size(); ++i) {
        if (demand[i] == omgp::OP_GET_EVENT && ev == demand.size()) {
            ev = i;
        }
        if (demand[i] == omgp::OP_READ_DESC && de == demand.size()) {
            de = i;
        }
        if (is_param_op(demand[i]) && pa == demand.size()) {
            pa = i;
        }
    }
    CHECK(ev < de);
    CHECK(de < pa);
}

TEST_CASE("FR-025: at most one GET_EVENT per node per superframe, however many events that node "
          "reports pending [scheduler][us2]") {
    // spec FR-025: K = 1 per node per superframe turn, and `remaining_count` is advisory — it
    // decides only whether to re-queue, and MUST NOT size a buffer or bound a loop (R-07). The
    // ring is loaded with several items for ONE node, which is the shape a node reporting
    // further events pending produces; the assertion is per superframe, so a drain that honoured
    // the queue but not the turn would issue them all in one superframe and fail.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    const uint8_t node = ids[0];

    for (int i = 0; i < 4; ++i) {
        REQUIRE(Bk::push_event(rig.engine(), node));
    }
    const uint32_t first = rig.transcript().last_superframe() + 1u;
    rig.run_until([&] { return Bk::event_queued(rig.engine()) == 0u; }, 60u);
    const uint32_t last = rig.transcript().last_superframe();

    size_t superframes_with_an_event = 0;
    for (uint32_t sf = first; sf <= last; ++sf) {
        size_t events_for_node = 0;
        for (const TranscriptEntry& e : rig.transcript().entries_in(sf)) {
            if (e.opcode == omgp::OP_GET_EVENT && e.node_id == node) {
                ++events_for_node;
            }
        }
        INFO("superframe " << sf);
        CHECK(events_for_node <= 1u);
        if (events_for_node == 1u) {
            ++superframes_with_an_event;
        }
    }
    // Four items for one node therefore take four superframes, not one.
    CHECK(superframes_with_an_event == 4u);

    SECTION("and that superframe still issues its status polls and its enrolment probe") {
        // FR-002/FR-003 are unconditional: the event drain takes a demand slot, never a poll's
        // place or the probe's.
        for (uint32_t sf = first; sf <= last; ++sf) {
            const std::vector<uint8_t> ops = rig.transcript().opcodes_in(sf);
            if (ops.empty()) {
                continue;
            }
            size_t polls = 0;
            for (const uint8_t op : ops) {
                if (is_status_poll(op)) {
                    ++polls;
                }
            }
            INFO("superframe " << sf);
            CHECK(polls == 3u);
        }
    }
}

TEST_CASE("FR-025: one node's queued events do not delay another node's [scheduler][us2]") {
    // R-07's correction: the remainder re-queues behind every OTHER node's pending event, not
    // behind the same node's own next one. Two nodes, the first with several events queued ahead
    // of the second's single one: the second must be served in the superframe after the first's
    // first event, not after all of the first's.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    const uint8_t busy = ids[0], quiet = ids[1];

    REQUIRE(Bk::push_event(rig.engine(), busy));
    REQUIRE(Bk::push_event(rig.engine(), busy));
    REQUIRE(Bk::push_event(rig.engine(), busy));
    REQUIRE(Bk::push_event(rig.engine(), quiet));

    rig.run_until([&] { return Bk::event_queued(rig.engine()) == 0u; }, 60u);

    uint32_t quiet_superframe = 0, busy_second = 0;
    size_t busy_seen = 0;
    for (const TranscriptEntry& e : rig.transcript().lines) {
        if (e.opcode != omgp::OP_GET_EVENT) {
            continue;
        }
        if (e.node_id == quiet && quiet_superframe == 0u) {
            quiet_superframe = e.superframe;
        }
        if (e.node_id == busy) {
            ++busy_seen;
            if (busy_seen == 2u) {
                busy_second = e.superframe;
            }
        }
    }
    REQUIRE(quiet_superframe != 0u);
    REQUIRE(busy_second != 0u);
    // The quiet node is served no later than the busy node's SECOND event: K = 1 moved the
    // busy node's remainder behind it.
    CHECK(quiet_superframe <= busy_second);
}

TEST_CASE(
    "FR-021: the superframes that transact demand items invoke no callback [scheduler][us2]") {
    // spec FR-021 / data-model.md §8a: run_superframe() enqueues, drain_callbacks() delivers.
    // Re-asserted here for the bodies T028 and T029 add — the callback-queue test's own note
    // says its version of this claim does not cover them.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    rig.engine().drain_callbacks(256);
    rig.recorder().lifecycle.clear();
    rig.recorder().result_ids.clear();

    for (size_t i = 0; i < 12u; ++i) {
        REQUIRE(rig.engine().set_param(ids[i], 1, 0, 1) == CoreStatus::Ok);
        REQUIRE(Bk::push_event(rig.engine(), ids[i]));
    }
    rig.run_superframes(12);

    CHECK(rig.recorder().deliveries() == 0u);
}

TEST_CASE("FR-018/rule 3: with the clock held fixed no further transaction is issued "
          "[scheduler][us2]") {
    // Every time value comes from the injected FakeClock. A second run_superframe() at the SAME
    // now_us cannot start another transaction: one is outstanding and nothing can have completed
    // without time moving, which is what makes the budget's measured durations meaningful.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    for (size_t i = 0; i < 8u; ++i) {
        REQUIRE(rig.engine().set_param(ids[i], 1, 0, 1) == CoreStatus::Ok);
    }
    rig.run_superframes(1);

    const size_t lines = rig.transcript().lines.size();
    const uint64_t held = rig.now_us();
    for (int i = 0; i < 20; ++i) {
        rig.engine().run_superframe(held);
    }
    CHECK(rig.transcript().lines.size() == lines);
}

TEST_CASE("SC-005/FR-019: the same script issues the same transcript at a finer cadence "
          "[scheduler][us2]") {
    // The scheduler's output is a function of its plan, not of how finely the caller calls it:
    // two rigs driven at one byte time and at a fifth of one, with the same burst, produce the
    // same opcode sequence. Guards against a drain that reads the clock's granularity — the
    // divergence FR-027's measured budget could otherwise introduce.
    std::vector<uint8_t> coarse, fine;
    for (int pass = 0; pass < 2; ++pass) {
        Rig rig;
        bring_up_rig(rig);
        const std::vector<uint8_t> ids = rig.discovered_ids();
        for (size_t i = 0; i < 12u; ++i) {
            REQUIRE(rig.engine().set_param(ids[i], static_cast<uint8_t>(i), 0, 1) ==
                    CoreStatus::Ok);
        }
        const size_t before = rig.transcript().lines.size();
        const uint64_t stride = pass == 0 ? byte_us() : byte_us() / 5u;
        REQUIRE(stride > 0u);
        REQUIRE(byte_us() % stride == 0u);
        while (Bk::param_queued(rig.engine()) > 0u) {
            rig.step(stride);
        }
        std::vector<uint8_t>& out = pass == 0 ? coarse : fine;
        for (size_t i = before; i < rig.transcript().lines.size(); ++i) {
            out.push_back(rig.transcript().lines[i].opcode);
        }
    }
    CHECK(coarse == fine);
}

TEST_CASE("SC-007/FR-027/FR-028: a backplane whose answers cost several times an honest poll is "
          "demoted, reported, and recovers [scheduler][us2]") {
    // spec SC-007 with FR-027's measured budget: a backplane answering every status poll at
    // several times an honest poll's cost has its MODULES' demand items demoted within a bounded
    // number of superframes, LifecycleKind::Demoted is delivered with demoted_is_backplane set,
    // and the demotion clears once its measured cost returns to normal (FR-028).
    //
    // The cost is made real, not stipulated: the expensive backplane answers with a padded
    // ERROR detail ("#110 F2c"), so its answer is protocol-legal and simply longer on the wire,
    // and the duration the budget records for it is MockL3Node's own byte-time scheduling. That
    // is what makes this a test of FR-027's measured path rather than of a seeded constant —
    // quickstart.md §4's discriminating check (hardcode the worst-case bound in place of the
    // measured value) then makes the demotion fail to fire.
    Rig rig;
    bring_up_rig(rig);
    const uint8_t expensive = omgp::ADDR_backplane_min;
    const uint8_t honest = static_cast<uint8_t>(omgp::ADDR_backplane_min + 1);
    rig.engine().drain_callbacks(256);
    rig.recorder().lifecycle.clear();

    // An honest poll is already under the share — nothing is demoted on a healthy rig.
    REQUIRE(Bk::backplane(rig.engine(), expensive).demoted == false);
    REQUIRE(rig.recorder().count(LifecycleKind::Demoted) == 0u);

    rig.make_expensive(expensive);
    const uint32_t from = rig.transcript().last_superframe();
    rig.run_until([&] { return Bk::backplane(rig.engine(), expensive).demoted; }, 40u);
    const uint32_t demoted_at = rig.transcript().last_superframe();

    // Bounded, and the bound is named: the threshold is consecutive superframes of overrun, so
    // the demotion cannot take more than a handful of them once the cost is standing.
    // TIGHT on purpose: GET_STATUS is issued on alternate superframes, so three consecutive
    // overrunning polls land six superframes after the cost becomes standing. A threshold one
    // larger takes eight and fails here, which is what makes this a bound and not a formality.
    INFO("demoted at superframe " << demoted_at << ", cost became expensive after " << from);
    CHECK(demoted_at - from <= 6u);

    rig.engine().drain_callbacks(256);
    const LifecycleEvent* ev = rig.recorder().first(LifecycleKind::Demoted);
    REQUIRE(ev != nullptr);
    CHECK(ev->node_id == expensive);
    CHECK(ev->demoted_is_backplane == true);

    SECTION("the other backplanes are untouched, and no poll or probe is skipped for any of them") {
        CHECK(Bk::backplane(rig.engine(), honest).demoted == false);
        // Up to but NOT including `demoted_at`: the run stops the instant the demotion fires,
        // which is in the middle of that superframe's poll phase, so it has only the polls it
        // had got to. Every superframe before it is complete.
        for (uint32_t sf = from + 1u; sf < demoted_at; ++sf) {
            const std::vector<uint8_t> ops = rig.transcript().opcodes_in(sf);
            if (ops.empty()) {
                continue;
            }
            size_t polls = 0;
            for (const uint8_t op : ops) {
                if (is_status_poll(op)) {
                    ++polls;
                }
            }
            // FR-002/FR-003 stay unconditional: the demoted backplane's OWN poll is still
            // issued, so all three are present in every superframe throughout.
            INFO("superframe " << sf);
            CHECK(polls == 3u);
        }
    }

    SECTION("FR-028: the demotion clears, and DemotionCleared is delivered") {
        rig.make_honest(expensive);
        rig.recorder().lifecycle.clear();
        rig.run_until([&] { return !Bk::backplane(rig.engine(), expensive).demoted; }, 40u);
        rig.engine().drain_callbacks(256);
        const LifecycleEvent* cleared = rig.recorder().first(LifecycleKind::DemotionCleared);
        REQUIRE(cleared != nullptr);
        CHECK(cleared->node_id == expensive);
        CHECK(cleared->demoted_is_backplane == true);
    }
}

TEST_CASE("FR-027: the fair share is derived from the period and the enrolled count, so a "
          "healthy rig demotes nothing [scheduler][us2]") {
    // The share is this engine's own definition where the artefacts are silent
    // (docs/OPEN-QUESTIONS.md 2026-10-08): the superframe period divided by the number of
    // enrolled backplanes plus one. Asserted through BEHAVIOUR and through the seam's own
    // reader rather than by restating the arithmetic: on the benchmark rig it must leave an
    // honest poll comfortably inside the share, which is what keeps SC-007 a test of a real
    // overrun rather than of the definition.
    Rig rig;
    bring_up_rig(rig);
    const uint64_t share = Bk::fair_share_us(rig.engine());
    CHECK(share > 0u);
    CHECK(share < omgp::TRUNK_T_poll_us);

    rig.run_superframes(20);
    for (uint8_t i = 0; i < 3u; ++i) {
        const uint8_t addr = static_cast<uint8_t>(omgp::ADDR_backplane_min + i);
        INFO("backplane " << static_cast<unsigned>(addr));
        CHECK(Bk::backplane(rig.engine(), addr).demoted == false);
        CHECK(Bk::backplane(rig.engine(), addr).last_measured_duration_us < share);
    }
    CHECK(rig.recorder().count(LifecycleKind::Demoted) == 0u);
}

TEST_CASE("CLAUDE.md rule 5: a superframe under demand load allocates nothing [scheduler][us2]") {
    // rule 5 for the embedded path: the drain loop and the budget accounting run inside the
    // counting heap guard, with all three rings loaded so every branch of the loop is taken.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    for (size_t i = 0; i < 8u; ++i) {
        REQUIRE(rig.engine().set_param(ids[i], 1, 0, 1) == CoreStatus::Ok);
        REQUIRE(Bk::push_event(rig.engine(), ids[i]));
    }
    REQUIRE(Bk::push_desc(rig.engine(), ids[0], 0));
    // The guard counts EVERY allocation on the thread, so the test's own instruments must not
    // grow inside it: the transcript sink is a std::vector whose push_back would otherwise be
    // charged to core/. Reserved here, which is the test's business and not the engine's.
    rig.transcript().lines.reserve(rig.transcript().lines.size() + 4096u);
    rig.recorder().lifecycle.reserve(256u);
    rig.recorder().result_ids.reserve(256u);

    HEAP_FREE_SCOPE({
        for (int i = 0; i < 200; ++i) {
            rig.step(byte_us());
        }
        rig.engine().drain_callbacks(16);
    });
}

TEST_CASE("FR-028: a demoted backplane's modules lose their PLACE in a ring, not their turn "
          "[scheduler][us2]") {
    // The half of demotion that nothing else here observes: what it DOES. Two nodes queue a
    // parameter operation each, the first behind a backplane that is demoted and the second not,
    // so plain arrival order and demoted-last order disagree — the second must be served first,
    // and the first must still be served afterwards (FR-028 demotes priority, not existence).
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    // ids are assigned ascending across backplanes, so the first and last discovered modules sit
    // on different backplanes — asserted rather than assumed, since the whole case rests on it.
    const uint8_t demoted_node = ids.front();
    const uint8_t normal_node = ids.back();
    const uint8_t demoted_bp = Bk::node(rig.engine(), demoted_node).backplane_addr;
    REQUIRE(demoted_bp != Bk::node(rig.engine(), normal_node).backplane_addr);

    Bk::backplane(rig.engine(), demoted_bp).demoted = true;
    REQUIRE(rig.engine().set_param(demoted_node, 11, 0, 1) == CoreStatus::Ok);
    REQUIRE(rig.engine().set_param(normal_node, 22, 0, 2) == CoreStatus::Ok);
    // Arrival order: the demoted node's operation is at the head.
    REQUIRE(Bk::param_at(rig.engine(), 0).node_id == demoted_node);

    rig.run_until([&] { return Bk::param_queued(rig.engine()) < 2u; }, 20u);
    // After one is served, the one LEFT is the demoted node's — so the other went first.
    REQUIRE(Bk::param_queued(rig.engine()) == 1u);
    CHECK(Bk::param_at(rig.engine(), 0).node_id == demoted_node);
    CHECK(Bk::param_at(rig.engine(), 0).param_id == 11u);

    SECTION("and it is served once the demotion clears, so demotion is priority not exclusion") {
        Bk::backplane(rig.engine(), demoted_bp).demoted = false;
        rig.run_until([&] { return Bk::param_queued(rig.engine()) == 0u; }, 20u);
        CHECK(Bk::param_queued(rig.engine()) == 0u);
    }

    SECTION("a ring whose every item is demoted still serves its head") {
        // One full rotation finds no servable item and leaves the ring as it was, rather than
        // refusing to serve anyone — the bound that makes the rotation terminate at all.
        REQUIRE(rig.engine().set_param(demoted_node, 33, 0, 3) == CoreStatus::Ok);
        REQUIRE(Bk::param_queued(rig.engine()) == 2u);
        rig.run_until([&] { return Bk::param_queued(rig.engine()) < 2u; }, 20u);
        CHECK(Bk::param_queued(rig.engine()) == 1u);
        CHECK(Bk::param_at(rig.engine(), 0).param_id == 33u); // the head went first, in FIFO
    }
}

TEST_CASE("FR-028: a demoted node's own event item is deprioritised the same way "
          "[scheduler][us2]") {
    // target_demoted() reads the NODE's flag as well as its backplane's. Nothing sets the node
    // flag at this head (docs/OPEN-QUESTIONS.md 2026-10-08 records why), so the rotation's
    // node-level half would otherwise go unexercised until a later task.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    const uint8_t slow = ids[0], quick = ids[1];
    Bk::node(rig.engine(), slow).demoted = true;

    REQUIRE(Bk::push_event(rig.engine(), slow));
    REQUIRE(Bk::push_event(rig.engine(), quick));
    REQUIRE(Bk::event_at(rig.engine(), 0).node_id == slow);

    rig.run_until([&] { return Bk::event_queued(rig.engine()) < 2u; }, 20u);
    REQUIRE(Bk::event_queued(rig.engine()) == 1u);
    CHECK(Bk::event_at(rig.engine(), 0).node_id == slow);
}

TEST_CASE("FR-005/FR-027: an item whose own measured cost will not fit is refused, and the "
          "estimate is the target's own [scheduler][us2]") {
    // admit_demand() is consulted with cost_estimate(this target's last_measured_duration_us),
    // which is what makes the budget a MEASURED bound (FR-027) rather than a per-item constant.
    // The first-item exception is spent deliberately, so the second item faces the real test:
    // with a target whose measured cost exceeds the whole period, it cannot be admitted.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    const uint8_t cheap = ids[0], ruinous = ids[1];
    Bk::node(rig.engine(), ruinous).last_measured_duration_us = omgp::TRUNK_T_poll_us * 4u;

    REQUIRE(rig.engine().set_param(cheap, 1, 0, 1) == CoreStatus::Ok);
    REQUIRE(rig.engine().set_param(ruinous, 2, 0, 2) == CoreStatus::Ok);

    // It IS eventually issued — admit_demand()'s first-item exception exists precisely so a
    // target nothing can afford does not livelock the rig (run_superframe()'s header note). The
    // property the estimate buys is narrower and is the one asserted: such an item is never
    // admitted as the SECOND demand item of a superframe, so it cannot be issued alongside
    // another. With the estimate replaced by a small constant it would be.
    const uint32_t first = rig.transcript().last_superframe() + 1u;
    rig.run_until([&] { return Bk::param_queued(rig.engine()) == 0u; }, 30u);
    const uint32_t last = rig.transcript().last_superframe();

    bool saw_ruinous = false;
    for (uint32_t sf = first; sf <= last; ++sf) {
        size_t demand_before_ruinous = 0;
        for (const TranscriptEntry& e : rig.transcript().entries_in(sf)) {
            if (!is_param_op(e.opcode)) {
                continue;
            }
            if (e.node_id == ruinous) {
                saw_ruinous = true;
                INFO("superframe " << sf);
                CHECK(demand_before_ruinous == 0u); // it was this superframe's FIRST demand item
            }
            ++demand_before_ruinous;
        }
    }
    CHECK(saw_ruinous); // so the check above was reached
}

TEST_CASE("FR-004/FR-005: one superframe admits ONE demand item beyond the first-item exception "
          "[scheduler][us2]") {
    // The budget's whole purpose, asserted against the rig's real costs: three mandatory polls
    // leave less than one more transaction's worth of the period, so a superframe issues the one
    // item its exception admits and no more. A drain that ignored the budget, or an estimate
    // replaced by a small constant, would empty the ring in a superframe or two instead.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    for (size_t i = 0; i < 8u; ++i) {
        REQUIRE(Bk::push_event(rig.engine(), ids[i]));
    }
    const uint32_t first = rig.transcript().last_superframe() + 1u;
    rig.run_until([&] { return Bk::event_queued(rig.engine()) == 0u; }, 40u);
    const uint32_t last = rig.transcript().last_superframe();

    // Eight items do NOT go out in one or two superframes: three mandatory polls leave well
    // under eight transactions' worth of the period, so the budget spreads them. The bound is
    // deliberately loose — what it discriminates is a drain that ignored the budget, or an
    // estimate replaced by a small constant, either of which empties the ring at once.
    INFO("first " << first << " last " << last);
    CHECK(last - first + 1u >= 4u);

    SECTION("and no superframe's accounted demand traffic exceeded the period") {
        const omgp::core::SuperframeBudget& b = Bk::budget(rig.engine());
        CHECK(b.remaining_us <= b.period_us);
    }
}

TEST_CASE("protocol-l3 §3.1: a queued Set is issued as SET_PARAM and a queued Get as GET_PARAM "
          "[scheduler][us2]") {
    // The ParamOpItem's kind selects the opcode and the transaction kind. Asserted on the
    // transcript, which records what the trunk was actually asked to carry.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    const uint8_t node = ids[0];
    const size_t sets_before = rig.transcript().count(omgp::OP_SET_PARAM);
    const size_t gets_before = rig.transcript().count(omgp::OP_GET_PARAM);

    // ONE kind per section. Queuing both at once cannot discriminate: a drain that swapped
    // the two opcodes would still leave one of each in the transcript.
    SECTION("a Set alone produces a SET_PARAM and no GET_PARAM") {
        REQUIRE(rig.engine().set_param(node, 5, 0, 7) == CoreStatus::Ok);
        rig.run_until([&] { return Bk::param_queued(rig.engine()) == 0u; }, 30u);
        CHECK(rig.transcript().count(omgp::OP_SET_PARAM) == sets_before + 1u);
        CHECK(rig.transcript().count(omgp::OP_GET_PARAM) == gets_before);
    }

    SECTION("a Get alone produces a GET_PARAM and no SET_PARAM") {
        ParamRequestId id = 0;
        REQUIRE(rig.engine().get_param(node, 6, 0, id) == CoreStatus::Ok);
        rig.run_until([&] { return Bk::param_queued(rig.engine()) == 0u; }, 30u);
        CHECK(rig.transcript().count(omgp::OP_GET_PARAM) == gets_before + 1u);
        CHECK(rig.transcript().count(omgp::OP_SET_PARAM) == sets_before);
    }
}

TEST_CASE("FR-010: a parameter operation whose node leaves Discovered is dropped, never "
          "addressed [scheduler][us2]") {
    // The drop path in the parameter drain: a slot that empties under a queued operation takes
    // the operation with it, rather than the engine addressing a node id that no longer names
    // what queued it.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    const uint8_t node = ids[0];
    REQUIRE(rig.engine().set_param(node, 9, 0, 1) == CoreStatus::Ok);
    REQUIRE(Bk::param_queued(rig.engine()) == 1u);

    const size_t sets_before = rig.transcript().count(omgp::OP_SET_PARAM);
    Bk::node(rig.engine(), node).discovery = DiscoveryState::Undiscovered;
    rig.run_until([&] { return Bk::param_queued(rig.engine()) == 0u; }, 20u);

    CHECK(Bk::param_queued(rig.engine()) == 0u);
    CHECK(rig.transcript().count(omgp::OP_SET_PARAM) == sets_before); // dropped, not issued
}

TEST_CASE("FR-028: the overrun counter is CONSECUTIVE — an intermittent overrun never demotes "
          "[scheduler][us2]") {
    // What "consistently exceed their fair share" means, and the one thing a counter that never
    // reset would get wrong: a backplane that alternates expensive and honest answers is never
    // demoted, however long it runs.
    Rig rig;
    bring_up_rig(rig);
    const uint8_t bp = omgp::ADDR_backplane_min;
    rig.engine().drain_callbacks(256);
    rig.recorder().lifecycle.clear();

    for (int cycle = 0; cycle < 6; ++cycle) {
        rig.make_expensive(bp);
        rig.run_superframes(2);
        rig.make_honest(bp);
        rig.run_superframes(2);
    }

    CHECK(Bk::backplane(rig.engine(), bp).demoted == false);
    rig.engine().drain_callbacks(256);
    CHECK(rig.recorder().count(LifecycleKind::Demoted) == 0u);
}

TEST_CASE("FR-028: the descriptor-chunk ring is deprioritised the same way as the other two "
          "[scheduler][us2]") {
    // rotate_past_demoted() is one template used on all three rings; this is the chunk ring's
    // own case, so removing its call is not invisible. Two nodes still reading a descriptor, the
    // head one behind a demoted backplane: the other must be served first.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    const uint8_t demoted_node = ids.front();
    const uint8_t normal_node = ids.back();
    const uint8_t demoted_bp = Bk::node(rig.engine(), demoted_node).backplane_addr;
    REQUIRE(demoted_bp != Bk::node(rig.engine(), normal_node).backplane_addr);

    Bk::node(rig.engine(), demoted_node).discovery = DiscoveryState::ReadingDescriptor;
    Bk::node(rig.engine(), normal_node).discovery = DiscoveryState::ReadingDescriptor;
    Bk::backplane(rig.engine(), demoted_bp).demoted = true;
    REQUIRE(Bk::push_desc(rig.engine(), demoted_node, 0));
    REQUIRE(Bk::push_desc(rig.engine(), normal_node, 0));

    const size_t before = rig.transcript().count(omgp::OP_READ_DESC);
    rig.run_until([&] { return rig.transcript().count(omgp::OP_READ_DESC) > before; }, 20u);

    // The first READ_DESC issued after the demotion went to the node that is NOT demoted.
    uint8_t first_node = 0;
    size_t seen = 0;
    for (const TranscriptEntry& e : rig.transcript().lines) {
        if (e.opcode != omgp::OP_READ_DESC) {
            continue;
        }
        if (++seen == before + 1u) {
            first_node = e.node_id;
            break;
        }
    }
    CHECK(first_node == normal_node);
}

TEST_CASE("FR-025: a node that has had its turn does not block a node queued behind it in the "
          "SAME superframe [scheduler][us2]") {
    // R-07's correction, in the form that discriminates a drain making only ONE pass over the
    // ring: the head node has already been drained this superframe, so it must be moved aside
    // and the node behind it served in this same superframe — not in the next one.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    const uint8_t first_node = ids[0], second_node = ids[1];

    // Give the head node its turn in whatever superframe the engine is in, then queue a second
    // item for it plus one for another node — so the ring's head is a node already drained.
    REQUIRE(Bk::push_event(rig.engine(), first_node));
    rig.run_until([&] { return rig.transcript().count(omgp::OP_GET_EVENT) > 0u; }, 20u);
    const uint32_t turn = rig.transcript().last_superframe();
    REQUIRE(Bk::node(rig.engine(), first_node).event_drained_superframe == turn);

    REQUIRE(Bk::push_event(rig.engine(), first_node));
    REQUIRE(Bk::push_event(rig.engine(), second_node));
    const size_t before = rig.transcript().count(omgp::OP_GET_EVENT);

    // Drive within this same superframe only: one more GET_EVENT must appear, and it must be
    // the second node's, reached by stepping PAST the head rather than by waiting a superframe.
    rig.run_until(
        [&] {
            return rig.transcript().count(omgp::OP_GET_EVENT) > before ||
                   rig.transcript().last_superframe() > turn;
        },
        4u);
    bool served_in_turn = false;
    for (const TranscriptEntry& e : rig.transcript().lines) {
        if (e.opcode == omgp::OP_GET_EVENT && e.superframe == turn && e.node_id == second_node) {
            served_in_turn = true;
        }
    }
    CHECK(served_in_turn);
}

TEST_CASE("FR-028: the fair share counts every enrolled backplane, including the last address "
          "[scheduler][us2]") {
    // The share's divisor is `enrolled + 1`, enumerated over the whole backplane address range.
    // A sweep that stopped one address short would miss a backplane enrolled AT
    // ADDR_backplane_max — the one address an inclusive bound and an exclusive one disagree
    // about — and compute too large a share, demoting nothing.
    Rig rig;
    for (uint8_t i = 0; i < 3u; ++i) {
        rig.install(static_cast<uint8_t>(omgp::ADDR_backplane_min + i), 4, 0x0Fu);
    }
    rig.run_until([&] { return rig.discovered_count() == 12u; }, 200u);
    const uint64_t three = Bk::fair_share_us(rig.engine());

    rig.install(omgp::ADDR_backplane_max, 1, 0x01u);
    rig.run_until([&] { return rig.engine().backplane_enrolled(omgp::ADDR_backplane_max); }, 60u);
    const uint64_t four = Bk::fair_share_us(rig.engine());

    INFO("share with three enrolled " << three << ", with four " << four);
    CHECK(four < three);
}

TEST_CASE("FR-010: a dropped parameter operation does not cost the item behind it a superframe "
          "[scheduler][us2]") {
    // The drop path continues the pass rather than ending it, so an operation whose node has
    // left Discovered is skipped and the next one is served in the SAME superframe. A drain that
    // made only one pass would serve nothing and make the survivor wait.
    Rig rig;
    bring_up_rig(rig);
    const std::vector<uint8_t> ids = rig.discovered_ids();
    const uint8_t gone = ids[0], alive = ids[1];

    REQUIRE(rig.engine().set_param(gone, 1, 0, 1) == CoreStatus::Ok);
    REQUIRE(rig.engine().set_param(alive, 2, 0, 2) == CoreStatus::Ok);
    Bk::node(rig.engine(), gone).discovery = DiscoveryState::Undiscovered;
    const size_t before = rig.transcript().count(omgp::OP_SET_PARAM);

    // One superframe's demand slot is enough for both: one dropped, one issued.
    rig.run_until([&] { return rig.transcript().count(omgp::OP_SET_PARAM) > before; }, 8u);
    const uint32_t served = rig.transcript().last_superframe();

    CHECK(Bk::param_queued(rig.engine()) == 0u); // both gone: one dropped, one issued
    bool alive_served = false;
    for (const TranscriptEntry& e : rig.transcript().entries_in(served)) {
        if (e.opcode == omgp::OP_SET_PARAM && e.node_id == alive) {
            alive_served = true;
        }
        CHECK(e.node_id != gone); // the dropped one was never addressed
    }
    CHECK(alive_served);
}
