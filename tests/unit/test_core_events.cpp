// A module event reaches the application within a bounded time — spec 003 User Story 4
// (tasks.md T038, made to pass by T039). Driven against a real CoreEngine over the real
// link::Master and link::HealthTracker it owns, answered by MockL3Node (research.md R-08) on a
// FakeClock: no hand-built frames, no wall clock (CLAUDE.md rule 3).
// trunk §6: the superframe whose status polls collect event_pending and whose demand slots carry
// the drains; trunk §7's retry/timeout rules stay inside link::Master, which this file never
// reaches past (core/ sees no frame byte).
// protocol-l3 §3.1 (GET_EVENT 0x15 — `u8 event_type, u8 remaining_count, u8[] detail`), §3.3
// (the status block's `event_pending` count), §3.4 (v1 event types, and NONE as the answer to a
// GET_EVENT on an empty queue), §4.2 (an answer this host cannot read is ignored, never fatal —
// CLAUDE.md rule 7).
// Spec: spec.md US4 Acceptance Scenarios 1-3, FR-015 (drain across superframes), FR-016 (every
// drained event delivered, in the module's own order), FR-020 (events ahead of the other demand
// kinds), FR-021 (no callback from run_superframe()), FR-025 (K = 1 per node per superframe turn;
// remaining_count advisory), FR-026 (the per-node event-rate budget, the fault and its clear),
// SC-002 (the latency bound), SC-006 (a flooding node starves nobody); data-model.md §3
// (NodeRecord's event-rate fields), §6 (EventDrainItem), §7 (LifecycleKind::ModuleEvent /
// NodeEventFault / NodeEventFaultCleared), §8a (the pending-delivery rings); research.md R-07 and
// its 2026-09-21 correction; docs/OPEN-QUESTIONS.md 2026-09-06 "#110 F6" and 2026-11-23.
//
// WHAT THIS FILE IS AN ORACLE FOR, AND WHAT IT IS NOT (CLAUDE.md rule 11).
//   * It is an oracle for the END-TO-END event path at message level: a status poll's own
//     event_pending count, the GET_EVENT it leads to, the decoded GetEventResp, the
//     LifecycleKind::ModuleEvent delivered by drain_callbacks(), and the FR-026 flood defence.
//     Every event here is detected by production code from a scripted status block — nothing is
//     pushed onto the event ring through a seam, which is what makes these cases FR-015's oracle
//     and not only the drain loop's (tests/unit/test_core_scheduler.cpp owns that half).
//   * The SC-002 bound asserted here is MEASURED IN SIMULATED TIME from the instant the events
//     are armed on the module, against the bound SC-002 itself names — that node's backplane's
//     status-poll period (two superframes, since trunk §6 alternates GET_STATUS with BP_SLOT_MAP)
//     plus two superframe periods. It is computed from TRUNK_T_poll_us in each case, never
//     written out as a number.
//   * It is NOT a rig-level oracle. The scenario runner, VirtualTransport and the virtual rig are
//     F4's and do not exist (sim/, cli/, transport/ are empty); MockL3Node is a message-level
//     double, not a rig substitute (R-08), and rig-level evidence for this behaviour is owed when
//     F4 lands.
//   * It is NOT an oracle for CHANNEL_SETTLED's meaning. protocol-l3 §3.4's `(channel, seq)`
//     detail layout is ruled (#110 F9) but absent from protocol/omgp-protocol.yaml, so this file
//     keeps detail OPAQUE: a CHANNEL_SETTLED event here is asserted only as a ModuleEvent whose
//     detail bytes arrive unchanged. No node here has a switch outstanding, so T036's later
//     special case cannot change these expectations.
//   * It is NOT an oracle for an unallocated event code reaching the application:
//     l3::decode_get_event_resp refuses an event_type outside §3.4's own set, so such an answer
//     is IGNORED one layer below core/ (asserted directly, and recorded in
//     docs/OPEN-QUESTIONS.md 2026-11-23).
//   * The SC-002 cases below script FEWER events than kEventRateBudgetPerWindow, and that is a
//     LIMIT on what they establish, not a calibration: measured at this head, K=1 paces a node at
//     one event per ~1900 us, so the fifth event of any backlog is past SC-002's 8000 us bound by
//     FR-025 alone and FR-026's budget then reports the node faulted. So this file is an oracle
//     for SC-002 on backlogs up to the budget ONLY; beyond it the behaviour is the collision
//     recorded in docs/OPEN-QUESTIONS.md 2026-11-23 ("FR-025's K=1, SC-002's bound and FR-026's
//     rate budget cannot all hold ..."), with the measured run, awaiting a ruling. No case here
//     asserts the breach as expected behaviour.
//   * The event-rate window and budget are core/core_types.hpp constants, read from there by
//     every case below and never restated as literals — the engine's own policy where FR-026 says
//     only "a configured budget".
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
#include <cstring>
#include <memory>
#include <vector>

using omgp::core::CoreCallbacks;
using omgp::core::CoreEngine;
using omgp::core::CoreStatus;
using omgp::core::DiscoveryState;
using omgp::core::kEventRateBudgetPerWindow;
using omgp::core::kEventRateWindowUs;
using omgp::core::kMaxEventDetailBytes;
using omgp::core::LifecycleEvent;
using omgp::core::LifecycleKind;
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
// defines the accessors it needs. NOTHING here pushes onto a demand ring: this file's subject is
// the production path from a status block to a delivered event.
namespace omgp::core {
struct CoreEngineTestSeam {
    static void set_transcript(CoreEngine& engine, TranscriptFn fn, void* ctx) {
        engine.transcript_ = fn;
        engine.transcript_ctx_ = ctx;
    }
    static size_t event_queued(CoreEngine& engine) {
        return engine.event_queue_.size();
    }
    static NodeRecord& node(CoreEngine& engine, uint8_t node_id) {
        return engine.nodes_[CoreEngine::node_index(node_id)];
    }
    static uint32_t superframe(CoreEngine& engine) {
        return engine.superframe_;
    }
    // --- white-box access for the mutation-kill cases at the end of this file ------------------
    // These call the engine's own private steps directly. They add no behaviour: a friend
    // declaration emits no code (core_engine.hpp), and each accessor only forwards.
    static BackplaneRecord& backplane(CoreEngine& engine, uint8_t addr) {
        return engine.backplanes_[CoreEngine::backplane_index(addr)];
    }
    static void sweep(CoreEngine& engine, uint8_t addr) {
        engine.sweep_one_module_for_events(backplane(engine, addr), addr);
    }
    // `speculative` defaults to false: every case that uses this seam is about a drain of an
    // event the host has been TOLD about, which is the kind EventDrainItem::speculative is
    // false for. The sweep's own probes are queued by production code, never here.
    static void queue_drain(CoreEngine& engine, uint8_t node_id, bool speculative = false) {
        engine.queue_event_drain(node(engine, node_id), node_id, speculative);
    }
    static uint8_t queue_head(CoreEngine& engine) {
        return engine.event_queue_.at(0).node_id;
    }
    static void drain_events(CoreEngine& engine, uint64_t now_us) {
        (void)engine.drain_events(now_us);
    }
    static void note_drained(CoreEngine& engine, uint8_t node_id, uint64_t now_us) {
        engine.note_event_drained(node(engine, node_id), node_id, now_us);
    }
    static void roll(CoreEngine& engine, uint8_t node_id, uint64_t now_us) {
        engine.roll_event_window(node(engine, node_id), node_id, now_us);
    }
    static void tick(CoreEngine& engine, uint64_t now_us) {
        engine.tick_event_windows(now_us);
    }
    static void set_tx(CoreEngine& engine, uint8_t addr, uint8_t node_id) {
        engine.tx_addr_ = addr;
        engine.tx_node_ = node_id;
    }
    static void on_status(CoreEngine& engine, const link::MasterEvent& ev, uint64_t now_us) {
        engine.on_status_block(ev, now_us);
    }
    static void on_event_drain(CoreEngine& engine, const link::MasterEvent& ev, uint64_t now_us) {
        engine.on_event_drain(ev, now_us);
    }
    static bool enqueue(CoreEngine& engine, const LifecycleEvent& ev) {
        return engine.enqueue_lifecycle(ev);
    }
    static size_t arena_next(CoreEngine& engine) {
        return engine.event_detail_next_;
    }
    static void set_arena_next(CoreEngine& engine, size_t row) {
        engine.event_detail_next_ = row;
    }
    static uint8_t arena_byte(CoreEngine& engine, size_t row, size_t i) {
        return engine.event_detail_[row][i];
    }
};
} // namespace omgp::core

using Bk = omgp::core::CoreEngineTestSeam;

namespace {

uint64_t byte_us() {
    return omgp::link::byte_time_us(omgp::TRUNK_bit_rate);
}

// SC-002's bound, computed rather than written down (CLAUDE.md rule 4): "that module's
// backplane's status-poll period plus two superframe periods". A healthy enrolled backplane is
// poll_due() every superframe (link/health.cpp) and trunk §6 alternates GET_STATUS with
// BP_SLOT_MAP, so its STATUS-poll period is two superframes; a superframe is TRUNK_T_poll_us
// (spec FR-001). Hence four periods. A plan that is spent early opens the next superframe early
// (core_engine.hpp's own note), so this is an upper bound on simulated time, which is exactly
// what SC-002 asserts.
uint64_t sc002_bound_us() {
    return 4u * static_cast<uint64_t>(omgp::TRUNK_T_poll_us);
}

// --- the transcript (T023), stamped with the simulated instant each request went out ----------

struct Line {
    TranscriptEntry entry;
    uint64_t at_us;
};

struct Transcript {
    std::vector<Line> lines;
    const uint64_t* now = nullptr; // the rig's own simulated clock; set at construction

    static void record(void* ctx, const TranscriptEntry& e) {
        Transcript* self = static_cast<Transcript*>(ctx);
        self->lines.push_back(Line{e, self->now == nullptr ? 0u : *self->now});
    }
    size_t count(uint8_t opcode) const {
        size_t n = 0;
        for (const Line& l : lines) {
            if (l.entry.opcode == opcode) {
                ++n;
            }
        }
        return n;
    }
    size_t count_for(uint8_t opcode, uint8_t node_id) const {
        size_t n = 0;
        for (const Line& l : lines) {
            if (l.entry.opcode == opcode && l.entry.node_id == node_id) {
                ++n;
            }
        }
        return n;
    }
    // The superframes in which `opcode` was issued to `node_id`, in order.
    std::vector<uint32_t> superframes_for(uint8_t opcode, uint8_t node_id) const {
        std::vector<uint32_t> out;
        for (const Line& l : lines) {
            if (l.entry.opcode == opcode && l.entry.node_id == node_id) {
                out.push_back(l.entry.superframe);
            }
        }
        return out;
    }
    size_t count_between(uint8_t opcode, uint8_t node_id, uint64_t from_us, uint64_t to_us) const {
        size_t n = 0;
        for (const Line& l : lines) {
            if (l.entry.opcode == opcode && l.entry.node_id == node_id && l.at_us >= from_us &&
                l.at_us <= to_us) {
                ++n;
            }
        }
        return n;
    }
    uint32_t last_superframe() const {
        return lines.empty() ? 0u : lines.back().entry.superframe;
    }
};

// --- deliveries (data-model.md §8a: only drain_callbacks() ever calls back) --------------------

// A delivered LifecycleEvent, the simulated instant it was delivered at, and a COPY of its detail
// bytes taken at that instant. The copy is a fixed array, not a container, so a delivery costs no
// allocation and the rule-5 case below can run inside HEAP_FREE_SCOPE.
struct Delivery {
    LifecycleEvent ev{};
    uint64_t at_us = 0;
    uint8_t detail[kMaxEventDetailBytes] = {};
    uint8_t detail_len = 0;
};

struct Recorder {
    std::vector<Delivery> deliveries;
    std::vector<ParamRequestId> result_ids;
    const uint64_t* now = nullptr;

    static void on_lifecycle(void* ctx, LifecycleEvent ev) {
        Recorder* self = static_cast<Recorder*>(ctx);
        Delivery d{};
        d.ev = ev;
        d.at_us = self->now == nullptr ? 0u : *self->now;
        if (ev.module_event_detail != nullptr) {
            d.detail_len = ev.module_event_detail_len;
            for (uint8_t i = 0; i < d.detail_len; ++i) {
                d.detail[i] = ev.module_event_detail[i];
            }
        }
        self->deliveries.push_back(d);
    }
    static void on_param_result(void* ctx, ParamRequestId id, ParamResult) {
        static_cast<Recorder*>(ctx)->result_ids.push_back(id);
    }
    size_t count(LifecycleKind kind) const {
        size_t n = 0;
        for (const Delivery& d : deliveries) {
            if (d.ev.kind == kind) {
                ++n;
            }
        }
        return n;
    }
    size_t count_for(LifecycleKind kind, uint8_t node_id) const {
        size_t n = 0;
        for (const Delivery& d : deliveries) {
            if (d.ev.kind == kind && d.ev.node_id == node_id) {
                ++n;
            }
        }
        return n;
    }
    std::vector<Delivery> events_of(uint8_t node_id) const {
        std::vector<Delivery> out;
        for (const Delivery& d : deliveries) {
            if (d.ev.kind == LifecycleKind::ModuleEvent && d.ev.node_id == node_id) {
                out.push_back(d);
            }
        }
        return out;
    }
    // BY VALUE, deliberately: a pointer into `deliveries` dangles the moment a later step
    // delivers something and the vector grows, and every case that reads a delivery goes on
    // running the rig afterwards (ASan caught exactly that while this file was being written).
    Delivery first(LifecycleKind kind) const {
        for (const Delivery& d : deliveries) {
            if (d.ev.kind == kind) {
                return d;
            }
        }
        FAIL("no delivery of the requested kind");
        return Delivery{};
    }
};

// --- scripted events --------------------------------------------------------------------------

// One scripted module event: protocol-l3 §3.1's three fields, with the detail bytes owned by the
// test (MockL3Node::set_script copies them into its own queue).
struct ScriptedEvent {
    uint8_t event_type = omgp::EVT_FAULT_RAISED;
    uint8_t remaining_count = 0;
    std::vector<uint8_t> detail;
};

ScriptedEvent event_of(uint8_t type, uint8_t remaining, std::vector<uint8_t> detail) {
    ScriptedEvent e;
    e.event_type = type;
    e.remaining_count = remaining;
    e.detail = std::move(detail);
    return e;
}

// --- descriptors ------------------------------------------------------------------------------

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
    const char* name = "events-under-test";
    REQUIRE(writer.add_name(omgp::l3::Str{reinterpret_cast<const uint8_t*>(name),
                                          static_cast<uint8_t>(__builtin_strlen(name))}) ==
            omgp::l3::Status::Ok);
    REQUIRE(writer.add_model_id(omgp::l3::ModelIdRec{0x4321, 2, 3}) == omgp::l3::Status::Ok);
    Descriptor d;
    d.bytes.assign(buf, buf + writer.size());
    d.crc = omgp::l3::descriptor_crc(d.bytes.data(), d.bytes.size());
    return d;
}

// --- the rig ----------------------------------------------------------------------------------

// ONE MODULE PER BACKPLANE, deliberately. MockL3Node scripts one event queue per TRUNK address
// (contracts/mock-l3-node.md), and a module is addressed through its backplane's own address — so
// two modules behind one backplane would share one scripted event queue and no case could say
// which of them an event came from. Every rig below gives each module a backplane of its own,
// which is also what makes the SC-006 flood case a statement about two nodes rather than one.
class Rig {
  public:
    Rig() : node_(clock_), descriptor_(make_descriptor()) {
        transcript_.now = &now_us_;
        recorder_.now = &now_us_;
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

    // A backplane with one occupied slot whose module answers IDENTIFY, READ_DESC and parameter
    // operations, reporting `pending` events in its status block and holding `events` on its
    // queue. `pending` and `events` are independent on purpose: FR-015's trigger is the COUNT,
    // and a count that over-reports what the module actually holds is exactly the EVT_NONE case.
    void install(uint8_t addr, uint8_t pending, const std::vector<ScriptedEvent>& events) {
        std::vector<L3Step>& steps = scripts_[addr];
        steps.clear();
        steps.push_back(L3Step::of(SlotMapStep{1, 0x01u, 0x01u}));
        steps.push_back(L3Step::of(StatusStep{ready_status(pending)}));
        steps.push_back(L3Step::of(IdentifyStep{identify_for(descriptor_)}));
        steps.push_back(L3Step::of(DescChunkStep{descriptor_.bytes.data(),
                                                 static_cast<uint16_t>(descriptor_.bytes.size())}));
        steps.push_back(L3Step::of(ParamStep{true, 2048, 0}));
        for (const ScriptedEvent& e : events) {
            omgp::l3::GetEventResp resp{};
            resp.event_type = e.event_type;
            resp.remaining_count = e.remaining_count;
            resp.detail.data = e.detail.empty() ? nullptr : e.detail.data();
            resp.detail.len = static_cast<uint8_t>(e.detail.size());
            steps.push_back(L3Step::of(EventStep{resp}));
        }
        node_.set_script(addr, steps.data(), steps.size());
    }

    // The ONE exception to the one-module-per-backplane rule above, and only because nothing
    // about it is scripted per module: `slots` occupied slots behind `addr`, every one of them
    // answering IDENTIFY and READ_DESC from the same script, reporting `pending` in every status
    // block and holding NO event at all — so every GET_EVENT the count provokes answers EVT_NONE
    // (protocol-l3 §3.4) whichever module it was addressed to. That is exactly what the sibling
    // fan-out case needs: it asserts about DISCOVERY traffic, never about whose event an event
    // was. No ParamStep: nothing here sets a parameter.
    void install_populated(uint8_t addr, uint8_t slots, uint8_t pending) {
        std::vector<L3Step>& steps = scripts_[addr];
        steps.clear();
        const uint32_t occupied = (1u << slots) - 1u;
        steps.push_back(L3Step::of(SlotMapStep{slots, occupied, occupied}));
        steps.push_back(L3Step::of(StatusStep{ready_status(pending)}));
        steps.push_back(L3Step::of(IdentifyStep{identify_for(descriptor_)}));
        steps.push_back(L3Step::of(DescChunkStep{descriptor_.bytes.data(),
                                                 static_cast<uint16_t>(descriptor_.bytes.size())}));
        node_.set_script(addr, steps.data(), steps.size());
    }

    // Brings `addr`'s single module all the way to Discovered with nothing to report, so that
    // every case can arm its events at a known instant on a settled rig.
    uint8_t bring_up(uint8_t addr) {
        install(addr, 0, {});
        const size_t before = discovered_ids().size();
        run_until([&] { return discovered_ids().size() == before + 1u; }, 200u);
        const std::vector<uint8_t> ids = discovered_ids();
        REQUIRE(ids.size() == before + 1u);
        for (const uint8_t id : ids) {
            if (Bk::node(*engine_, id).backplane_addr == addr) {
                return id;
            }
        }
        FAIL("no discovered module behind backplane " << static_cast<unsigned>(addr));
        return 0;
    }

    // Re-installs `addr`'s script with events queued and its status block reporting `pending`.
    // Called on a settled rig (no transaction mid-flight), which is why it cannot disturb one.
    void arm(uint8_t addr, uint8_t pending, const std::vector<ScriptedEvent>& events) {
        install(addr, pending, events);
    }

    void step() {
        now_us_ += byte_us();
        node_.advance_to(now_us_);
        engine_->run_superframe(now_us_);
        if (drain_each_step_) {
            engine_->drain_callbacks();
        }
    }

    // Engine-only step: no callback drain and no transcript, for the rule-5 case — the test's own
    // instruments must not be charged to core/ by the counting heap guard.
    void step_bare() {
        now_us_ += byte_us();
        node_.advance_to(now_us_);
        engine_->run_superframe(now_us_);
    }

    void set_drain_each_step(bool on) {
        drain_each_step_ = on;
    }
    void detach_transcript() {
        Bk::set_transcript(*engine_, nullptr, nullptr);
    }

    // Drives until `done()` holds, failing at a stall rather than after a flat budget — the idiom
    // tests/unit/test_core_scheduler.cpp's run_until() uses, for the same reason.
    template <typename Done> void run_until(Done done, uint32_t horizon) {
        uint32_t last = transcript_.last_superframe();
        const uint32_t limit = last + horizon;
        uint64_t idle = 0;
        for (;;) {
            step();
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

    // Runs for `span_us` of simulated time, whatever the engine does in it — the idiom the
    // FR-026 cases need, where what is being waited for is a WINDOW elapsing and not a
    // transaction happening.
    void run_for(uint64_t span_us) {
        const uint64_t until = now_us_ + span_us;
        while (now_us_ < until) {
            step();
        }
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
    bool drain_each_step_ = true;
};

// A detail tail of the longest length protocol-l3 §3.1 can carry, with distinguishable bytes.
std::vector<uint8_t> max_detail() {
    std::vector<uint8_t> d(kMaxEventDetailBytes, 0);
    for (size_t i = 0; i < d.size(); ++i) {
        d[i] = static_cast<uint8_t>(0x40u + (i % 0x30u));
    }
    return d;
}

// The status block the ERROR-answer case scripts: READY, two events pending (protocol-l3 §3.3).
omgp::l3::StatusBlock error_case_status() {
    omgp::l3::StatusBlock b{};
    b.state = omgp::STATE_READY;
    b.active_channel = 0;
    b.bypass = 0;
    b.fault_code = 0;
    b.uptime_s = 1;
    b.event_pending = 2;
    return b;
}

// A flood: as many events as one MockL3Node address can hold, every one of them claiming 255
// more still pending — the shape issue #159 names, and more than kEventRateBudgetPerWindow by
// construction (asserted where it matters, so a change to either constant is loud).
std::vector<ScriptedEvent> flood_script(bool big_detail) {
    std::vector<ScriptedEvent> flood;
    for (size_t i = 0; i < MockL3Node::kEventQueueCapacity; ++i) {
        std::vector<uint8_t> detail{static_cast<uint8_t>(i)};
        if (big_detail) {
            detail = max_detail();
        }
        flood.push_back(event_of(omgp::EVT_FAULT_RAISED, 0xFFu, detail));
    }
    return flood;
}

// Drives a FRESH rig whose one backplane has `slots` occupied slots and reports `pending` events
// in every status block, and answers how many of those modules reached Discovered within
// `horizon` superframes (plus, out-parameter, how many GET_EVENTs the run issued in total). Every
// event queue behind that backplane is empty, so the count is the ONLY thing that differs between
// the two arms of the case that uses this.
size_t discovered_behind(uint8_t pending, uint8_t slots, uint32_t horizon, size_t& get_events) {
    Rig rig;
    rig.install_populated(omgp::ADDR_backplane_min, slots, pending);
    rig.run_superframes(horizon);
    get_events = rig.transcript().count(omgp::OP_GET_EVENT);
    return rig.discovered_ids().size();
}

// The same measurement with `bps` populated backplanes instead of one, the first `liars` of them
// reporting `pending` in every status block and the rest reporting none. US1 AS3's subject is
// "any other slot OR BACKPLANE", so the one-backplane form above cannot be the whole oracle: a
// per-poll bound on the sweep is still B sweeps per poll period at B reporting backplanes.
size_t discovered_behind_many(uint8_t bps, uint8_t liars, uint8_t slots_each, uint8_t pending,
                              uint32_t horizon, size_t& get_events) {
    Rig rig;
    for (uint8_t i = 0; i < bps; ++i) {
        rig.install_populated(static_cast<uint8_t>(omgp::ADDR_backplane_min + i), slots_each,
                              i < liars ? pending : 0u);
    }
    rig.run_superframes(horizon);
    get_events = rig.transcript().count(omgp::OP_GET_EVENT);
    return rig.discovered_ids().size();
}

// FR-020 floor (docs/OPEN-QUESTIONS.md 2026-11-24, ratified): `flooders` backplanes each holding
// one module that answers every GET_EVENT with another real event ("255 more"), re-armed so the
// backlog never ends, beside a QUIET backplane of `slots` modules still to be discovered. Answers
// how many of the quiet backplane's modules reached Discovered within `horizon` superframes, plus
// (out-parameters) the GET_EVENTs issued and the superframes in which one was issued. Every one of
// those drains is a drain of an event the host was TOLD about, so none is an attribution probe.
size_t discovered_beside_flood(uint8_t flooders, uint8_t slots, uint32_t horizon,
                               size_t& get_events) {
    Rig rig;
    const uint8_t quiet_bp = static_cast<uint8_t>(omgp::ADDR_backplane_min + 14u);
    rig.install_populated(quiet_bp, slots, 0u); // no count at all
    const std::vector<ScriptedEvent> flood = flood_script(false);
    for (uint8_t i = 0; i < flooders; ++i) {
        rig.install(static_cast<uint8_t>(omgp::ADDR_backplane_min + i), 0xFFu, flood);
    }
    for (uint32_t sf = 0; sf < horizon; sf += 8u) {
        rig.run_superframes(8u);
        for (uint8_t i = 0; i < flooders; ++i) { // MockL3Node holds only kEventQueueCapacity
            rig.arm(static_cast<uint8_t>(omgp::ADDR_backplane_min + i), 0xFFu, flood);
        }
    }
    get_events = rig.transcript().count(omgp::OP_GET_EVENT);
    size_t found = 0;
    for (const uint8_t id : rig.discovered_ids()) {
        if (Bk::node(rig.engine(), id).backplane_addr == quiet_bp) {
            ++found;
        }
    }
    return found;
}

} // namespace

TEST_CASE("AS1/FR-015/SC-002: a nonzero event_pending drains fully within the SC-002 bound "
          "[events][us4]") {
    // US4 AS1: "every queued event is drained and delivered to the application within (that
    // node's backplane's poll period) + (at most two superframe periods)". The whole path is
    // production code: the engine learns of the events from the scripted status block alone.
    Rig rig;
    const uint8_t bp = omgp::ADDR_backplane_min;
    const uint8_t node = rig.bring_up(bp);

    rig.arm(bp, 3,
            {event_of(omgp::EVT_FAULT_RAISED, 2, {0xA1}),
             event_of(omgp::EVT_FAULT_CLEARED, 1, {0xB2, 0xB3}),
             event_of(omgp::EVT_CHANNEL_SETTLED, 0, {0x01, 0x07})});
    const uint64_t armed_us = rig.now_us();
    rig.run_until([&] { return rig.recorder().events_of(node).size() == 3u; }, 40u);

    const std::vector<Delivery> events = rig.recorder().events_of(node);
    REQUIRE(events.size() == 3u);
    const uint64_t bound = sc002_bound_us();
    for (size_t i = 0; i < events.size(); ++i) {
        INFO("event " << i << " delivered at " << events[i].at_us << ", armed at " << armed_us
                      << ", bound " << bound);
        CHECK(events[i].at_us - armed_us <= bound);
    }
    // FR-025 is not defeated by the bound: the three drains fall in three DIFFERENT superframe
    // turns, which is what makes the latency above a statement about a K=1 drain and not about a
    // repeat-until-empty loop that happened to meet the deadline.
    const std::vector<uint32_t> turns = rig.transcript().superframes_for(omgp::OP_GET_EVENT, node);
    REQUIRE(turns.size() >= 3u);
    CHECK(turns[0] < turns[1]);
    CHECK(turns[1] < turns[2]);
    // No flood: three events are inside kEventRateBudgetPerWindow, so nothing is reported faulted.
    CHECK(rig.recorder().count(LifecycleKind::NodeEventFault) == 0u);
    CHECK_FALSE(Bk::node(rig.engine(), node).event_faulted);
}

TEST_CASE("AS2/FR-016: several queued events arrive in the order the module queued them "
          "[events][us4]") {
    // US4 AS2. The scripted queue order IS the module's order (MockL3Node drains its queue
    // head-first), so the assertion is a straight sequence comparison — type, remaining_count and
    // detail each delivered verbatim (FR-016).
    Rig rig;
    const uint8_t bp = omgp::ADDR_backplane_min;
    const uint8_t node = rig.bring_up(bp);

    const std::vector<ScriptedEvent> scripted = {
        event_of(omgp::EVT_FAULT_RAISED, 3, {0x11}),
        event_of(omgp::EVT_CHANNEL_SETTLED, 2, {0x02, 0x09}),
        event_of(omgp::EVT_FAULT_CLEARED, 1, {0x33, 0x34, 0x35}),
        event_of(omgp::EVT_FAULT_RAISED, 0, {})};
    rig.arm(bp, static_cast<uint8_t>(scripted.size()), scripted);
    rig.run_until([&] { return rig.recorder().events_of(node).size() == scripted.size(); }, 40u);

    const std::vector<Delivery> events = rig.recorder().events_of(node);
    REQUIRE(events.size() == scripted.size());
    for (size_t i = 0; i < scripted.size(); ++i) {
        INFO("delivery " << i);
        CHECK(events[i].ev.module_event_type == scripted[i].event_type);
        CHECK(events[i].ev.module_event_remaining == scripted[i].remaining_count);
        REQUIRE(events[i].detail_len == scripted[i].detail.size());
        for (size_t j = 0; j < scripted[i].detail.size(); ++j) {
            CHECK(events[i].detail[j] == scripted[i].detail[j]);
        }
    }
}

TEST_CASE("AS3/FR-015: a node that keeps reporting events pending is not abandoned "
          "[events][us4]") {
    // US4 AS3: a node queuing faster than one drain cycle empties it keeps being drained across
    // superframes, honouring the per-superframe budget — never dropped after the first drain.
    // The scripted status block holds its count at kEventRateBudgetPerWindow for ever (an
    // exhausted MockL3Node class answers with its last step), so the count is still being
    // re-reported after the queue it describes has been drained.
    Rig rig;
    const uint8_t bp = omgp::ADDR_backplane_min;
    const uint8_t node = rig.bring_up(bp);

    std::vector<ScriptedEvent> scripted;
    for (size_t i = 0; i < kEventRateBudgetPerWindow; ++i) {
        scripted.push_back(
            event_of(omgp::EVT_FAULT_RAISED, 0xFFu, {static_cast<uint8_t>(0x50u + i)}));
    }
    rig.arm(bp, kEventRateBudgetPerWindow, scripted);
    rig.run_until([&] { return rig.recorder().events_of(node).size() == scripted.size(); }, 60u);

    const std::vector<Delivery> events = rig.recorder().events_of(node);
    REQUIRE(events.size() == scripted.size());
    // Every event's own detail byte arrived, in order: nothing was dropped mid-drain.
    for (size_t i = 0; i < events.size(); ++i) {
        REQUIRE(events[i].detail_len == 1u);
        CHECK(events[i].detail[0] == static_cast<uint8_t>(0x50u + i));
    }
    // And each took its own superframe turn — never zero drains after the first, which is the
    // falsifier AS3 names (an engine that dropped the item when the first drain did not empty the
    // queue would deliver one and stop).
    const std::vector<uint32_t> turns = rig.transcript().superframes_for(omgp::OP_GET_EVENT, node);
    REQUIRE(turns.size() >= scripted.size());
    for (size_t i = 1; i < scripted.size(); ++i) {
        INFO("drain " << i << " in superframe " << turns[i] << ", previous " << turns[i - 1]);
        CHECK(turns[i] > turns[i - 1]);
    }
}

TEST_CASE("FR-025: at most one GET_EVENT per node per superframe turn, whatever remaining_count "
          "claims [events][us4]") {
    // spec FR-025 / #110 F6: `remaining_count` is advisory — read only to decide whether to
    // re-queue, never to size a buffer or bound a loop. Every scripted event claims 255 still
    // pending, which is the flooding vector issue #159 names: a repeat-until-empty loop (§3.1's
    // own wording) would issue 255 GET_EVENTs, or hang.
    Rig rig;
    const uint8_t bp = omgp::ADDR_backplane_min;
    const uint8_t node = rig.bring_up(bp);

    std::vector<ScriptedEvent> scripted;
    for (size_t i = 0; i < kEventRateBudgetPerWindow; ++i) {
        scripted.push_back(event_of(omgp::EVT_FAULT_RAISED, 0xFFu, {static_cast<uint8_t>(i)}));
    }
    rig.arm(bp, 0xFFu, scripted);
    const uint32_t first = Bk::superframe(rig.engine()) + 1u;
    rig.run_until([&] { return rig.recorder().events_of(node).size() == scripted.size(); }, 60u);
    const uint32_t last = rig.transcript().last_superframe();

    size_t turns_with_a_drain = 0;
    for (uint32_t sf = first; sf <= last; ++sf) {
        size_t drains = 0;
        for (const Line& l : rig.transcript().lines) {
            if (l.entry.superframe == sf && l.entry.opcode == omgp::OP_GET_EVENT &&
                l.entry.node_id == node) {
                ++drains;
            }
        }
        INFO("superframe " << sf);
        CHECK(drains <= 1u);
        if (drains == 1u) {
            ++turns_with_a_drain;
        }
    }
    // One per turn, so as many turns as events — and the four deliveries prove the turns were
    // spent on real events rather than on a loop that re-asked within one superframe.
    CHECK(turns_with_a_drain >= scripted.size());
    CHECK(rig.recorder().events_of(node).size() == scripted.size());

    SECTION("and a module that claimed 255 and then answers EVT_NONE stops being drained") {
        // #110 F6's other half: the advisory count cannot make the host keep asking. The
        // scripted queue is exhausted, so the next GET_EVENT is answered NONE (§3.4) and the
        // count the status block keeps reporting is the only thing that offers the node again.
        const size_t events_before = rig.recorder().events_of(node).size();
        const size_t status_before = rig.transcript().count_for(omgp::OP_GET_STATUS, bp);
        const size_t drains_before = rig.transcript().count_for(omgp::OP_GET_EVENT, node);
        rig.run_superframes(8u);
        const size_t status_after = rig.transcript().count_for(omgp::OP_GET_STATUS, bp);
        const size_t drains_after = rig.transcript().count_for(omgp::OP_GET_EVENT, node);
        // Nothing more was delivered, and the drains that did happen are bounded by the polls
        // that re-reported the count — never one per superframe, which is what a re-queue on a
        // NONE answer would produce.
        CHECK(rig.recorder().events_of(node).size() == events_before);
        CHECK(drains_after - drains_before <= status_after - status_before);
        CHECK_FALSE(Bk::node(rig.engine(), node).event_faulted);
    }
}

TEST_CASE("protocol-l3 §3.4: EVT_NONE terminates draining — no ModuleEvent, no fault, no re-queue "
          "[events][us4]") {
    // A status block over-reporting what the module holds is the ordinary case for the fan-out a
    // backplane's aggregate count forces (docs/OPEN-QUESTIONS.md 2026-11-23): a module with
    // nothing queued answers NONE, which must deliver nothing and charge nothing.
    Rig rig;
    const uint8_t bp = omgp::ADDR_backplane_min;
    const uint8_t node = rig.bring_up(bp);

    rig.arm(bp, 2, {}); // a count of two, and an empty event queue
    rig.run_superframes(10u);

    CHECK(rig.recorder().count(LifecycleKind::ModuleEvent) == 0u);
    CHECK(rig.recorder().count(LifecycleKind::NodeEventFault) == 0u);
    CHECK_FALSE(Bk::node(rig.engine(), node).event_faulted);
    // It really did ask — an engine that never issued a GET_EVENT would pass every check above.
    const size_t drains = rig.transcript().count_for(omgp::OP_GET_EVENT, node);
    CHECK(drains >= 1u);
    // ... and it did not re-queue on the NONE: the drains are bounded by the status polls that
    // re-reported the count, not by the superframes that elapsed.
    CHECK(drains <= rig.transcript().count_for(omgp::OP_GET_STATUS, bp));
    // The node is untouched by any of it.
    CHECK(rig.engine().discovery_state(node) == DiscoveryState::Discovered);
}

TEST_CASE("FR-015: a status poll reporting no events produces no GET_EVENT at all "
          "[events][us4]") {
    // The detection half's own falsifier: the trigger is the status block's count, not the
    // module's possession of an event. The module holds three events and reports none pending, so
    // a host that polled GET_EVENT speculatively — or on a timer — would drain them and fail here.
    Rig rig;
    const uint8_t bp = omgp::ADDR_backplane_min;
    const uint8_t node = rig.bring_up(bp);

    rig.arm(bp, 0,
            {event_of(omgp::EVT_FAULT_RAISED, 0, {0x01}), event_of(omgp::EVT_FAULT_CLEARED, 0, {}),
             event_of(omgp::EVT_CHANNEL_SETTLED, 0, {0x00, 0x01})});
    rig.run_superframes(12u);

    CHECK(rig.transcript().count_for(omgp::OP_GET_EVENT, node) == 0u);
    CHECK(rig.recorder().count(LifecycleKind::ModuleEvent) == 0u);
    CHECK(Bk::event_queued(rig.engine()) == 0u);
}

TEST_CASE("SC-006/FR-025: a flooding node never delays another node's event past the bound "
          "[events][us4][concurrent-load]") {
    // SC-006, and quickstart.md §6's discriminating check: with the K=1 cap removed the
    // well-behaved node's latency assertion below must fail. Two backplanes, one module each — a
    // flood (always reporting events pending, every answer claiming 255 more) and a single honest
    // event armed at the same instant.
    Rig rig;
    const uint8_t flood_bp = omgp::ADDR_backplane_min;
    const uint8_t quiet_bp = static_cast<uint8_t>(omgp::ADDR_backplane_min + 1u);
    const uint8_t flooder = rig.bring_up(flood_bp);
    const uint8_t quiet = rig.bring_up(quiet_bp);
    REQUIRE(flooder != quiet);

    rig.arm(flood_bp, 0xFFu, flood_script(true));
    rig.arm(quiet_bp, 1, {event_of(omgp::EVT_CHANNEL_SETTLED, 0, {0x03, 0x11})});
    const uint64_t armed_us = rig.now_us();

    rig.run_until([&] { return rig.recorder().events_of(quiet).size() == 1u; }, 60u);
    const std::vector<Delivery> quiet_events = rig.recorder().events_of(quiet);
    REQUIRE(quiet_events.size() == 1u);
    const uint64_t bound = sc002_bound_us();
    INFO("the quiet node's event landed at " << quiet_events[0].at_us << ", armed at " << armed_us
                                             << ", bound " << bound << "; the flooder had "
                                             << rig.recorder().events_of(flooder).size()
                                             << " events drained by then");
    CHECK(quiet_events[0].at_us - armed_us <= bound);
    // The flooder was being drained throughout — the bound above is held WITH the flood in
    // flight, not because the flood never started.
    CHECK(rig.recorder().events_of(flooder).size() >= 1u);
    // And no superframe gave the flooder two drains.
    //
    // MEASURED, and stated rather than claimed (CLAUDE.md rule 11): on THIS rig that assertion is
    // a CONTROL, not a discriminator. With the K=1 cap removed from drain_events() the two
    // assertions above still hold and this one still holds too — two enrolled backplanes spend
    // enough of the superframe budget on their mandatory polls (FR-002) that the budget alone
    // admits one event drain per superframe, and this engine re-queues a node's remainder at the
    // BACK of the ring (R-07's correction), behind every other node's item, so a flood cannot
    // take the quiet node's place even uncapped. quickstart.md §6 expects the cap's removal to
    // fail the well-behaved node's latency assertion; on this rig it does not, and what it does
    // fail is the per-turn assertion in `AS1/FR-015/SC-002`, `AS3/FR-015` and `FR-025` — single
    // -node rigs whose budget admits several drains per superframe — plus three cases in
    // tests/unit/test_core_scheduler.cpp (`FR-025: one node's queued events do not delay another
    // node's` among them). The run that shows it is quoted in this PR's description, under
    // "Discriminating check (quickstart.md §6)"; it is reproduced by negating the
    // `event_drained_superframe == superframe_` test in drain_events() and running
    // `./pipeline.sh build unit`. quickstart.md §6's wording is a Spec Kit artefact this fix does
    // not amend, so the mismatch is named here and in the PR rather than edited away there.
    rig.run_superframes(12u);
    const std::vector<uint32_t> flood_turns =
        rig.transcript().superframes_for(omgp::OP_GET_EVENT, flooder);
    REQUIRE(flood_turns.size() >= 2u);
    for (size_t i = 1; i < flood_turns.size(); ++i) {
        INFO("flood drain " << i << " in superframe " << flood_turns[i]);
        CHECK(flood_turns[i] > flood_turns[i - 1]);
    }
}

TEST_CASE("SC-006/US1 AS3: a backplane's standing event_pending does not delay the discovery of "
          "the modules behind it [events][us4]") {
    // The cost of v1's missing per-slot event attribution (docs/OPEN-QUESTIONS.md 2026-11-23) is
    // paid in DEMAND slots, which is where IDENTIFY and READ_DESC live — and events are drained
    // ahead of both (FR-020). So the attribution sweep a backplane's aggregate count provokes has
    // to be bounded per poll, not one GET_EVENT per sibling module per poll: US1 AS3 says a slot
    // whose module misbehaves "does not block the discovery of any other slot or backplane", and
    // SC-006 that a node whose status poll always reports events pending never prevents any other
    // node's progress.
    //
    // Two arms differing in ONE byte of one status block, with every event queue behind the
    // backplane empty — so every GET_EVENT either arm issues answers EVT_NONE and delivers
    // nothing. The control arm is the whole of the evidence: if the count were free, both arms
    // would discover the same number of modules.
    constexpr uint8_t kSlots = static_cast<uint8_t>(omgp::LIMIT_max_slots_per_backplane);
    constexpr uint32_t kHorizon = 400;
    size_t quiet_drains = 0;
    size_t liar_drains = 0;
    const size_t quiet = discovered_behind(0u, kSlots, kHorizon, quiet_drains);
    const size_t liar = discovered_behind(0xFFu, kSlots, kHorizon, liar_drains);
    INFO("control (event_pending=0): " << quiet << "/" << static_cast<unsigned>(kSlots)
                                       << " discovered in " << kHorizon
                                       << " superframes, GET_EVENT=" << quiet_drains);
    INFO("attack  (event_pending=255): " << liar << "/" << static_cast<unsigned>(kSlots)
                                         << " discovered in " << kHorizon
                                         << " superframes, GET_EVENT=" << liar_drains);
    REQUIRE(quiet == static_cast<size_t>(kSlots)); // the arm the attack is measured against
    CHECK(quiet_drains == 0u);                     // FR-015: no count, no GET_EVENT at all
    CHECK(liar == quiet);
    // It really did ask — an engine that ignored the count would pass the line above for the
    // wrong reason. One sweep step per poll that reports a count, never one per sibling: the
    // drains are bounded by the status polls of that backplane, not by its slot count times them.
    CHECK(liar_drains >= 1u);
    // MEASURED on this rig (stated, not claimed — CLAUDE.md rule 11): 16/16 discovered in both
    // arms, 0 GET_EVENT in the control and 198 in the attack — one per status poll of the
    // reporting backplane, where the fan-out-to-every-sibling form issued 980 and discovered 5.
}

TEST_CASE("SC-006/US1 AS3: SEVERAL backplanes' standing event_pending does not delay the discovery "
          "of any other backplane's modules [events][us4]") {
    // The case above pins the sweep at ONE reporting backplane, where one sweep step per poll
    // is one GET_EVENT per poll period. US1 AS3's subject is "any other slot or BACKPLANE"
    // and SC-006's is "any other node", so the property has to hold with SEVERAL liars too.
    // A bound that is per (backplane, poll) is B GET_EVENTs per poll period in AGGREGATE,
    // competing for the same demand budget as the discovery of every OTHER backplane's
    // modules (FR-020 drains events first). v1 allows 15 backplane addresses
    // (ADDR_backplane_min..max), so B > 1 is the ordinary case and not a corner.
    //
    // Every event queue behind every backplane is EMPTY here, exactly as above: every
    // GET_EVENT either arm issues answers EVT_NONE and delivers nothing. The arms differ in
    // one byte of the liars' status blocks.
    constexpr uint32_t kHorizon = 400;
    constexpr uint8_t kBps = 8;
    constexpr uint8_t kSlotsEach = 4;
    constexpr size_t kTotal = static_cast<size_t>(kBps) * kSlotsEach;
    size_t quiet_drains = 0;
    const size_t quiet =
        discovered_behind_many(kBps, 0u, kSlotsEach, 0xFFu, kHorizon, quiet_drains);
    INFO("control (no backplane reports): " << quiet << "/" << kTotal << " discovered in "
                                            << kHorizon
                                            << " superframes, GET_EVENT=" << quiet_drains);
    REQUIRE(quiet == kTotal); // the arm the attack is measured against
    CHECK(quiet_drains == 0u);

    // One liar, then a majority of them: the pathology the one-backplane case cannot see
    // appears between the two, so both arms are asserted rather than only the worst.
    const uint8_t liars = static_cast<uint8_t>(GENERATE(1, 2, 4, 8));
    size_t liar_drains = 0;
    const size_t discovered =
        discovered_behind_many(kBps, liars, kSlotsEach, 0xFFu, kHorizon, liar_drains);
    INFO("attack  (" << static_cast<unsigned>(liars) << " of " << static_cast<unsigned>(kBps)
                     << " backplanes report event_pending=255): " << discovered << "/" << kTotal
                     << " discovered, GET_EVENT=" << liar_drains);
    CHECK(discovered == kTotal);
    // It really did ask — an engine that ignored the count would pass the line above for
    // the wrong reason.
    CHECK(liar_drains >= 1u);
    // MEASURED on this rig (stated, not claimed — CLAUDE.md rule 11): 32/32 discovered in
    // every arm, and GET_EVENT = 198 at one, two and eight reporting backplanes alike, so
    // the aggregate probe rate is INDEPENDENT of how many backplanes report. That is the
    // property EventDrainItem::speculative exists for. Bounded only per (backplane, poll),
    // the same arms gave 392 GET_EVENT and 5 of 32 discovered at two liars and above: the
    // RED run this case was written from, quoted again in drain_events()'s own note.
}

TEST_CASE("FR-020 floor: an endless honest event backlog does not starve discovery [events][us4]") {
    // #952 red team round 5, finding 1 [HIGH]. The speculative-item alternation bounds attribution
    // PROBES only; a drain of an event the host was told about (on_event_drain()'s
    // remaining_count > 0 continuation) keeps strict FR-020 priority, so enough nodes with an
    // endless backlog hold every demand slot and nothing is ever identified. The ratified floor
    // (OPEN-QUESTIONS 2026-11-24): while non-event demand work is pending, at least every second
    // superframe's demand phase serves it first. WRITTEN FIRST, RED at 479bf34 for flooders 3 and
    // 4 (0 of 4 quiet modules discovered in 400 superframes; the red team measured the same).
    constexpr uint32_t kHorizon = 400;
    constexpr uint8_t kSlots = 4;
    size_t drains = 0;
    REQUIRE(discovered_beside_flood(0u, kSlots, kHorizon, drains) == kSlots); // control
    CHECK(drains == 0u);
    for (const uint8_t flooders : {uint8_t{1}, uint8_t{2}, uint8_t{3}, uint8_t{4}}) {
        drains = 0;
        const size_t found = discovered_beside_flood(flooders, kSlots, kHorizon, drains);
        INFO(static_cast<unsigned>(flooders)
             << " flooding backplane(s): " << found << "/" << static_cast<unsigned>(kSlots)
             << " quiet modules discovered in " << kHorizon
             << " superframes, GET_EVENT=" << drains);
        CHECK(found == static_cast<size_t>(kSlots));
        CHECK(drains >= 1u); // it really drained, so a pass is not an engine that ignores the flood
    }
}

TEST_CASE("FR-020 floor: events keep making progress while non-event work is pending, and are not "
          "delayed when none is [events][us4]") {
    // The floor's other edge. It must not turn FR-020 into "discovery first": a finite honest
    // backlog (within kEventRateBudgetPerWindow, so FR-026 does not engage) is drained at least
    // every SECOND superframe even while four other modules are being discovered, and with no
    // non-event work pending the floor is not engaged at all, so the drains are in CONSECUTIVE
    // superframes (FR-025's K = 1, undelayed). Both arms PASS at 479bf34 — strict priority gives
    // events every slot — so this is a guard on the implementation, not a RED case.
    constexpr uint8_t kBacklog = 4;
    static_assert(kBacklog <= kEventRateBudgetPerWindow, "the backlog must stay inside FR-026");
    const std::vector<ScriptedEvent> backlog = {
        event_of(omgp::EVT_FAULT_RAISED, 3u, {1u}), event_of(omgp::EVT_FAULT_RAISED, 2u, {2u}),
        event_of(omgp::EVT_FAULT_RAISED, 1u, {3u}), event_of(omgp::EVT_FAULT_RAISED, 0u, {4u})};
    REQUIRE(backlog.size() == kBacklog);
    const uint8_t bp = omgp::ADDR_backplane_min;
    const uint8_t quiet_bp = static_cast<uint8_t>(omgp::ADDR_backplane_min + 14u);

    // Arm A: nothing else to do. Consecutive superframes.
    {
        Rig rig;
        const uint8_t node = rig.bring_up(bp);
        rig.arm(bp, kBacklog, backlog);
        rig.run_superframes(24u);
        const std::vector<uint32_t> at = rig.transcript().superframes_for(omgp::OP_GET_EVENT, node);
        REQUIRE(at.size() >= kBacklog);
        for (size_t i = 1; i < kBacklog; ++i) {
            INFO("drain " << i << " at superframe " << at[i] << ", previous " << at[i - 1]);
            CHECK(at[i] - at[i - 1] == 1u);
        }
    }
    // Arm B: four modules behind another backplane are being discovered meanwhile.
    {
        Rig rig;
        const uint8_t node = rig.bring_up(bp);
        rig.install_populated(quiet_bp, 4u, 0u);
        rig.arm(bp, kBacklog, backlog);
        rig.run_superframes(48u);
        const std::vector<uint32_t> at = rig.transcript().superframes_for(omgp::OP_GET_EVENT, node);
        REQUIRE(at.size() >= kBacklog);
        for (size_t i = 1; i < kBacklog; ++i) {
            INFO("drain " << i << " at superframe " << at[i] << ", previous " << at[i - 1]);
            CHECK(at[i] - at[i - 1] <= 2u); // the floor's "at least every second superframe"
        }
    }
}

TEST_CASE("FR-026: an over-budget node is reported NodeEventFault and stops being drained "
          "[events][us4]") {
    // spec FR-026 / research.md R-07's correction: a node whose event-delivery rate exceeds the
    // configured budget is reported faulted and MUST NOT have further events drained from it
    // until its rate returns within budget, which is reported too. Budget and window are
    // core/core_types.hpp's own constants — never literals here.
    Rig rig;
    const uint8_t bp = omgp::ADDR_backplane_min;
    const uint8_t node = rig.bring_up(bp);

    const std::vector<ScriptedEvent> flood = flood_script(false);
    REQUIRE(flood.size() > static_cast<size_t>(kEventRateBudgetPerWindow));
    rig.arm(bp, 0xFFu, flood);

    rig.run_until([&] { return rig.recorder().count(LifecycleKind::NodeEventFault) == 1u; }, 60u);
    REQUIRE(rig.recorder().count(LifecycleKind::NodeEventFault) == 1u);
    const Delivery faulted = rig.recorder().first(LifecycleKind::NodeEventFault);
    CHECK(faulted.ev.node_id == node);
    CHECK(Bk::node(rig.engine(), node).event_faulted);
    // It took a flood to get here: the budget is not crossed by one event.
    const size_t drained_by_fault = rig.recorder().events_of(node).size();
    CHECK(drained_by_fault > static_cast<size_t>(kEventRateBudgetPerWindow));

    SECTION("no further GET_EVENT reaches it while it is faulted, and exactly one report is made") {
        // Half a window after the fault: still faulted, and not addressed once in between.
        const uint64_t fault_us = faulted.at_us;
        rig.run_for(kEventRateWindowUs / 2u);
        CHECK(Bk::node(rig.engine(), node).event_faulted);
        CHECK(rig.transcript().count_between(omgp::OP_GET_EVENT, node, fault_us, rig.now_us()) ==
              0u);
        CHECK(rig.recorder().count(LifecycleKind::NodeEventFault) == 1u);
        CHECK(rig.recorder().events_of(node).size() == drained_by_fault);
        // The backlog is kept, not discarded: its item is still in the ring waiting for the
        // fault to clear (FR-026 suspends draining, it does not abandon the queue).
        CHECK(Bk::event_queued(rig.engine()) >= 1u);
        // Its backplane's own status poll never stops (FR-002/FR-028): a behavioural fault is not
        // a connectivity one.
        const size_t polls_before = rig.transcript().count_for(omgp::OP_GET_STATUS, bp);
        rig.run_superframes(4u);
        CHECK(rig.transcript().count_for(omgp::OP_GET_STATUS, bp) > polls_before);
    }

    SECTION("a full window within budget clears the fault, exactly once, and draining resumes") {
        const auto cleared_once = [&] {
            return rig.recorder().count(LifecycleKind::NodeEventFaultCleared) == 1u;
        };
        rig.run_until(cleared_once, 400u);
        const Delivery cleared = rig.recorder().first(LifecycleKind::NodeEventFaultCleared);
        CHECK(cleared.ev.node_id == node);
        CHECK_FALSE(Bk::node(rig.engine(), node).event_faulted);
        // A whole window passed under budget before it cleared — not a superframe, and not the
        // next status poll.
        CHECK(cleared.at_us - faulted.at_us >= kEventRateWindowUs);
        // Draining resumes, and the events the fault suspended are delivered after all.
        rig.run_until([&] { return rig.recorder().events_of(node).size() > drained_by_fault; },
                      60u);
        CHECK(rig.recorder().events_of(node).size() > drained_by_fault);
        CHECK(rig.recorder().count(LifecycleKind::NodeEventFault) == 1u);
        CHECK(rig.recorder().count(LifecycleKind::NodeEventFaultCleared) == 1u);
    }
}

TEST_CASE("CLAUDE.md rule 7: a reserved or user-defined event type is passed through "
          "uninterpreted [events][us4]") {
    // protocol-l3 §3.4 allocates PARAM_CHANGED_LOCALLY (reserved in v1) and the whole
    // USER_DEFINED range. core/ interprets none of them: each is delivered as a plain ModuleEvent
    // carrying the type verbatim, with no per-type branch, no node fault and no effect on the
    // node's enrolment.
    Rig rig;
    const uint8_t bp = omgp::ADDR_backplane_min;
    const uint8_t node = rig.bring_up(bp);

    const std::vector<ScriptedEvent> scripted = {
        event_of(omgp::EVT_PARAM_CHANGED_LOCALLY, 3, {0x01}),
        event_of(omgp::EVT_USER_DEFINED_MIN, 2, {0x02, 0x03}),
        event_of(static_cast<uint8_t>(omgp::EVT_USER_DEFINED_MIN + 7u), 1, {}),
        event_of(omgp::EVT_USER_DEFINED_MAX, 0, {0x04})};
    rig.arm(bp, static_cast<uint8_t>(scripted.size()), scripted);
    rig.run_until([&] { return rig.recorder().events_of(node).size() == scripted.size(); }, 60u);

    const std::vector<Delivery> events = rig.recorder().events_of(node);
    REQUIRE(events.size() == scripted.size());
    for (size_t i = 0; i < scripted.size(); ++i) {
        INFO("delivery " << i << " type " << static_cast<unsigned>(events[i].ev.module_event_type));
        CHECK(events[i].ev.module_event_type == scripted[i].event_type);
    }
    CHECK(rig.recorder().count(LifecycleKind::NodeEventFault) == 0u);
    CHECK(rig.engine().discovery_state(node) == DiscoveryState::Discovered);
    // The next superframe still runs: an unknown type is not fatal (rule 7).
    const uint32_t before = Bk::superframe(rig.engine());
    rig.run_superframes(2u);
    CHECK(Bk::superframe(rig.engine()) > before);

    SECTION("an UNALLOCATED code never reaches core/ at all — the L3 codec refuses it") {
        // Where this host's forward compatibility is narrower than rule 7's "ignored" would
        // allow, stated rather than left to be discovered: l3::decode_get_event_resp (and the
        // encoder, so MockL3Node cannot script one either) refuses an event_type outside §3.4's
        // own set, so an unallocated 0x05..0xEF code is ignored one layer BELOW core/ instead of
        // being delivered verbatim. Asserted here so a later l3/ change that widens the codec
        // shows up as this file's business too (docs/OPEN-QUESTIONS.md 2026-11-23).
        omgp::l3::GetEventResp resp{};
        resp.event_type = 0x05u;
        resp.remaining_count = 0;
        resp.detail.data = nullptr;
        resp.detail.len = 0;
        uint8_t buf[omgp::LIMIT_max_l3_payload] = {};
        size_t written = 0;
        CHECK(omgp::l3::encode_get_event_resp(resp, buf, sizeof buf, written) ==
              omgp::l3::Status::OutOfRange);
        const uint8_t wire[2] = {0x05u, 0x00u};
        omgp::l3::GetEventResp decoded{};
        CHECK(omgp::l3::decode_get_event_resp(wire, sizeof wire, decoded) ==
              omgp::l3::Status::OutOfRange);
    }
}

TEST_CASE("FR-016: detail is delivered verbatim and survives to drain_callbacks() "
          "[events][us4]") {
    // l3::GetEventResp::detail is a view into link::Master's receive buffer, valid only until the
    // next poll() (link/master.hpp:16-23), while a queued ModuleEvent is delivered arbitrarily
    // later (FR-021). This case holds the drain back for several superframes — many poll() calls,
    // and several more transactions through that same buffer — and then compares the bytes.
    Rig rig;
    const uint8_t bp = omgp::ADDR_backplane_min;
    const uint8_t node = rig.bring_up(bp);
    const std::vector<uint8_t> big = max_detail();
    // bring_up() already delivered this node's NodeDiscovered; this case's subject starts here.
    rig.engine().drain_callbacks();
    rig.recorder().deliveries.clear();

    rig.set_drain_each_step(false);
    const std::vector<ScriptedEvent> two = {event_of(omgp::EVT_FAULT_RAISED, 1, big),
                                            event_of(omgp::EVT_FAULT_CLEARED, 0, {})};
    rig.arm(bp, 2, two);
    rig.run_until([&] { return rig.transcript().count_for(omgp::OP_GET_EVENT, node) >= 2u; }, 60u);
    // Nothing has been delivered yet (FR-021): run_superframe() enqueues only.
    REQUIRE(rig.recorder().deliveries.empty());
    rig.run_superframes(6u);
    rig.engine().drain_callbacks();

    const std::vector<Delivery> events = rig.recorder().events_of(node);
    REQUIRE(events.size() == 2u);
    REQUIRE(events[0].detail_len == big.size());
    for (size_t i = 0; i < big.size(); ++i) {
        INFO("detail byte " << i);
        CHECK(events[0].detail[i] == big[i]);
    }
    // An empty detail is delivered as an empty detail, not as a dangling pointer.
    CHECK(events[1].detail_len == 0u);
    CHECK(events[1].ev.module_event_detail == nullptr);
    CHECK(rig.engine().dropped_deliveries() == 0u);
}

TEST_CASE("FR-021: no event is delivered from inside run_superframe() [events][us4]") {
    // data-model.md §8a / R-04: run_superframe() enqueues, and only drain_callbacks() calls back.
    // The whole drain runs with no drain_callbacks() call at all.
    Rig rig;
    const uint8_t bp = omgp::ADDR_backplane_min;
    const uint8_t node = rig.bring_up(bp);
    rig.engine().drain_callbacks();
    rig.recorder().deliveries.clear();

    rig.set_drain_each_step(false);
    rig.arm(bp, 3,
            {event_of(omgp::EVT_FAULT_RAISED, 2, {0x21}), event_of(omgp::EVT_FAULT_CLEARED, 1, {}),
             event_of(omgp::EVT_CHANNEL_SETTLED, 0, {0x05, 0x06})});
    // Until the third drain has been ANSWERED, not merely issued: the transcript records a
    // request at issue time, and the answer this case is about arrives a step or two later.
    rig.run_until([&] { return rig.transcript().count_for(omgp::OP_GET_EVENT, node) >= 3u; }, 60u);
    rig.run_superframes(2u);

    CHECK(rig.recorder().deliveries.empty());
    rig.engine().drain_callbacks();
    CHECK(rig.recorder().events_of(node).size() == 3u);
}

TEST_CASE("AS1/FR-020/SC-002: the bound holds while a 40-operation parameter burst competes for "
          "the same budget [events][us4][concurrent-load]") {
    // SC-002's own "including scenarios with concurrent parameter-set traffic", and FR-020's
    // reason for existing: the event drain is scheduled ahead of the parameter operations already
    // queued, so the latency bound holds on a loaded schedule and not only on an idle one.
    Rig rig;
    const uint8_t bp = omgp::ADDR_backplane_min;
    const uint8_t node = rig.bring_up(bp);

    for (int i = 0; i < 40; ++i) {
        REQUIRE(rig.engine().set_param(node, static_cast<uint8_t>(i), 0, 1) == CoreStatus::Ok);
    }
    rig.arm(bp, 1, {event_of(omgp::EVT_FAULT_RAISED, 0, {0x77})});
    const uint64_t armed_us = rig.now_us();
    const size_t params_before = rig.transcript().count(omgp::OP_SET_PARAM);

    rig.run_until([&] { return rig.recorder().events_of(node).size() == 1u; }, 60u);
    const std::vector<Delivery> events = rig.recorder().events_of(node);
    REQUIRE(events.size() == 1u);
    INFO("delivered at " << events[0].at_us << ", armed at " << armed_us << ", bound "
                         << sc002_bound_us());
    CHECK(events[0].at_us - armed_us <= sc002_bound_us());
    // The burst really was in flight — the bound was met under load, not after it drained.
    CHECK(rig.transcript().count(omgp::OP_SET_PARAM) > params_before);
    CHECK(rig.recorder().count(LifecycleKind::ModuleEvent) == 1u);
}

TEST_CASE("trunk §8: an ERROR answer to a GET_EVENT delivers nothing and is retried from the next "
          "poll [events][us4]") {
    // trunk §8 MANDATES ERR_BUSY from a bridge whose module bus is not ready, so an ERROR is the
    // common first answer to a bridged request, not a corner. Nothing is delivered, no node is
    // faulted, and the node is offered again by the next status poll that still reports a count —
    // never re-queued on the spot, which is what would let a permanently-refusing node hold the
    // event ring (drained first, FR-020) against every other node's events.
    // Declared before the rig so the script outlives the MockL3Node that points at it.
    std::vector<L3Step> steps;
    Rig rig;
    const uint8_t bp = omgp::ADDR_backplane_min;
    const uint8_t node = rig.bring_up(bp);

    // A StatusStep reporting events pending, with an ErrorStep behind it. An ErrorStep is a
    // MockL3Node wildcard taken by the first request of any class that reaches it in script
    // order — the GET_STATUS takes the status step, so the GET_EVENT takes the error, and from
    // then on the Event class (which has no step of its own kind) keeps answering from that same
    // wildcard while the Status class answers from its own last step (its steady state).
    steps.push_back(L3Step::of(StatusStep{error_case_status()}));
    steps.push_back(L3Step::of(ErrorStep{omgp::ERR_BUSY}));
    rig.node().set_script(bp, steps.data(), steps.size());
    rig.run_until([&] { return rig.transcript().count_for(omgp::OP_GET_EVENT, node) >= 1u; }, 40u);
    const size_t drains_before = rig.transcript().count_for(omgp::OP_GET_EVENT, node);
    const size_t polls_before = rig.transcript().count_for(omgp::OP_GET_STATUS, bp);
    rig.run_superframes(8u);

    // It asked, it was refused, and nothing was delivered or charged for it.
    CHECK(drains_before >= 1u);
    CHECK(rig.recorder().count(LifecycleKind::ModuleEvent) == 0u);
    CHECK(rig.recorder().count(LifecycleKind::NodeEventFault) == 0u);
    CHECK(rig.engine().discovery_state(node) == DiscoveryState::Discovered);
    // Re-offered by the polls that re-report the count, never re-queued on the spot: the drains
    // stay bounded by the status polls rather than taking a demand slot every superframe.
    const size_t drains = rig.transcript().count_for(omgp::OP_GET_EVENT, node) - drains_before;
    const size_t polls = rig.transcript().count_for(omgp::OP_GET_STATUS, bp) - polls_before;
    INFO(drains << " drains against " << polls << " status polls");
    CHECK(drains <= polls + 1u);
    // The schedule keeps running: an ERROR is an answer, not a stall.
    const uint32_t before = Bk::superframe(rig.engine());
    rig.run_superframes(2u);
    CHECK(Bk::superframe(rig.engine()) > before);
}

TEST_CASE("CLAUDE.md rule 5: the event path allocates nothing [events][us4]") {
    // rule 5 for the embedded path: detection, the K=1 drain, the detail copy into the engine's
    // own arena, the rate window and the flood fault all run inside the counting heap guard. The
    // test's own instruments are detached or reserved first — the guard counts every allocation
    // on the thread, and a std::vector growing inside it would be charged to core/.
    Rig rig;
    const uint8_t bp = omgp::ADDR_backplane_min;
    const uint8_t node = rig.bring_up(bp);
    rig.arm(bp, 0xFFu, flood_script(true));
    rig.set_drain_each_step(false);
    rig.detach_transcript();
    rig.recorder().deliveries.reserve(rig.recorder().deliveries.size() + 1024u);

    // Fewer than one event-rate window of simulated time, so the flood fault raised inside the
    // scope is still in force when it closes — the state the last two checks below read.
    const int steps = static_cast<int>((kEventRateWindowUs / byte_us()) - 100u);
    HEAP_FREE_SCOPE({
        for (int i = 0; i < steps; ++i) {
            rig.step_bare();
        }
        rig.engine().drain_callbacks(32);
    });
    // The guard would pass vacuously if nothing had happened: this ran the whole path, including
    // the flood fault.
    CHECK(rig.recorder().count(LifecycleKind::ModuleEvent) >= 1u);
    CHECK(rig.recorder().count(LifecycleKind::NodeEventFault) == 1u);
    CHECK(Bk::node(rig.engine(), node).event_faulted);
}

// =================================================================================================
// Mutation-kill cases (tools/mutate.sh gate on the T039 diff, Refs #710).
//
// WHAT THESE ARE: white-box cases that call the engine's own private steps through
// CoreEngineTestSeam on a bare engine, so that each boundary the rig-level cases above cross only
// by luck is crossed on purpose. They are an oracle for the arithmetic and comparison boundaries of
// the event path (the sweep cursor, the detail arena, the FR-026 window) and for NOTHING at message
// level — that stays the cases above. Every expectation is stated in terms of the spec constant or
// the structure it comes from, never a number copied from the implementation.
// =================================================================================================

namespace {

namespace l3 = omgp::l3;

// A bare engine: no scripts, no superframe run. Modules are placed straight into the node table.
class Bare {
  public:
    Bare() : node_(clock_) {
        callbacks_.ctx = &recorder_;
        callbacks_.on_lifecycle = &Recorder::on_lifecycle;
        callbacks_.on_param_result = &Recorder::on_param_result;
        engine_ = std::make_unique<CoreEngine>(node_, clock_, omgp::ADDR_host, callbacks_);
    }
    CoreEngine& engine() {
        return *engine_;
    }
    Recorder& recorder() {
        return recorder_;
    }
    void module(uint8_t id, uint8_t backplane_addr,
                DiscoveryState state = DiscoveryState::Discovered) {
        omgp::core::NodeRecord& n = Bk::node(*engine_, id);
        n.in_use = true;
        n.backplane_addr = backplane_addr;
        n.discovery = state;
    }
    // The engine's own queue, which `sweep`/`queue_drain` push to, as a node-id list in order.
    std::vector<uint8_t> queued() {
        std::vector<uint8_t> out;
        const size_t n = Bk::event_queued(*engine_);
        // Read by popping through drain is not possible without issuing; the head is enough for
        // every case here (each queues at most one item before asserting).
        if (n > 0) {
            out.push_back(Bk::queue_head(*engine_));
        }
        return out;
    }

  private:
    FakeClock clock_;
    MockL3Node node_;
    Recorder recorder_;
    CoreCallbacks callbacks_{};
    std::unique_ptr<CoreEngine> engine_;
};

// One L3 message as link::Master would hand it up. The buffer is filled with a guard byte so a
// read PAST the message — which a view into link::Master's buffer permits — is detectable.
struct Msg {
    uint8_t buf[128];
    omgp::link::MasterEvent ev{};
};

void build_msg(Msg& m, uint8_t opcode, uint8_t node_id, const uint8_t* payload, size_t plen) {
    std::memset(m.buf, 0xEE, sizeof m.buf);
    l3::Header h{};
    h.opcode = opcode;
    h.node_id = node_id;
    h.seq = 0;
    h.flags = omgp::FLAG_response;
    h.payload_len = static_cast<uint8_t>(plen);
    size_t w = 0;
    REQUIRE(l3::encode_header(h, m.buf, sizeof m.buf, w) == l3::Status::Ok);
    if (plen > 0) {
        std::memcpy(m.buf + w, payload, plen);
    }
    m.ev.kind = omgp::link::MasterEvent::Answered;
    m.ev.response = {};
    m.ev.response.payload = m.buf;
    m.ev.response.len = static_cast<uint8_t>(w + plen);
}

void status_msg(Msg& m, uint8_t node_id, uint8_t pending) {
    l3::StatusBlock b{};
    b.state = omgp::STATE_READY;
    b.event_pending = pending;
    uint8_t p[16];
    size_t w = 0;
    REQUIRE(l3::encode_status_block(b, p, sizeof p, w) == l3::Status::Ok);
    build_msg(m, omgp::OP_GET_STATUS, node_id, p, w);
}

void event_msg(Msg& m, uint8_t node_id, uint8_t remaining, const std::vector<uint8_t>& detail) {
    l3::GetEventResp r{};
    r.event_type = omgp::EVT_FAULT_RAISED;
    r.remaining_count = remaining;
    r.detail.data = detail.empty() ? nullptr : detail.data();
    r.detail.len = static_cast<uint8_t>(detail.size());
    uint8_t p[96];
    size_t w = 0;
    REQUIRE(l3::encode_get_event_resp(r, p, sizeof p, w) == l3::Status::Ok);
    build_msg(m, omgp::OP_GET_EVENT, node_id, p, w);
}

constexpr uint8_t kBp = omgp::ADDR_backplane_min;
constexpr uint8_t kMin = omgp::ADDR_module_min;
constexpr uint8_t kMax = omgp::ADDR_module_max;

// One sweep step over `modules` (all Discovered behind `kBp`) from `cursor`: the module it queued
// (0 when none) and the cursor it left.
struct SweepResult {
    uint8_t queued;
    uint8_t cursor;
};
SweepResult sweep_from(uint8_t cursor, const std::vector<uint8_t>& modules) {
    Bare b;
    for (const uint8_t id : modules) {
        b.module(id, kBp);
    }
    Bk::backplane(b.engine(), kBp).event_sweep_next_id = cursor;
    Bk::sweep(b.engine(), kBp);
    const std::vector<uint8_t> q = b.queued();
    return SweepResult{q.empty() ? uint8_t{0} : q[0],
                       Bk::backplane(b.engine(), kBp).event_sweep_next_id};
}

} // namespace

TEST_CASE("FR-015 attribution: the sweep resumes at its cursor, wraps once, and leaves the cursor "
          "one past the module it queued [events][us4][mutation]") {
    // The cursor is the rotation's only state; each expectation below is the rotation's own
    // definition ("the first Discovered module from the cursor, wrapping once; the cursor then
    // names the id after it, wrapping at the span's end").
    SECTION(
        "a cursor mid-span picks the next Discovered module at or after it, not an earlier one") {
        const SweepResult r = sweep_from(0x50, {0x20, 0x60});
        CHECK(r.queued == 0x60);
        CHECK(r.cursor == 0x61);
    }
    SECTION("a cursor past every Discovered module wraps to the first one") {
        const SweepResult r = sweep_from(0x50, {0x20, 0x40});
        CHECK(r.queued == 0x20);
        CHECK(r.cursor == 0x21);
    }
    SECTION("a cursor on the span's last id is a real position, not an out-of-span restart") {
        const SweepResult r = sweep_from(kMax, {kMin, kMax});
        CHECK(r.queued == kMax);
        CHECK(r.cursor == kMin); // one past the last id wraps to the first
    }
    SECTION("a cursor on the span's first id starts there") {
        const SweepResult r = sweep_from(kMin, {kMin, 0x30});
        CHECK(r.queued == kMin);
        CHECK(r.cursor == kMin + 1u);
    }
    SECTION("a cursor below the span restarts at the first id") {
        const SweepResult r = sweep_from(0x00, {kMin, 0x30});
        CHECK(r.queued == kMin);
        CHECK(r.cursor == kMin + 1u);
    }
    SECTION("a cursor above the span restarts at the first id") {
        const SweepResult r = sweep_from(0xFF, {kMin, 0x30});
        CHECK(r.queued == kMin);
        CHECK(r.cursor == kMin + 1u);
    }
    SECTION("the module found several steps past the cursor is queued, and the cursor follows it") {
        const SweepResult r = sweep_from(kMin, {0x12});
        CHECK(r.queued == 0x12);
        CHECK(r.cursor == 0x13);
    }
    SECTION("the last id in the span, found by walking to it, leaves the cursor wrapped") {
        const SweepResult r = sweep_from(0x70, {kMax});
        CHECK(r.queued == kMax);
        CHECK(r.cursor == kMin);
    }
    SECTION("no Discovered module behind the backplane queues nothing and leaves the cursor") {
        const SweepResult r = sweep_from(0x40, {});
        CHECK(r.queued == 0);
        CHECK(r.cursor == 0x40);
    }
}

TEST_CASE("FR-015 attribution: a status poll's count sweeps the polled backplane, and only that "
          "one [events][us4][mutation]") {
    Msg m;
    SECTION("a poll that names a Discovered MODULE queues that module's drain") {
        Bare b;
        b.module(0x20, kBp);
        status_msg(m, 0x20, 1);
        Bk::set_tx(b.engine(), kBp, 0x20);
        Bk::on_status(b.engine(), m.ev, 0);
        REQUIRE(Bk::event_queued(b.engine()) == 1);
        CHECK(Bk::queue_head(b.engine()) == 0x20);
    }
    SECTION("a poll that names a module not yet Discovered queues nothing") {
        Bare b;
        b.module(0x20, kBp, DiscoveryState::Identifying);
        status_msg(m, 0x20, 1);
        Bk::set_tx(b.engine(), kBp, 0x20);
        Bk::on_status(b.engine(), m.ev, 0);
        CHECK(Bk::event_queued(b.engine()) == 0);
    }
    SECTION("a poll that names a module with no table entry queues nothing") {
        Bare b;
        status_msg(m, 0x20, 1);
        Bk::set_tx(b.engine(), kBp, 0x20);
        Bk::on_status(b.engine(), m.ev, 0);
        CHECK(Bk::event_queued(b.engine()) == 0);
    }
    SECTION("a poll of a backplane advances THAT backplane's cursor and no other's") {
        Bare b;
        const uint8_t other = kBp + 2u;
        b.module(0x20, other);
        status_msg(m, 0, 1); // node id 0 is not a module: the count is a backplane summary
        Bk::set_tx(b.engine(), other, 0);
        Bk::on_status(b.engine(), m.ev, 0);
        REQUIRE(Bk::event_queued(b.engine()) == 1);
        CHECK(Bk::queue_head(b.engine()) == 0x20);
        CHECK(Bk::backplane(b.engine(), other).event_sweep_next_id == 0x21);
        CHECK(Bk::backplane(b.engine(), kBp).event_sweep_next_id == kMin); // untouched
    }
}

TEST_CASE("FR-025/FR-026: the drain loop drops what is gone and keeps what is faulted "
          "[events][us4][mutation]") {
    SECTION("an item whose node left Discovered is dropped and its pending flag cleared") {
        Bare b;
        b.module(0x20, kBp);
        Bk::queue_drain(b.engine(), 0x20);
        REQUIRE(Bk::event_queued(b.engine()) == 1);
        REQUIRE(Bk::node(b.engine(), 0x20).event_drain_pending);
        Bk::node(b.engine(), 0x20).discovery =
            DiscoveryState::Identifying; // the slot re-identifies
        Bk::drain_events(b.engine(), 0);
        CHECK(Bk::event_queued(b.engine()) == 0);
        CHECK_FALSE(Bk::node(b.engine(), 0x20).event_drain_pending);
    }
    SECTION("an item for a FAULTED node stays queued (rotated, not dropped, not issued)") {
        Bare b;
        b.module(0x20, kBp);
        Bk::queue_drain(b.engine(), 0x20);
        Bk::node(b.engine(), 0x20).event_faulted = true;
        Bk::drain_events(b.engine(), 0);
        REQUIRE(Bk::event_queued(b.engine()) == 1);
        CHECK(Bk::queue_head(b.engine()) == 0x20);
        CHECK(Bk::node(b.engine(), 0x20).event_drain_pending); // still owed
    }
}

TEST_CASE("FR-016/FR-021: a delivered detail is copied exactly len bytes into the arena, which "
          "advances one row per copied delivery and wraps [events][us4][mutation]") {
    const size_t lens[] = {1u, 7u, static_cast<size_t>(kMaxEventDetailBytes)};
    for (const size_t len : lens) {
        DYNAMIC_SECTION("detail length " << len) {
            Bare b;
            b.module(0x20, kBp);
            std::vector<uint8_t> detail(len);
            for (size_t i = 0; i < len; ++i) {
                detail[i] = static_cast<uint8_t>(0x41u + i);
            }
            Msg m;
            event_msg(m, 0x20, 0, detail);
            Bk::set_tx(b.engine(), kBp, 0x20);
            Bk::on_event_drain(b.engine(), m.ev, 0);
            CHECK(Bk::arena_next(b.engine()) == 1); // one row consumed
            for (size_t i = 0; i < len; ++i) {
                CHECK(Bk::arena_byte(b.engine(), 0, i) == detail[i]);
            }
            // The byte at [len] is whatever followed the detail in the buffer (the guard 0xEE
            // here); the arena row must not have taken it. At the row's own end [len] is the
            // NEXT row's first byte.
            if (len < kMaxEventDetailBytes) {
                CHECK(Bk::arena_byte(b.engine(), 0, len) == 0);
            } else {
                CHECK(Bk::arena_byte(b.engine(), 1, 0) == 0);
            }
            b.engine().drain_callbacks();
            REQUIRE(b.recorder().deliveries.size() == 1);
            CHECK(b.recorder().deliveries[0].detail_len == len);
            // remaining_count 0: the node's backlog is over, nothing is re-queued (FR-015).
            CHECK(Bk::event_queued(b.engine()) == 0);
        }
    }
    SECTION("the arena wraps after its last row") {
        Bare b;
        b.module(0x20, kBp);
        Bk::set_arena_next(b.engine(), omgp::LIMIT_max_nodes - 1u);
        Msg m;
        event_msg(m, 0x20, 0, {0x55});
        Bk::set_tx(b.engine(), kBp, 0x20);
        Bk::on_event_drain(b.engine(), m.ev, 0);
        CHECK(Bk::arena_byte(b.engine(), omgp::LIMIT_max_nodes - 1u, 0) == 0x55);
        CHECK(Bk::arena_next(b.engine()) == 0);
    }
    SECTION("an event with no detail consumes no arena row") {
        Bare b;
        b.module(0x20, kBp);
        Msg m;
        event_msg(m, 0x20, 0, {});
        Bk::set_tx(b.engine(), kBp, 0x20);
        Bk::on_event_drain(b.engine(), m.ev, 0);
        CHECK(Bk::arena_next(b.engine()) == 0);
        b.engine().drain_callbacks();
        CHECK(b.recorder().deliveries.size() == 1);
    }
    SECTION("a full pending ring copies nothing into the arena and advances nothing") {
        Bare b;
        b.module(0x20, kBp);
        LifecycleEvent filler{};
        filler.kind = LifecycleKind::ModuleEvent;
        filler.node_id = 0x30;
        while (Bk::enqueue(b.engine(), filler)) {
        }
        Msg m;
        event_msg(m, 0x20, 0, {0x77, 0x78});
        Bk::set_tx(b.engine(), kBp, 0x20);
        Bk::on_event_drain(b.engine(), m.ev, 0);
        CHECK(Bk::arena_byte(b.engine(), 0, 0) == 0);
        CHECK(Bk::arena_next(b.engine()) == 0);
    }
}

TEST_CASE("FR-015/FR-025: remaining_count above zero re-queues the node, zero does not "
          "[events][us4][mutation]") {
    Bare b;
    b.module(0x20, kBp);
    Msg m;
    event_msg(m, 0x20, 1, {});
    Bk::set_tx(b.engine(), kBp, 0x20);
    Bk::on_event_drain(b.engine(), m.ev, 0);
    REQUIRE(Bk::event_queued(b.engine()) == 1);
    CHECK(Bk::queue_head(b.engine()) == 0x20);
}

TEST_CASE("FR-026: the drained-event count saturates, a fresh window starts at the fault, and a "
          "full window inside budget clears it [events][us4][mutation]") {
    SECTION("the count saturates at 255 rather than wrapping to zero") {
        Bare b;
        b.module(0x20, kBp);
        omgp::core::NodeRecord& n = Bk::node(b.engine(), 0x20);
        n.event_faulted = true; // already faulted: the count is not reset by a new fault
        n.events_drained_this_window = UINT8_MAX;
        Bk::note_drained(b.engine(), 0x20, 0);
        CHECK(n.events_drained_this_window == UINT8_MAX);
    }
    SECTION("the drain that takes a node over budget faults it and starts a fresh window") {
        Bare b;
        b.module(0x20, kBp);
        omgp::core::NodeRecord& n = Bk::node(b.engine(), 0x20);
        n.events_drained_this_window = kEventRateBudgetPerWindow; // the next one is over
        const uint64_t now = 12345;
        Bk::note_drained(b.engine(), 0x20, now);
        CHECK(n.event_faulted);
        CHECK(n.events_drained_this_window == 0);
        CHECK(n.event_rate_window_start_us == now);
        b.engine().drain_callbacks();
        CHECK(b.recorder().count_for(LifecycleKind::NodeEventFault, 0x20) == 1);
    }
    SECTION("a window that has run exactly kEventRateWindowUs is closed") {
        Bare b;
        b.module(0x20, kBp);
        omgp::core::NodeRecord& n = Bk::node(b.engine(), 0x20);
        n.event_rate_window_start_us = 1000;
        n.events_drained_this_window = 2;
        Bk::roll(b.engine(), 0x20, 1000 + kEventRateWindowUs);
        CHECK(n.events_drained_this_window == 0);
        CHECK(n.event_rate_window_start_us == 1000 + kEventRateWindowUs);
    }
    SECTION("a window one microsecond short of kEventRateWindowUs is still open") {
        Bare b;
        b.module(0x20, kBp);
        omgp::core::NodeRecord& n = Bk::node(b.engine(), 0x20);
        n.event_rate_window_start_us = 1000;
        n.events_drained_this_window = 2;
        Bk::roll(b.engine(), 0x20, 1000 + kEventRateWindowUs - 1u);
        CHECK(n.events_drained_this_window == 2);
        CHECK(n.event_rate_window_start_us == 1000);
    }
    SECTION("a faulted node that drained exactly the budget is back within budget: cleared") {
        Bare b;
        b.module(0x20, kBp);
        omgp::core::NodeRecord& n = Bk::node(b.engine(), 0x20);
        n.event_faulted = true;
        n.event_rate_window_start_us = 0;
        n.events_drained_this_window = kEventRateBudgetPerWindow; // AT budget is within it
        Bk::roll(b.engine(), 0x20, kEventRateWindowUs);
        CHECK_FALSE(n.event_faulted);
        b.engine().drain_callbacks();
        CHECK(b.recorder().count_for(LifecycleKind::NodeEventFaultCleared, 0x20) == 1);
    }
    SECTION("a faulted node that drained OVER budget in the window stays faulted") {
        Bare b;
        b.module(0x20, kBp);
        omgp::core::NodeRecord& n = Bk::node(b.engine(), 0x20);
        n.event_faulted = true;
        n.event_rate_window_start_us = 0;
        n.events_drained_this_window = kEventRateBudgetPerWindow + 1u;
        Bk::roll(b.engine(), 0x20, kEventRateWindowUs);
        CHECK(n.event_faulted);
    }
    SECTION("every module id in the span has its window rolled, first to last") {
        Bare b;
        const uint8_t ids[] = {kMin, static_cast<uint8_t>(kMin + 1u), kMax};
        for (const uint8_t id : ids) {
            b.module(id, kBp);
            omgp::core::NodeRecord& n = Bk::node(b.engine(), id);
            n.event_faulted = true;
            n.event_rate_window_start_us = 0;
            n.events_drained_this_window = 0;
        }
        Bk::tick(b.engine(), kEventRateWindowUs);
        for (const uint8_t id : ids) {
            CHECK_FALSE(Bk::node(b.engine(), id).event_faulted);
        }
    }
}

TEST_CASE("FR-015: a node whose drain has completed can be drained again by a later burst "
          "[events][us4][mutation]") {
    // A completed GET_EVENT clears the node's in-flight flag (on_complete, whatever it answered);
    // were it left set, the node would never be offered again and a second burst would be lost.
    Rig rig;
    const uint8_t id = rig.bring_up(omgp::ADDR_backplane_min);
    rig.arm(omgp::ADDR_backplane_min, 1, {event_of(omgp::EVT_FAULT_RAISED, 0, {0x01})});
    rig.run_until([&] { return rig.recorder().events_of(id).size() == 1; }, 200u);
    rig.arm(omgp::ADDR_backplane_min, 1, {event_of(omgp::EVT_FAULT_RAISED, 0, {0x02})});
    rig.run_until([&] { return rig.recorder().events_of(id).size() == 2; }, 200u);
    const std::vector<Delivery> got = rig.recorder().events_of(id);
    REQUIRE(got.size() == 2);
    CHECK(got[1].detail_len == 1);
    CHECK(got[1].detail[0] == 0x02);
}
