// User Story 1 — a newly powered rig discovers itself (spec 003 T017, #688). The four
// Acceptance Scenarios of specs/003-host-core-engine/spec.md User Story 1, plus SC-001 and
// SC-005, driven against a REAL omgp::core::CoreEngine over MockL3Node (T012, contracts/
// mock-l3-node.md) and F2's FakeClock — no stubbed engine, no pre-decoded messages, every byte
// of every request and answer built by the real l3/ and link/ codecs.
//
// trunk §6 is the schedule under test: each superframe issues one status poll per enrolled
// backplane (GET_STATUS and BP_SLOT_MAP on alternate superframes) before anything else, then
// demand traffic under the budget, then exactly one enrolment probe. trunk §5 is the addressing
// (0x01-0x0F backplanes; module ids never appear as L2 addresses, so every module request is
// addressed to its backplane and bridged by L3 node id, §8). protocol-l3 §3.1 is the message
// set (IDENTIFY, READ_DESC, GET_STATUS, BP_SLOT_MAP, PING) and §4 the descriptor TLV bytes.
//
// Time is explicit (CLAUDE.md rule 3): this file includes no <chrono>, calls nothing that
// sleeps, and reads no wall clock — simulated time enters only through FakeClock, and every
// decision the engine makes reaches it as run_superframe()'s own now_us argument. Every
// protocol value is a generated symbol (rule 4); the STATIC_REQUIREs below pin that for the two
// this file derives rather than names.
//
// WHAT THIS FILE IS AN ORACLE FOR, and what it is not (CLAUDE.md rule 11). Every claim here is
// about discovery: enrolment, node-id assignment, IDENTIFY, the descriptor read and cache, the
// presence half of the lifecycle stream, and the determinism of the resulting transcript. It is
// NOT the oracle for the superframe budget under load (T025), parameter traffic (T026), channel
// switching (T033), event latency (T038) or the SUSPECT/OFFLINE/BusFault contract (T041) —
// each of those has its own test_core_*.cpp and this file asserts none of them.
//
// TWO PLACES WHERE THE ACCEPTANCE CRITERIA AND THE AVAILABLE INSTRUMENT DIVERGE, stated rather
// than papered over (both are in the PR body too):
//
//   1. "one slot's module scripted SilenceStep for IDENTIFY" (AS3) is not expressible.
//      MockL3Node scripts one trunk ADDRESS, not one slot (set_script() takes `node <
//      kAddrCount`), and SilenceStep is a node-wide wildcard — a SilenceStep in a backplane's
//      script silences that backplane's status polls too, which is a different scenario.
//      trunk §8 also forbids the behaviour it would model: a backplane "MUST begin its response
//      within T_turn ... with the module's reply if it is ready, otherwise ERR_BUSY ... or
//      ERR_UNKNOWN_TARGET for a slot with no live module", and "MUST NOT stall the trunk". So
//      the unanswering module is scripted the way trunk §8 says a real one appears: its
//      backplane answers ERR_UNKNOWN_TARGET on its behalf, for ever. What AS3 asks to be
//      OBSERVED is asserted unchanged — that node stays Identifying, the other eleven reach
//      Discovered, and it is retried at distinct superframes rather than abandoned. A genuine
//      trunk-level timeout is exercised separately, by a backplane that goes silent.
//
//   2. SC-005's clock granularity is exercised over granularities that divide one byte time.
//      spec FR-027 requires the demand budget to be computed from the MEASURED duration of the
//      superframe's own traffic, so a cadence coarse enough to change those measurements
//      changes how many demand items a superframe admits — and with it which superframe a
//      given request lands in. The two requirements are in tension; the tension is recorded in
//      docs/OPEN-QUESTIONS.md (2026-09-28). This file fixes the wire's own event instants (all
//      steps divide the byte time) and varies only the caller's cadence, which is what
//      discriminates a scheduler that reads elapsed time or call counts from one that reads
//      only its now_us argument. That is a labelled control, not a proof of SC-005 at every
//      cadence.
#include "core/core_engine.hpp"

#include "catch_amalgamated.hpp"
#include "fake_clock.hpp"
#include "l3/l3_descriptor.hpp"
#include "l3/l3_header.hpp"
#include "l3/l3_payload.hpp"
#include "l3/l3_types.hpp"
#include "link/link_types.hpp"
#include "mock_l3_node.hpp"
#include "omgp_protocol.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// --- the test-only transcript seam (T023) ------------------------------------------------------
// core/core_engine.hpp declares `struct CoreEngineTestSeam;` and befriends it without defining
// it; this is that definition, and it installs the transcript sink and nothing else. Deliberately
// not a public setter: the transcript is a test instrument, and a friend declaration emits no
// code for a build that never defines this type.
namespace omgp {
namespace core {
struct CoreEngineTestSeam {
    static void set_transcript(CoreEngine& engine, TranscriptFn fn, void* ctx) {
        engine.transcript_ = fn;
        engine.transcript_ctx_ = ctx;
    }
    // The one hostile READ_DESC answer MockL3Node cannot express: its build_desc_chunk() echoes
    // the REQUEST's own offset (tests/support/mock_l3_node.cpp, contracts/mock-l3-node.md), so a
    // node answering an offset it was not asked for is not scriptable, and protocol-l3 §3.1's
    // offset-continuity rule — which lives in on_desc_chunk() and nowhere else — would otherwise
    // be asserted by comment only. `ev` is a real, codec-encoded response: the same input
    // link::Master hands the engine, built by the same l3/ encoders. What this does NOT exercise,
    // stated rather than implied: the L2 path that would carry such a frame to the engine.
    static void deliver_desc_chunk(CoreEngine& engine, const link::MasterEvent& ev,
                                   uint64_t now_us) {
        engine.on_desc_chunk(ev, now_us);
    }
    // Whether the transaction currently outstanding at link::Master is a READ_DESC (trunk §3:
    // one at a time), so a case can inject the answer above at the only instant a real one could
    // arrive, rather than inferring that instant from the transcript.
    static bool read_desc_outstanding(const CoreEngine& engine) {
        return engine.tx_kind_ == CoreEngine::TxKind::ReadDesc;
    }
    // The descriptor cache itself (spec FR-008, R-12), for the one property no rig-level run can
    // pin as a PROPERTY rather than as a symptom: which entry claim_descriptor() is willing to
    // hand out. A rig shows the consequence (a module losing its reassembly buffer); these show
    // the rule. Read-only apart from claim(), which is the very call under test.
    static DescriptorCacheEntry* claim(CoreEngine& engine, uint8_t module_type, uint16_t desc_len,
                                       uint16_t crc) {
        return engine.claim_descriptor(module_type, desc_len, crc);
    }
    static DescriptorCacheEntry* find(CoreEngine& engine, uint8_t module_type, uint16_t desc_len,
                                      uint16_t crc) {
        return engine.find_descriptor(module_type, desc_len, crc);
    }
    static size_t cache_entries(const CoreEngine& engine) {
        return sizeof engine.descriptors_ / sizeof engine.descriptors_[0];
    }
    static const DescriptorCacheEntry& entry(const CoreEngine& engine, size_t i) {
        return engine.descriptors_[i];
    }
    // A node record put straight into the state a chunked READ_DESC leaves it in. Not a shortcut
    // around the wire: the rig-level cases above reach this state through real IDENTIFY and
    // READ_DESC traffic, and this seam only lets a case reach it for all 32 cache entries at once,
    // which no scriptable rig can (MockL3Node scripts one identity per trunk address).
    static void put_reading(CoreEngine& engine, uint8_t node_id, uint8_t module_type,
                            uint16_t desc_len, uint16_t crc) {
        NodeRecord& node = engine.nodes_[CoreEngine::node_index(node_id)];
        node.in_use = true;
        node.backplane_addr = ADDR_backplane_min;
        node.module_type = module_type;
        node.desc_len = desc_len;
        node.desc_crc = crc;
        node.discovery = DiscoveryState::ReadingDescriptor;
    }

    // --- mutation-kill accessors -------------------------------------------------------------
    // Reference-returning views of the tables and cursors the engine's bookkeeping lives in, so a
    // case can pin a field a rig-level run only reaches by coincidence (a stored value that every
    // downstream read happens to tolerate). Test-only for the same reason the seam above is: this
    // type is defined by this binary alone, so no other build carries any of it.
    static NodeRecord& node(CoreEngine& engine, uint8_t node_id) {
        return engine.nodes_[CoreEngine::node_index(node_id)];
    }
    static omgp::core::BackplaneRecord& backplane(CoreEngine& engine, uint8_t addr) {
        return engine.backplanes_[CoreEngine::backplane_index(addr)];
    }
    static DescriptorCacheEntry& entry_mut(CoreEngine& engine, size_t i) {
        return engine.descriptors_[i];
    }
    static omgp::core::SuperframeBudget& budget(CoreEngine& engine) {
        return engine.budget_;
    }
    static uint32_t& superframe(CoreEngine& engine) {
        return engine.superframe_;
    }
    static bool& demand_issued(CoreEngine& engine) {
        return engine.demand_issued_;
    }
    static bool& probe_issued(CoreEngine& engine) {
        return engine.probe_issued_;
    }
    static bool& superframe_open(CoreEngine& engine) {
        return engine.superframe_open_;
    }
    static bool& desc_held(CoreEngine& engine) {
        return engine.desc_held_;
    }
    static omgp::core::DescChunkItem& desc_hold(CoreEngine& engine) {
        return engine.desc_hold_;
    }
    static uint8_t& l3_seq(CoreEngine& engine) {
        return engine.l3_seq_;
    }
    static uint8_t& identify_cursor(CoreEngine& engine) {
        return engine.identify_cursor_;
    }
    static uint8_t& desc_retry_cursor(CoreEngine& engine) {
        return engine.desc_retry_cursor_;
    }
    static uint8_t& poll_cursor(CoreEngine& engine) {
        return engine.poll_cursor_;
    }
    static uint32_t& wire_rate(CoreEngine& engine) {
        return engine.wire_rate_;
    }
    static uint16_t& tx_offset(CoreEngine& engine) {
        return engine.tx_offset_;
    }
    static uint8_t& tx_node(CoreEngine& engine) {
        return engine.tx_node_;
    }
    static uint64_t& tx_issued_us(CoreEngine& engine) {
        return engine.tx_issued_us_;
    }
    static bool tx_idle(const CoreEngine& engine) {
        return engine.tx_kind_ == CoreEngine::TxKind::None;
    }
    static bool phase_closed(const CoreEngine& engine) {
        return engine.phase_ == CoreEngine::Phase::Closed;
    }
    static size_t desc_queue_size(const CoreEngine& engine) {
        return engine.desc_queue_.size();
    }
    static bool push_desc(CoreEngine& engine, uint8_t node_id, uint16_t offset) {
        return engine.desc_queue_.push(omgp::core::DescChunkItem{node_id, offset});
    }
    static bool pop_desc(CoreEngine& engine, omgp::core::DescChunkItem& out) {
        return engine.desc_queue_.pop(out);
    }

    // Wrappers over the private operations a case drives directly. Each returns what the
    // operation returns; none adds behaviour.
    static uint64_t cost_estimate(const CoreEngine& engine, uint64_t last_measured_us) {
        return engine.cost_estimate(last_measured_us);
    }
    static bool admit_demand(const CoreEngine& engine, uint64_t estimate) {
        return engine.admit_demand(estimate);
    }
    static void mark_discovered(CoreEngine& engine, NodeRecord& node, uint8_t node_id,
                                const DescriptorCacheEntry& entry) {
        engine.mark_discovered(node, node_id, entry);
    }
    static void release_descriptor(CoreEngine& engine, NodeRecord& node) {
        engine.release_descriptor(node);
    }
    static void attach_descriptor(CoreEngine& engine, DescriptorCacheEntry& entry) {
        engine.attach_descriptor(entry);
    }
    static bool has_reader(const CoreEngine& engine, const DescriptorCacheEntry& entry) {
        return engine.descriptor_has_reader(entry);
    }
    static void on_identify(CoreEngine& engine, const link::MasterEvent& ev, uint64_t now_us) {
        engine.on_identify(ev, now_us);
    }
    static void complete_request(CoreEngine& engine, const link::MasterEvent& ev, uint64_t now_us) {
        engine.complete_request(ev, now_us);
    }
    static bool begin_request(CoreEngine& engine, uint8_t dst, uint8_t node_id, uint8_t opcode,
                              const uint8_t* payload, uint8_t len, uint64_t now_us) {
        return engine.begin_request(dst, node_id, opcode, payload, len,
                                    CoreEngine::TxKind::ReadDesc, now_us);
    }
    static bool begin_desc_chunk(CoreEngine& engine, uint8_t node_id, uint16_t offset,
                                 uint64_t now_us) {
        return engine.begin_desc_chunk(node_id, offset, now_us);
    }
    // 0 = Nothing, 1 = Issued, 2 = Refused: CoreEngine::Resume is private to the engine.
    static int resume_desc_read(CoreEngine& engine, uint8_t node_id, uint64_t now_us) {
        return static_cast<int>(engine.resume_desc_read(node_id, node(engine, node_id), now_us));
    }
    static int scan_identify(CoreEngine& engine, uint64_t now_us) {
        return static_cast<int>(engine.scan_identify(now_us));
    }
    static int retry_stalled_desc(CoreEngine& engine, uint64_t now_us) {
        return static_cast<int>(engine.retry_stalled_desc(now_us));
    }
    static bool issue_demand(CoreEngine& engine, uint64_t now_us) {
        return engine.issue_demand(now_us);
    }
    static void open_superframe(CoreEngine& engine, uint64_t now_us) {
        engine.open_superframe(now_us);
    }
};
} // namespace core
} // namespace omgp

using omgp::core::CoreCallbacks;
using omgp::core::CoreEngine;
using omgp::core::DiscoveryState;
using omgp::core::LifecycleEvent;
using omgp::core::LifecycleKind;
using omgp::core::TranscriptEntry;
using omgp_test::DescChunkStep;
using omgp_test::ErrorStep;
using omgp_test::FakeClock;
using omgp_test::IdentifyStep;
using omgp_test::L3Step;
using omgp_test::MockL3Node;
using omgp_test::SilenceStep;
using omgp_test::SlotMapStep;
using omgp_test::StatusStep;

namespace {

// research.md R-09 as corrected 2026-09-21: READ_DESC's response is `u16 offset, u8 len,
// u8[len]`, so the largest chunk is what is left of LIMIT_max_l3_payload after that 3-byte
// head. Derived, never restated — the number has already moved once (61 -> 56).
constexpr uint8_t kDescChunkMax = static_cast<uint8_t>(omgp::LIMIT_max_l3_payload - 3);

// CLAUDE.md rule 4, made checkable rather than asserted in prose: the two values this file
// derives are derived from generated symbols, and a YAML edit that moves either moves these.
static_assert(kDescChunkMax + 3 == omgp::LIMIT_max_l3_payload,
              "the READ_DESC chunk size is LIMIT_max_l3_payload minus its own response head");
static_assert(omgp::ADDR_module_min > omgp::ADDR_backplane_max,
              "this file's rigs assume module ids and backplane addresses do not overlap "
              "(protocol-l3 §2)");

uint64_t byte_us() {
    return omgp::link::byte_time_us(omgp::TRUNK_bit_rate);
}

// --- descriptors (protocol-l3 §4) --------------------------------------------------------------

// A descriptor blob plus the CRC an IDENTIFY response must advertise for it (§4.1: desc_crc is
// l3::descriptor_crc over the whole blob exactly as READ_DESC serves it). Built with the real
// DescriptorWriter, so a blob this file scripts is one a real module could serve.
struct Descriptor {
    std::vector<uint8_t> bytes;
    uint16_t crc = 0;
    uint16_t vendor = 0, hw_rev = 0, fw_rev = 0;
};

// Builds a descriptor of EXACTLY `total_len` bytes. The exact length matters: the READ_DESC
// coverage case counts chunks against it, and a length that is an exact multiple of the R-09
// chunk size is what makes that count sensitive to the chunk size at all — at a length that is
// not, ceil(len / max_len) is the same for max_len and max_len - 1, and an off-by-one chunk
// request would go unnoticed (measured: with the first draft's arbitrary lengths, a max_len of
// kDescChunkMax - 1 left every case green).
Descriptor make_descriptor(uint16_t vendor, uint16_t hw_rev, uint16_t fw_rev, uint8_t module_type,
                           size_t total_len) {
    uint8_t buf[omgp::LIMIT_max_descriptor_bytes];
    omgp::l3::DescriptorWriter writer(buf, sizeof buf);
    REQUIRE(writer.add_protocol(omgp::l3::ProtocolRec{1, 0}) == omgp::l3::Status::Ok);
    REQUIRE(writer.add_module_type(omgp::l3::ModuleTypeRec{module_type}) == omgp::l3::Status::Ok);
    const char* name = "unit-under-test";
    REQUIRE(writer.add_name(omgp::l3::Str{reinterpret_cast<const uint8_t*>(name),
                                          static_cast<uint8_t>(__builtin_strlen(name))}) ==
            omgp::l3::Status::Ok);
    const char* maker = "OMGP";
    REQUIRE(writer.add_manufacturer(omgp::l3::Str{reinterpret_cast<const uint8_t*>(maker),
                                                  static_cast<uint8_t>(__builtin_strlen(maker))}) ==
            omgp::l3::Status::Ok);
    REQUIRE(writer.add_model_id(omgp::l3::ModelIdRec{vendor, hw_rev, fw_rev}) ==
            omgp::l3::Status::Ok);
    // §4's forward-compatibility rule cuts both ways: an unknown record type is skipped by
    // length, so it is the honest way to make a descriptor a chosen number of bytes long
    // without inventing values for records whose ranges the codecs police. One record is
    // `u8 type, u8 len, u8[len]`, so padding to an exact total costs 2 bytes of head per record.
    REQUIRE(total_len >= writer.size() + 2u);
    size_t remaining = total_len - writer.size();
    const std::vector<uint8_t> pad(remaining, 0xA5);
    while (remaining > 0) {
        REQUIRE(remaining >= 2u); // a 1-byte tail is not expressible: every record has a head
        size_t body = remaining - 2u;
        if (body > 200u) {
            body = 200u;
            if (remaining - (body + 2u) == 1u) {
                --body; // leave a tail another whole record can carry
            }
        }
        REQUIRE(writer.add_raw(0xF0, pad.data(), static_cast<uint8_t>(body)) ==
                omgp::l3::Status::Ok);
        remaining -= body + 2u;
    }
    REQUIRE(writer.size() == total_len);
    Descriptor d;
    d.bytes.assign(buf, buf + writer.size());
    d.crc = omgp::l3::descriptor_crc(d.bytes.data(), d.bytes.size());
    d.vendor = vendor;
    d.hw_rev = hw_rev;
    d.fw_rev = fw_rev;
    return d;
}

omgp::l3::IdentifyResp identify_for(const Descriptor& d, uint8_t module_type) {
    omgp::l3::IdentifyResp r{};
    r.major = 1;
    r.minor = 0;
    r.module_type = module_type;
    r.desc_len = static_cast<uint16_t>(d.bytes.size());
    r.desc_crc = d.crc;
    return r;
}

// The step that answers every READ_DESC for a backplane against this blob, honouring the
// request's own offset and max_len (contracts/mock-l3-node.md).
L3Step desc_step(const Descriptor& d) {
    return L3Step::of(DescChunkStep{d.bytes.data(), static_cast<uint16_t>(d.bytes.size())});
}

omgp::l3::StatusBlock ready_status() {
    omgp::l3::StatusBlock b{};
    b.state = omgp::STATE_READY; // protocol-l3 §3.3: a value the codec's own range check accepts
    b.active_channel = 0;
    b.bypass = 0;
    b.fault_code = 0;
    b.uptime_s = 1;
    b.event_pending = 0;
    return b;
}

// --- the transcript (T023) ---------------------------------------------------------------------

struct Transcript {
    std::vector<TranscriptEntry> lines;

    static void record(void* ctx, const TranscriptEntry& entry) {
        static_cast<Transcript*>(ctx)->lines.push_back(entry);
    }

    // One line per Master::begin: superframe number, opcode, dst, node_id. Compared as a whole
    // ORDERED string, never as a multiset and never by line count (AS2/SC-001).
    std::string text() const {
        std::string out;
        char buf[64];
        for (const TranscriptEntry& e : lines) {
            std::snprintf(buf, sizeof buf, "sf=%06u op=%02X dst=%02X node=%02X\n",
                          static_cast<unsigned>(e.superframe), static_cast<unsigned>(e.opcode),
                          static_cast<unsigned>(e.dst), static_cast<unsigned>(e.node_id));
            out += buf;
        }
        return out;
    }

    // The same lines with the superframe number dropped: the SEQUENCE OF OPERATIONS alone.
    std::string operations() const {
        std::string out;
        char buf[48];
        for (const TranscriptEntry& e : lines) {
            std::snprintf(buf, sizeof buf, "op=%02X dst=%02X node=%02X\n",
                          static_cast<unsigned>(e.opcode), static_cast<unsigned>(e.dst),
                          static_cast<unsigned>(e.node_id));
            out += buf;
        }
        return out;
    }

    size_t count(uint8_t opcode) const {
        size_t n = 0;
        for (const TranscriptEntry& e : lines) {
            if (e.opcode == opcode)
                ++n;
        }
        return n;
    }
    // The same, for ONE node id: "this module was asked to identify itself exactly once" is a
    // per-node claim, and a rig-wide count cannot make it (another backplane's retries would
    // swamp it).
    size_t count_for(uint8_t opcode, uint8_t node_id) const {
        size_t n = 0;
        for (const TranscriptEntry& e : lines) {
            if (e.opcode == opcode && e.node_id == node_id)
                ++n;
        }
        return n;
    }
    uint32_t last_superframe() const {
        return lines.empty() ? 0u : lines.back().superframe;
    }
};

// --- the lifecycle recorder (spec FR-021, R-04) ------------------------------------------------

// Counts its OWN invocations, which is the whole point: run_superframe() must never reach it.
struct Recorder {
    unsigned calls = 0;
    std::vector<LifecycleEvent> events;

    static void on_lifecycle(void* ctx, LifecycleEvent ev) {
        Recorder* self = static_cast<Recorder*>(ctx);
        ++self->calls;
        self->events.push_back(ev);
    }
    size_t count(LifecycleKind kind) const {
        size_t n = 0;
        for (const LifecycleEvent& e : events) {
            if (e.kind == kind)
                ++n;
        }
        return n;
    }
};

// --- the scripted rig --------------------------------------------------------------------------

// One backplane's script. Every step array lives in the Rig (MockL3Node::set_script() keeps the
// caller's pointer), and the four-step shape is the same for every answering backplane:
//   SlotMapStep   — its occupancy, answered to every BP_SLOT_MAP (steady state after the first)
//   StatusStep    — its status block, answered to every GET_STATUS
//   IdentifyStep  — the identity every one of its modules reports (they are like-for-like)
//   DescChunkStep — the descriptor blob every READ_DESC is served from, honouring offset/max_len
// PING (the enrolment probe) matches no step kind and no wildcard, so the double answers
// ERR_UNKNOWN_OPCODE — a valid L2 answer, which is what enrols the backplane (trunk §6).
struct BackplaneScript {
    uint8_t addr = 0;
    uint8_t slot_count = 0;
    uint32_t occupied = 0;
    uint8_t module_type = omgp::MODULE_TYPE_CODES[0];
    const Descriptor* descriptor = nullptr;
    // trunk §8: the backplane answers on a slot's behalf when the module does not. With no
    // IdentifyStep of its own, every IDENTIFY falls back to this wildcard, for ever.
    bool modules_unanswering = false;
    bool absent = false; // scripted {SilenceStep}: nothing at this address at all
};

class Rig {
  public:
    Rig() : node_(clock_) {
        callbacks_.ctx = &recorder_;
        callbacks_.on_lifecycle = &Recorder::on_lifecycle;
        engine_ = std::make_unique<CoreEngine>(node_, clock_, omgp::ADDR_host, callbacks_);
        omgp::core::CoreEngineTestSeam::set_transcript(*engine_, &Transcript::record, &transcript_);
        // Every trunk address starts ABSENT. An unscripted MockL3Node answers
        // ERR_UNKNOWN_OPCODE to everything (contracts/mock-l3-node.md's unset-class rule), which
        // is a valid L2 answer and would enrol all fifteen addresses; {SilenceStep} is how the
        // double models "there is nothing there".
        for (uint8_t addr = omgp::ADDR_backplane_min; addr <= omgp::ADDR_backplane_max; ++addr) {
            install_absent(addr);
        }
    }

    void install(const BackplaneScript& script) {
        if (script.absent) {
            install_absent(script.addr);
            return;
        }
        std::vector<L3Step>& steps = scripts_[script.addr];
        steps.clear();
        steps.push_back(
            L3Step::of(SlotMapStep{script.slot_count, script.occupied, script.occupied}));
        steps.push_back(L3Step::of(StatusStep{ready_status()}));
        if (script.modules_unanswering) {
            // protocol-l3 §3.1 / trunk §8: ERR_UNKNOWN_TARGET is what a backplane answers for a
            // slot with no live module. The enrolment probe (PING) consumes this wildcard, and
            // from then on it is the disposition every class with no step of its own falls back
            // to — which is exactly the IDENTIFY class here.
            steps.push_back(L3Step::of(ErrorStep{omgp::ERR_UNKNOWN_TARGET}));
        } else {
            REQUIRE(script.descriptor != nullptr);
            steps.push_back(
                L3Step::of(IdentifyStep{identify_for(*script.descriptor, script.module_type)}));
            steps.push_back(desc_step(*script.descriptor));
        }
        node_.set_script(script.addr, steps.data(), steps.size());
    }

    // Replaces a backplane's slot map, leaving the rest of its script as it was. Used for the
    // occupancy-loss case: re-installing resets the double's own cursors, which is harmless —
    // every class's steady state is the same answer it was already giving.
    void reoccupy(const BackplaneScript& script, uint32_t occupied, uint32_t changed) {
        BackplaneScript s = script;
        s.occupied = occupied;
        std::vector<L3Step>& steps = scripts_[s.addr];
        steps.clear();
        steps.push_back(L3Step::of(SlotMapStep{s.slot_count, occupied, changed}));
        steps.push_back(L3Step::of(StatusStep{ready_status()}));
        REQUIRE(s.descriptor != nullptr);
        steps.push_back(L3Step::of(IdentifyStep{identify_for(*s.descriptor, s.module_type)}));
        steps.push_back(desc_step(*s.descriptor));
        node_.set_script(s.addr, steps.data(), steps.size());
    }

    // One call of the caller's loop: advance the simulated clock, let the double release whatever
    // is due, hand the engine the same instant. The only place time moves in this file — nothing
    // sleeps and nothing reads a wall clock (CLAUDE.md rule 3).
    void step(uint64_t step_us) {
        now_us_ += step_us;
        node_.advance_to(now_us_);
        engine_->run_superframe(now_us_);
    }

    // Drives the engine at `step_us` of simulated time per call until `target` superframes have
    // been opened, or until the step budget is spent.
    void run_to_superframe(uint32_t target, uint64_t step_us) {
        const uint64_t calls_per_byte_time = byte_us() / step_us + 1u;
        const uint64_t budget =
            static_cast<uint64_t>(target) * kStepsPerSuperframeBound * calls_per_byte_time;
        for (uint64_t i = 0; i < budget; ++i) {
            step(step_us);
            if (transcript_.last_superframe() > target) {
                return;
            }
        }
        FAIL("the engine did not reach superframe " << target << " within the step budget");
    }

    size_t discovered_count() const {
        size_t n = 0;
        for (uint8_t id = omgp::ADDR_module_min; id <= omgp::ADDR_module_max; ++id) {
            if (engine_->discovery_state(id) == DiscoveryState::Discovered)
                ++n;
        }
        return n;
    }
    size_t in_use_count() const {
        size_t n = 0;
        // The whole uint8_t range, not just the module window: "exactly 12 ids are in use" is
        // only a real count if an id outside the pool cannot have quietly been assigned one.
        for (unsigned id = 0; id <= 0xFFu; ++id) {
            if (engine_->node_in_use(static_cast<uint8_t>(id)))
                ++n;
        }
        return n;
    }
    std::vector<uint8_t> in_use_ids() const {
        std::vector<uint8_t> ids;
        for (unsigned id = 0; id <= 0xFFu; ++id) {
            if (engine_->node_in_use(static_cast<uint8_t>(id)))
                ids.push_back(static_cast<uint8_t>(id));
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
    // A generous per-superframe bound on run_to_superframe()'s loop: three status polls, a
    // handful of demand items and one enrolment probe whose worst case is trunk §7's full retry
    // set. It bounds the LOOP, not the engine — a scheduler that stalls fails by FAIL() here
    // rather than by hanging the suite (User Story 1 AS3's "the run still terminates").
    static constexpr uint64_t kStepsPerSuperframeBound = 1200;

    void install_absent(uint8_t addr) {
        std::vector<L3Step>& steps = scripts_[addr];
        steps.clear();
        steps.push_back(L3Step::of(SilenceStep{}));
        node_.set_script(addr, steps.data(), steps.size());
    }

    FakeClock clock_;
    MockL3Node node_;
    Recorder recorder_;
    Transcript transcript_;
    CoreCallbacks callbacks_{};
    std::unique_ptr<CoreEngine> engine_;
    std::vector<L3Step> scripts_[omgp::link::kAddrCount];
    uint64_t now_us_ = 0;
};

// The rig SC-001 and User Story 1 AS1/AS2 name: 3 backplanes, 12 modules. Each backplane's four
// slots hold like-for-like modules (one identity, one descriptor per backplane), which is what a
// real FX backplane full of one model looks like and what makes the descriptor cache observable
// in the transcript: one full READ_DESC sweep per DISTINCT descriptor, not per module.
struct ThreeBackplanes {
    // Two exact multiples of the R-09 chunk size and one that is not: the multiples make the
    // chunk COUNT sensitive to the chunk size (see make_descriptor's note), and the odd one
    // exercises a short final chunk.
    Descriptor a = make_descriptor(0x1001, 1, 1, omgp::MODULE_TYPE_CODES[0], 2u * kDescChunkMax);
    Descriptor b = make_descriptor(0x1002, 1, 2, omgp::MODULE_TYPE_CODES[1], 3u * kDescChunkMax);
    Descriptor c =
        make_descriptor(0x1003, 2, 1, omgp::MODULE_TYPE_CODES[2], 2u * kDescChunkMax - 9);

    BackplaneScript bp1{0x01, 4, 0b1111u, omgp::MODULE_TYPE_CODES[0], &a, false, false};
    BackplaneScript bp2{0x02, 4, 0b1111u, omgp::MODULE_TYPE_CODES[1], &b, false, false};
    BackplaneScript bp3{0x03, 4, 0b1111u, omgp::MODULE_TYPE_CODES[2], &c, false, false};

    void install(Rig& rig) const {
        rig.install(bp1);
        rig.install(bp2);
        rig.install(bp3);
    }
};

// How many superframes the three-backplane rig is given to converge, and how many further ones
// AS1's "nothing more is discoverable" is observed over. Both are bounds on the TEST, not
// thresholds the engine reads.
// The three-backplane rig is fully discovered by superframe 18 (measured), so 45 is 2.5x that and
// is what every case that just waits for discovery to finish is given. The four cases that assert a
// stalled or unanswering module is RETRIED rather than abandoned keep the longer horizon: what
// they observe is the engine's behaviour over a long stall, which is the point of them.
constexpr uint32_t kConvergeSuperframes = 45;
constexpr uint32_t kStallHorizonSuperframes = 90;
constexpr uint32_t kSettleSuperframes = 20;

// SC-005 replays the cold boot once per cadence, and its finest cadence (1 us) makes 10 engine
// calls per byte time, so it is by far the most expensive case in this file — and the file is the
// mutation oracle, run once per mutant. Measured, not derived: this rig has all twelve modules
// discovered by superframe 18, so 40 compares the whole discovery plus a steady state longer than
// one full enrolment-probe rotation (15 addresses). A bound on the TEST, not a threshold the
// engine reads; the case still REQUIREs twelve discovered at the end of every run.
constexpr uint32_t kGranularitySuperframes = 40;

} // namespace

// --- AS1: a cold rig discovers itself completely -----------------------------------------------

TEST_CASE("AS1: a cold CoreEngine enrols 3 backplanes, assigns 12 node ids and discovers every "
          "one of them [discovery][us1]") {
    const ThreeBackplanes topo;
    Rig rig;
    topo.install(rig);

    rig.run_to_superframe(kConvergeSuperframes, byte_us());

    // Every backplane enrolled — and only the three that are actually there. The other twelve
    // addresses are scripted silent, so a rotation that enrolled an address on nothing but its
    // own optimism would be caught here.
    for (uint8_t addr = omgp::ADDR_backplane_min; addr <= omgp::ADDR_backplane_max; ++addr) {
        // addr is already >= ADDR_backplane_min (0x01) by the loop bound; only the upper edge
        // of the three-backplane rig discriminates (CodeQL: the dropped half was a tautology).
        const bool expected = addr <= 0x03;
        INFO("backplane address " << static_cast<unsigned>(addr));
        REQUIRE(rig.engine().backplane_enrolled(addr) == expected);
    }

    // Exactly twelve ids in use, every one of them inside the R-06 pool, all distinct (a set of
    // ids by construction — each is read once from its own accessor), and every one Discovered.
    const std::vector<uint8_t> ids = rig.in_use_ids();
    REQUIRE(ids.size() == 12u);
    for (uint8_t id : ids) {
        INFO("node id " << static_cast<unsigned>(id));
        REQUIRE(id >= omgp::ADDR_module_min);
        REQUIRE(id <= omgp::ADDR_module_max);
        REQUIRE(rig.engine().discovery_state(id) == DiscoveryState::Discovered);
    }
    REQUIRE(rig.discovered_count() == 12u);

    // Identified exactly once each: twelve IDENTIFY lines carrying twelve distinct node ids,
    // each addressed to the backplane that owns that slot (trunk §5/§8).
    const Transcript& t = rig.transcript();
    REQUIRE(t.count(omgp::OP_IDENTIFY) == 12u);
    std::vector<uint8_t> identified;
    for (const TranscriptEntry& e : t.lines) {
        if (e.opcode != omgp::OP_IDENTIFY)
            continue;
        REQUIRE(e.dst >= omgp::ADDR_backplane_min);
        REQUIRE(e.dst <= 0x03);
        REQUIRE(e.node_id >= omgp::ADDR_module_min);
        for (uint8_t seen : identified) {
            REQUIRE(seen != e.node_id);
        }
        identified.push_back(e.node_id);
    }
    REQUIRE(identified.size() == 12u);
}

TEST_CASE("AS1: the descriptor of every distinct module is read whole — READ_DESC offsets cover "
          "[0, desc_len) contiguously at the R-09 chunk size [discovery][us1]") {
    const ThreeBackplanes topo;
    Rig rig;
    topo.install(rig);
    rig.run_to_superframe(kConvergeSuperframes, byte_us());
    REQUIRE(rig.discovered_count() == 12u);

    // One sweep per BACKPLANE, because each backplane's four modules are like-for-like: the
    // first one's read fills the cache entry and the other three are served from it (spec
    // FR-008). The assertion is against the SCRIPTED blob length, so a truncated or skipped
    // chunk fails rather than merely shortening the transcript.
    struct Expect {
        uint8_t dst;
        size_t len;
    };
    const Expect expected[] = {
        {0x01, topo.a.bytes.size()}, {0x02, topo.b.bytes.size()}, {0x03, topo.c.bytes.size()}};
    size_t total_chunks = 0;
    for (const Expect& ex : expected) {
        INFO("backplane " << static_cast<unsigned>(ex.dst) << ", descriptor " << ex.len
                          << " bytes");
        // Two independent halves make up "covers [0, desc_len) contiguously". (1) COUNT: the
        // transcript carries no offset (T023 fixes its four fields), so the chunk count is
        // compared against the SCRIPTED blob length — reading [0, len) in kDescChunkMax-sized
        // steps takes exactly this many, and two of the three lengths here are exact multiples
        // of that chunk size so the count moves if the chunk size does. (2) CONTENT: the engine
        // only reaches Discovered after l3::descriptor_crc over the REASSEMBLED blob matches the
        // desc_crc the scripted IdentifyResp advertised (core/core_engine.cpp), and AS1's case
        // above asserts all twelve reach it — so a skipped, duplicated or misplaced chunk could
        // not have produced this state. Neither half alone would do: the count would pass on a
        // wrongly-ordered read, and the CRC would pass on any chunking that happened to
        // reassemble correctly.
        const size_t want = (ex.len + kDescChunkMax - 1u) / kDescChunkMax;
        size_t got = 0;
        for (const TranscriptEntry& e : rig.transcript().lines) {
            if (e.opcode == omgp::OP_READ_DESC && e.dst == ex.dst)
                ++got;
        }
        REQUIRE(want > 1u); // the case would not exercise chunking at all otherwise
        REQUIRE(got == want);
        total_chunks += got;
    }
    REQUIRE(rig.transcript().count(omgp::OP_READ_DESC) == total_chunks);
}

TEST_CASE("AS1: past the fixed point nothing further is discoverable — no node id, no state and "
          "no IDENTIFY or READ_DESC is added [discovery][us1]") {
    const ThreeBackplanes topo;
    Rig rig;
    topo.install(rig);
    rig.run_to_superframe(kConvergeSuperframes, byte_us());
    REQUIRE(rig.discovered_count() == 12u);

    const std::vector<uint8_t> ids_before = rig.in_use_ids();
    const size_t identify_before = rig.transcript().count(omgp::OP_IDENTIFY);
    const size_t read_desc_before = rig.transcript().count(omgp::OP_READ_DESC);
    const size_t lines_before = rig.transcript().lines.size();

    rig.run_to_superframe(kConvergeSuperframes + kSettleSuperframes, byte_us());

    REQUIRE(rig.in_use_ids() == ids_before);
    for (uint8_t id : ids_before) {
        REQUIRE(rig.engine().discovery_state(id) == DiscoveryState::Discovered);
    }
    REQUIRE(rig.transcript().count(omgp::OP_IDENTIFY) == identify_before);
    REQUIRE(rig.transcript().count(omgp::OP_READ_DESC) == read_desc_before);
    // The traffic that DID continue is trunk §6's mandatory half and nothing else: status polls
    // and the enrolment probe. Asserted positively so "nothing was added" cannot be satisfied by
    // an engine that simply stopped.
    REQUIRE(rig.transcript().lines.size() > lines_before);
    for (size_t i = lines_before; i < rig.transcript().lines.size(); ++i) {
        const TranscriptEntry& e = rig.transcript().lines[i];
        INFO("transcript line " << i << " opcode " << static_cast<unsigned>(e.opcode));
        REQUIRE((e.opcode == omgp::OP_GET_STATUS || e.opcode == omgp::OP_BP_SLOT_MAP ||
                 e.opcode == omgp::OP_PING));
    }
}

// --- AS2 / SC-001: determinism -----------------------------------------------------------------

TEST_CASE("AS2: two runs of the same script from the same cold start produce identical "
          "transcripts [discovery][us1]") {
    const ThreeBackplanes topo;
    std::string first;
    std::string second;
    {
        Rig rig;
        topo.install(rig);
        rig.run_to_superframe(kConvergeSuperframes, byte_us());
        REQUIRE(rig.discovered_count() == 12u);
        first = rig.transcript().text();
    }
    {
        Rig rig;
        topo.install(rig);
        rig.run_to_superframe(kConvergeSuperframes, byte_us());
        REQUIRE(rig.discovered_count() == 12u);
        second = rig.transcript().text();
    }
    REQUIRE(!first.empty());
    // Whole ordered strings — not multisets, not line counts (SC-001).
    REQUIRE(first == second);
}

// --- SC-005: the same scenario at a different clock granularity ---------------------------------

TEST_CASE("SC-005: the transcript and the end state do not depend on how finely the caller "
          "advances the clock between run_superframe() calls [discovery][us1][clock-granularity]") {
    const ThreeBackplanes topo;
    // Every step divides one byte time, so the WIRE's own event instants are identical across
    // the three runs and only the caller's cadence differs — see this file's header note 2 for
    // why that is the regime this case fixes, and what it therefore does and does not establish.
    const uint64_t steps[] = {byte_us(), byte_us() / 2u, byte_us() / 5u, 1u};
    std::string reference;
    std::vector<uint8_t> reference_ids;
    for (uint64_t step : steps) {
        REQUIRE(step >= 1u);
        REQUIRE(byte_us() % step == 0u);
        Rig rig;
        topo.install(rig);
        rig.run_to_superframe(kGranularitySuperframes, step);
        INFO("clock step " << step << " us");
        REQUIRE(rig.discovered_count() == 12u);
        if (reference.empty()) {
            reference = rig.transcript().text();
            reference_ids = rig.in_use_ids();
            REQUIRE(!reference.empty());
            continue;
        }
        REQUIRE(rig.transcript().text() == reference);
        REQUIRE(rig.in_use_ids() == reference_ids);
    }
}

// --- FR-002: the status polls come first, every superframe -------------------------------------

TEST_CASE("FR-002: in every superframe each enrolled backplane's status poll precedes every "
          "discovery transaction of that superframe [discovery][us1]") {
    const ThreeBackplanes topo;
    Rig rig;
    topo.install(rig);
    rig.run_to_superframe(kConvergeSuperframes, byte_us());
    REQUIRE(rig.discovered_count() == 12u);

    uint32_t current = 0;
    bool discovery_seen = false;
    size_t superframes_with_discovery = 0;
    for (const TranscriptEntry& e : rig.transcript().lines) {
        if (e.superframe != current) {
            current = e.superframe;
            discovery_seen = false;
        }
        const bool is_poll = e.opcode == omgp::OP_GET_STATUS || e.opcode == omgp::OP_BP_SLOT_MAP;
        const bool is_discovery = e.opcode == omgp::OP_IDENTIFY || e.opcode == omgp::OP_READ_DESC;
        if (is_discovery && !discovery_seen) {
            discovery_seen = true;
            ++superframes_with_discovery;
        }
        INFO("superframe " << current << " opcode " << static_cast<unsigned>(e.opcode));
        // A status poll after a discovery transaction in the SAME superframe is the failure
        // this case exists to catch.
        REQUIRE(!(is_poll && discovery_seen));
    }
    // Non-vacuous: the ordering was actually exercised in superframes that had both.
    REQUIRE(superframes_with_discovery > 0u);
}

// --- AS3: a module that never answers IDENTIFY blocks nothing and is retried --------------------

namespace {

// 3 backplanes as before plus a fourth carrying one module; twelve modules in total, of which
// eleven answer. The fourth backplane is the variable: `unanswering` makes it answer
// ERR_UNKNOWN_TARGET for its one slot, for ever (see this file's header note 1).
struct FourBackplanes {
    Descriptor a = make_descriptor(0x2001, 1, 1, omgp::MODULE_TYPE_CODES[0], 2u * kDescChunkMax);
    Descriptor b = make_descriptor(0x2002, 1, 2, omgp::MODULE_TYPE_CODES[1], kDescChunkMax);
    Descriptor c = make_descriptor(0x2003, 2, 1, omgp::MODULE_TYPE_CODES[2], kDescChunkMax + 7);
    Descriptor d = make_descriptor(0x2004, 2, 2, omgp::MODULE_TYPE_CODES[3], kDescChunkMax - 5);

    void install(Rig& rig, bool unanswering) const {
        rig.install(
            BackplaneScript{0x01, 4, 0b1111u, omgp::MODULE_TYPE_CODES[0], &a, false, false});
        rig.install(
            BackplaneScript{0x02, 4, 0b1111u, omgp::MODULE_TYPE_CODES[1], &b, false, false});
        rig.install(BackplaneScript{0x03, 3, 0b111u, omgp::MODULE_TYPE_CODES[2], &c, false, false});
        rig.install(BackplaneScript{0x04, 1, 0b1u, omgp::MODULE_TYPE_CODES[3],
                                    unanswering ? nullptr : &d, unanswering, false});
    }
};

} // namespace

TEST_CASE("AS3: a slot whose module never answers IDENTIFY blocks no other slot or backplane, "
          "and is retried rather than abandoned [discovery][us1]") {
    const FourBackplanes topo;

    // The control: the same rig with every module answering. Its convergence point is the bound
    // the unanswering rig's own eleven must also meet — "the same bounded superframe count".
    uint32_t control_superframe = 0;
    {
        Rig rig;
        topo.install(rig, false);
        rig.run_to_superframe(kStallHorizonSuperframes, byte_us());
        REQUIRE(rig.discovered_count() == 12u);
        control_superframe = rig.transcript().last_superframe();
        REQUIRE(control_superframe > 0u);
    }

    Rig rig;
    topo.install(rig, true);
    rig.run_to_superframe(kStallHorizonSuperframes, byte_us());

    // Twelve slots occupied, twelve ids assigned: the unanswering module is not skipped at
    // assignment time — its slot is occupied and FR-009 gives it an id.
    const std::vector<uint8_t> ids = rig.in_use_ids();
    REQUIRE(ids.size() == 12u);
    // Eleven reach Discovered; the twelfth stays Identifying (data-model.md §2: IDENTIFY in
    // flight or unanswered).
    REQUIRE(rig.discovered_count() == 11u);
    uint8_t stuck = 0;
    for (uint8_t id : ids) {
        if (rig.engine().discovery_state(id) != DiscoveryState::Discovered) {
            REQUIRE(stuck == 0); // exactly one, not "at least one"
            REQUIRE(rig.engine().discovery_state(id) == DiscoveryState::Identifying);
            stuck = id;
        }
    }
    REQUIRE(stuck != 0);
    // It belongs to the fourth backplane, which is the one scripted to answer for it.
    // (Established from the transcript rather than from a new accessor: every IDENTIFY line for
    // this id names the backplane the engine addressed it through.)
    size_t attempts = 0;
    uint32_t first_sf = 0;
    uint32_t last_sf = 0;
    for (const TranscriptEntry& e : rig.transcript().lines) {
        if (e.opcode != omgp::OP_IDENTIFY || e.node_id != stuck)
            continue;
        REQUIRE(e.dst == 0x04);
        if (attempts == 0)
            first_sf = e.superframe;
        last_sf = e.superframe;
        ++attempts;
    }
    // Retried, not abandoned: two or more attempts, at DISTINCT superframes.
    REQUIRE(attempts >= 2u);
    REQUIRE(last_sf > first_sf);
    // And the id is never released while its slot stays occupied (FR-009).
    REQUIRE(rig.engine().node_in_use(stuck));

    // The other eleven were not held up: this rig ran at least as far as the control did, and
    // by that point every one of them is Discovered. A scheduler that let the unanswering
    // module's retries take the demand slot its siblings needed would leave some of them short.
    REQUIRE(rig.transcript().last_superframe() >= control_superframe);
    for (uint8_t id : ids) {
        if (id == stuck)
            continue;
        REQUIRE(rig.engine().discovery_state(id) == DiscoveryState::Discovered);
    }
}

TEST_CASE("AS3: a backplane that goes silent at the trunk level holds up no other backplane's "
          "discovery, and the run still terminates [discovery][us1]") {
    const ThreeBackplanes topo;
    Rig rig;
    topo.install(rig);
    // The third backplane answers nothing at all — a genuine trunk-level timeout on every
    // transaction (trunk §3's T_resp, §7's two retries), not an answer this engine can read.
    rig.install(BackplaneScript{0x03, 0, 0, omgp::MODULE_TYPE_CODES[2], nullptr, false, true});

    rig.run_to_superframe(kConvergeSuperframes, byte_us());

    // The eight modules behind the two live backplanes are fully discovered, and no id was
    // assigned behind the silent one (it never answered a BP_SLOT_MAP, so it reported no slots).
    REQUIRE(rig.in_use_ids().size() == 8u);
    REQUIRE(rig.discovered_count() == 8u);
    REQUIRE(rig.engine().backplane_enrolled(0x01));
    REQUIRE(rig.engine().backplane_enrolled(0x02));
    REQUIRE(!rig.engine().backplane_enrolled(0x03));
}

// --- AS4: a backplane attached after cold boot --------------------------------------------------

TEST_CASE("AS4: a backplane attached after cold boot is enrolled within 15 superframes and its "
          "slots discovered, without re-running the already-enrolled ones [discovery][us1]") {
    const ThreeBackplanes topo;
    Descriptor late = make_descriptor(0x3001, 3, 1, omgp::MODULE_TYPE_CODES[3], 2u * kDescChunkMax);
    Rig rig;
    topo.install(rig);
    rig.run_to_superframe(kConvergeSuperframes, byte_us());
    REQUIRE(rig.discovered_count() == 12u);

    const std::vector<uint8_t> original = rig.in_use_ids();
    const size_t identify_before = rig.transcript().count(omgp::OP_IDENTIFY);
    const size_t read_desc_before = rig.transcript().count(omgp::OP_READ_DESC);
    const uint32_t attached_at = rig.transcript().last_superframe();

    // The fourth backplane becomes answerable. Nothing else about the rig changes.
    rig.install(BackplaneScript{0x04, 2, 0b11u, omgp::MODULE_TYPE_CODES[3], &late, false, false});

    // spec FR-003: the enrolment rotation reaches it within 15 superframes of its becoming
    // answerable — there are 15 backplane addresses and one probe per superframe, so 15 is the
    // whole rotation. (FR-003 bounds ENROLMENT; discovering its slots then follows on the
    // ordinary schedule, which is what AS4 itself says: "when the next enrolment probes reach
    // it ... discovered the same way a cold-boot backplane is".)
    rig.run_to_superframe(attached_at + 15u, byte_us());
    REQUIRE(rig.engine().backplane_enrolled(0x04));

    rig.run_to_superframe(attached_at + 15u + kConvergeSuperframes, byte_us());
    REQUIRE(rig.in_use_ids().size() == 14u);
    REQUIRE(rig.discovered_count() == 14u);

    // The twelve that were already Discovered kept their ids and were not re-identified or
    // re-read: exactly two more IDENTIFYs, and READ_DESC only for the new descriptor.
    for (uint8_t id : original) {
        REQUIRE(rig.engine().node_in_use(id));
        REQUIRE(rig.engine().discovery_state(id) == DiscoveryState::Discovered);
    }
    REQUIRE(rig.transcript().count(omgp::OP_IDENTIFY) == identify_before + 2u);
    const size_t want_chunks = (late.bytes.size() + kDescChunkMax - 1u) / kDescChunkMax;
    REQUIRE(rig.transcript().count(omgp::OP_READ_DESC) == read_desc_before + want_chunks);
    for (size_t i = 0; i < rig.transcript().lines.size(); ++i) {
        const TranscriptEntry& e = rig.transcript().lines[i];
        if (e.superframe <= attached_at)
            continue;
        if (e.opcode != omgp::OP_IDENTIFY && e.opcode != omgp::OP_READ_DESC)
            continue;
        INFO("post-attachment discovery traffic at superframe " << e.superframe);
        REQUIRE(e.dst == 0x04); // never to an already-discovered node's backplane
    }
}

// --- FR-021: presence outcomes are QUEUED by the scheduler, never delivered by it ---------------

TEST_CASE("FR-021: NodeDiscovered and NodeRemoved are observed only through drain_callbacks() "
          "[discovery][us1]") {
    Descriptor desc = make_descriptor(0x4001, 1, 1, omgp::MODULE_TYPE_CODES[0], kDescChunkMax + 3u);
    const BackplaneScript bp{0x01, 2, 0b11u, omgp::MODULE_TYPE_CODES[0], &desc, false, false};
    Rig rig;
    rig.install(bp);
    rig.run_to_superframe(kConvergeSuperframes, byte_us());
    REQUIRE(rig.discovered_count() == 2u);

    // However many superframes have run, the application has not been called once.
    REQUIRE(rig.recorder().calls == 0u);
    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().calls >= 2u);
    REQUIRE(rig.recorder().count(LifecycleKind::NodeDiscovered) == 2u);
    REQUIRE(rig.recorder().count(LifecycleKind::NodeRemoved) == 0u);

    // One slot empties. The `changed` bit is set for it too, but the reconciliation is against
    // absolute `occupied` (ruled 2026-09-22, docs/OPEN-QUESTIONS.md) — this case asserts the
    // OUTCOME, which both readings agree on.
    const unsigned calls_before = rig.recorder().calls;
    rig.reoccupy(bp, 0b01u, 0b10u);
    rig.run_to_superframe(rig.transcript().last_superframe() + kSettleSuperframes, byte_us());

    REQUIRE(rig.recorder().calls == calls_before); // still nothing delivered by the scheduler
    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeRemoved) == 1u);
    REQUIRE(rig.in_use_ids().size() == 1u);
    // FR-009: the surviving slot's id is untouched by its neighbour's removal.
    REQUIRE(rig.engine().node_in_use(omgp::ADDR_module_min));
    REQUIRE(rig.engine().discovery_state(omgp::ADDR_module_min) == DiscoveryState::Discovered);
}

// --- CLAUDE.md rule 7: a hostile or malformed slot map is survived, not followed ----------------

namespace {

// A BP_SLOT_MAP response built by hand rather than by the codec, so the engine's own bound can
// be exercised independently of l3::decode_bp_slot_map_resp's (which refuses an over-cap
// slot_count outright, l3/l3_payload.cpp — so the wire path cannot deliver one and this is
// reachable only by calling reconcile_slot_map() directly, as contracts/core-cpp.md allows).
omgp::l3::BpSlotMapResp raw_slot_map(uint8_t slot_count, const uint8_t* occupied,
                                     const uint8_t* changed, uint8_t bitmap_len) {
    omgp::l3::BpSlotMapResp r{};
    r.slot_count = slot_count;
    r.occupied.data = occupied;
    r.occupied.len = bitmap_len;
    r.changed.data = changed;
    r.changed.len = bitmap_len;
    return r;
}

} // namespace

TEST_CASE("rule 7: a slot map over LIMIT_bp_slot_map_max_slots, or with bits set past its own "
          "slot_count, leaves the engine running and every other slot unaffected "
          "[discovery][us1][robustness]") {
    Descriptor desc = make_descriptor(0x5001, 1, 1, omgp::MODULE_TYPE_CODES[0], kDescChunkMax + 3u);
    const BackplaneScript bp{0x01, 2, 0b11u, omgp::MODULE_TYPE_CODES[0], &desc, false, false};
    Rig rig;
    rig.install(bp);
    rig.run_to_superframe(kConvergeSuperframes, byte_us());
    REQUIRE(rig.discovered_count() == 2u);
    const std::vector<uint8_t> before = rig.in_use_ids();

    SECTION("a slot_count above the wire cap is clamped, not followed") {
        // Every bitmap byte set, but a bitmap far shorter than the claimed slot_count implies:
        // an engine that trusted slot_count would read past `occupied` and write past
        // node_id_by_slot. Under the native ASan/UBSan build either is a finding, not a pass.
        uint8_t bits[4];
        for (uint8_t& b : bits)
            b = 0xFF;
        const omgp::l3::BpSlotMapResp hostile = raw_slot_map(0xFF, bits, bits, sizeof bits);
        rig.engine().reconcile_slot_map(0x02, hostile, rig.now_us());
        // Address 0x02 is not a backplane this rig has, but it is a legal backplane address, so
        // the response is reconciled: the first 32 slots the bitmaps DO cover get ids, and the
        // 223 slots they do not are read as unoccupied rather than overrun.
        REQUIRE(rig.in_use_ids().size() == before.size() + 32u);
    }

    SECTION("bits set at or past slot_count are ignored") {
        uint8_t bits[1];
        bits[0] = 0xFF; // slots 0..7 claimed occupied, but slot_count says there are only 3
        const omgp::l3::BpSlotMapResp padded = raw_slot_map(3, bits, bits, sizeof bits);
        rig.engine().reconcile_slot_map(0x03, padded, rig.now_us());
        REQUIRE(rig.in_use_ids().size() == before.size() + 3u);
    }

    SECTION("an address that is not a backplane names no record at all") {
        uint8_t bits[1];
        bits[0] = 0xFF;
        const omgp::l3::BpSlotMapResp anywhere = raw_slot_map(4, bits, bits, sizeof bits);
        rig.engine().reconcile_slot_map(omgp::ADDR_host, anywhere, rig.now_us());
        rig.engine().reconcile_slot_map(omgp::ADDR_module_min, anywhere, rig.now_us());
        rig.engine().reconcile_slot_map(0xFF, anywhere, rig.now_us());
        REQUIRE(rig.in_use_ids() == before);
    }

    // Whatever the section did, the engine is still running: the original two nodes keep their
    // ids and their state, and further superframes still poll and probe.
    for (uint8_t id : before) {
        REQUIRE(rig.engine().node_in_use(id));
        REQUIRE(rig.engine().discovery_state(id) == DiscoveryState::Discovered);
    }
    const size_t lines = rig.transcript().lines.size();
    rig.run_to_superframe(rig.transcript().last_superframe() + 3u, byte_us());
    REQUIRE(rig.transcript().lines.size() > lines);
}

TEST_CASE("a descriptor read whose reader's slot empties mid-read is taken over by another node "
          "waiting on the same descriptor, not stranded [discovery][us1][robustness]") {
    // Six chunks, so the read spans several superframes and the slot can be emptied while it is
    // genuinely in progress rather than in a race with its completion.
    Descriptor desc = make_descriptor(0x6001, 1, 1, omgp::MODULE_TYPE_CODES[0], 6u * kDescChunkMax);
    const BackplaneScript bp{0x01, 2, 0b11u, omgp::MODULE_TYPE_CODES[0], &desc, false, false};
    Rig rig;
    rig.install(bp);

    // Run only as far as the first READ_DESC: the lower of the two ids is reading, and the
    // other has not been identified yet (descriptor chunks outrank the IDENTIFY scan).
    bool reading = false;
    for (uint64_t i = 0; i < 400000u && !reading; ++i) {
        rig.step(byte_us());
        reading = rig.transcript().count(omgp::OP_READ_DESC) > 0u &&
                  rig.transcript().count(omgp::OP_READ_DESC) < 6u;
    }
    REQUIRE(reading);
    REQUIRE(rig.engine().node_in_use(omgp::ADDR_module_min));
    REQUIRE(rig.engine().discovery_state(omgp::ADDR_module_min) ==
            DiscoveryState::ReadingDescriptor);

    // Slot 0 empties under the reader (spec FR-010).
    rig.reoccupy(bp, 0b10u, 0b01u);
    rig.run_to_superframe(rig.transcript().last_superframe() + kConvergeSuperframes, byte_us());

    // The surviving slot's module is fully discovered: the descriptor it was waiting on was
    // taken over and finished rather than left half-read for ever.
    REQUIRE(rig.in_use_ids().size() == 1u);
    const uint8_t survivor = rig.in_use_ids().front();
    REQUIRE(rig.engine().discovery_state(survivor) == DiscoveryState::Discovered);
    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeRemoved) == 1u);
}

TEST_CASE("R-06: the node-id pool is never wrapped onto a live id — exhaustion is reported for "
          "the slot instead [discovery][us1][robustness]") {
    Rig rig;
    // Four backplanes' worth of 32-slot maps is 128 occupied slots against a pool of
    // ADDR_module_max - ADDR_module_min + 1 = 112 ids, so the last sixteen find none.
    uint8_t bits[4];
    for (uint8_t& b : bits)
        b = 0xFF;
    const omgp::l3::BpSlotMapResp full = raw_slot_map(32, bits, bits, sizeof bits);
    for (uint8_t addr = 0x01; addr <= 0x04; ++addr) {
        rig.engine().reconcile_slot_map(addr, full, 0);
    }
    const size_t pool = static_cast<size_t>(omgp::ADDR_module_max) -
                        static_cast<size_t>(omgp::ADDR_module_min) + 1u;
    REQUIRE(rig.in_use_ids().size() == pool);
    for (uint8_t id : rig.in_use_ids()) {
        REQUIRE(id >= omgp::ADDR_module_min);
        REQUIRE(id <= omgp::ADDR_module_max);
    }
    REQUIRE(rig.recorder().calls == 0u);
    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeIdPoolExhausted) == 4u * 32u - pool);
}

// --- AS3, the case the highest-id rig cannot show: the unanswering module holds the LOWEST id ---

TEST_CASE("AS3: an unanswering module at the LOWEST node id blocks no other slot either — the "
          "IDENTIFY scan does not starve on it [discovery][us1]") {
    // AS3's own rig above puts the unanswering module on the LAST backplane, so it holds the
    // highest node id and the scan reaches every other node before it. That arrangement cannot
    // observe the failure mode this case is for: a node stays in Identifying until it answers
    // with an identity, so a scan that always restarted at ADDR_module_min would hand every
    // demand slot of every superframe to the lowest such id and never identify anyone else.
    // Backplane 0x01 is reconciled first, so its one unanswering slot takes ADDR_module_min and
    // the four honest modules on 0x02 take the ids above it.
    Descriptor honest =
        make_descriptor(0x8001, 1, 1, omgp::MODULE_TYPE_CODES[0], 2u * kDescChunkMax);
    Rig rig;
    rig.install(BackplaneScript{0x01, 1, 0b1u, omgp::MODULE_TYPE_CODES[3], nullptr, true, false});
    rig.install(
        BackplaneScript{0x02, 4, 0b1111u, omgp::MODULE_TYPE_CODES[0], &honest, false, false});
    rig.run_to_superframe(kStallHorizonSuperframes, byte_us());

    REQUIRE(rig.in_use_ids().size() == 5u);
    REQUIRE(rig.engine().discovery_state(omgp::ADDR_module_min) == DiscoveryState::Identifying);
    // The four behind it are fully discovered, and each was reported once.
    REQUIRE(rig.discovered_count() == 4u);
    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeDiscovered) == 4u);
    // The unanswering one is still retried rather than abandoned (AS3's other half).
    const size_t identifies = rig.transcript().count(omgp::OP_IDENTIFY);
    rig.run_to_superframe(rig.transcript().last_superframe() + kSettleSuperframes, byte_us());
    REQUIRE(rig.transcript().count(omgp::OP_IDENTIFY) > identifies);
    REQUIRE(rig.discovered_count() == 4u); // and nothing regresses while it is retried
}

// --- R-06: a STANDING id shortage is reported once per transition, not once per sweep ----------

TEST_CASE("R-06: a standing node-id shortage is reported once per slot, and the presence events "
          "behind it are still delivered [discovery][us1][robustness]") {
    // The condition R-06's exhaustion outcome reports is a STANDING one: an over-full rig
    // re-reports the same occupancy on every BP_SLOT_MAP, for as long as it stays over-full. The
    // notice must therefore be edge-triggered — data-model.md §8a's pending ring holds
    // LIMIT_max_nodes items and is drop-NEWEST, so one notice per unassignable slot per sweep
    // would fill it with repeats of one unchanged condition and then refuse the NodeRemoved /
    // NodeDiscovered behind them (spec FR-017's presence reporting would stop for good).
    //
    // Two backplanes at the protocol's own slot cap, reconciled directly (the wire path cannot
    // carry a 232-slot map — see raw_slot_map's note): 464 occupied slots, a 112-id pool.
    Rig rig;
    uint8_t all_occupied[(omgp::LIMIT_bp_slot_map_max_slots + 7) / 8];
    for (uint8_t& b : all_occupied)
        b = 0xFF;
    const omgp::l3::BpSlotMapResp full =
        raw_slot_map(static_cast<uint8_t>(omgp::LIMIT_bp_slot_map_max_slots), all_occupied,
                     all_occupied, static_cast<uint8_t>(sizeof all_occupied));
    const size_t pool = static_cast<size_t>(omgp::ADDR_module_max) -
                        static_cast<size_t>(omgp::ADDR_module_min) + 1u;
    const size_t unassignable = omgp::LIMIT_bp_slot_map_max_slots - pool;

    // Sweep 1: the pool is spent on the first `pool` slots; every slot after that transitions
    // into the shortage, and each is reported exactly once.
    rig.engine().reconcile_slot_map(0x01, full, 0);
    rig.engine().drain_callbacks();
    REQUIRE(rig.in_use_ids().size() == pool);
    REQUIRE(rig.recorder().count(LifecycleKind::NodeIdPoolExhausted) == unassignable);
    REQUIRE(rig.engine().dropped_deliveries() == 0u);

    // Sweeps 2 and 3: the SAME occupancy, nothing changed. Not one further notice — and with the
    // ring no longer re-flooded, nothing is dropped.
    for (uint64_t now = 1; now <= 2; ++now) {
        rig.engine().reconcile_slot_map(0x01, full, now);
    }
    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeIdPoolExhausted) == unassignable);
    REQUIRE(rig.engine().dropped_deliveries() == 0u);

    // A second over-full backplane: 232 more slots, all unassignable, each its own first
    // transition. 232 is MORE than §8a's ring holds (LIMIT_max_nodes == 128), so this one sweep
    // cannot deliver them all — the drop-newest ring refuses the tail of them.
    //
    // What must NOT happen then: a slot whose notice the ring REFUSED counting as reported. The
    // dedupe bit records "the application has been told", so it may only be set when the enqueue
    // actually succeeded; set unconditionally, the refused slots are silent for ever, even while
    // the shortage stands and the application drains every superframe. Edge-triggering dedupes
    // what was DELIVERED, never what was dropped.
    rig.engine().reconcile_slot_map(0x02, full, 3);
    rig.engine().drain_callbacks();
    const size_t reported_after_second = rig.recorder().count(LifecycleKind::NodeIdPoolExhausted);
    REQUIRE(reported_after_second > unassignable);
    REQUIRE(reported_after_second < unassignable + omgp::LIMIT_bp_slot_map_max_slots);
    REQUIRE(rig.engine().dropped_deliveries() > 0u); // the ring turned the rest away

    // Repeat sweeps of the SAME map, each followed by a drain, re-offer exactly the refused
    // slots — so an application that keeps draining learns about every one of the 232 — and then
    // go quiet: once every slot has actually been reported, no later sweep of the unchanged map
    // adds anything. Both halves matter; the second is the one that keeps a standing shortage
    // from re-flooding the ring sweep after sweep.
    for (uint64_t now = 4; now <= 8; ++now) {
        rig.engine().reconcile_slot_map(0x02, full, now);
        rig.engine().drain_callbacks();
    }
    const size_t all_reported = rig.recorder().count(LifecycleKind::NodeIdPoolExhausted);
    REQUIRE(all_reported == unassignable + omgp::LIMIT_bp_slot_map_max_slots);
    rig.engine().reconcile_slot_map(0x02, full, 9);
    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeIdPoolExhausted) == all_reported);

    // The presence half still works on an over-full rig: slot 0 of 0x01 empties, and its
    // NodeRemoved is delivered rather than lost behind a re-flood of exhaustion notices.
    const uint32_t dropped_before = rig.engine().dropped_deliveries();
    uint8_t minus_slot0[sizeof all_occupied];
    for (size_t i = 0; i < sizeof minus_slot0; ++i)
        minus_slot0[i] = 0xFF;
    minus_slot0[0] = 0xFE; // slot 0 leaves; every other slot unchanged
    const omgp::l3::BpSlotMapResp gone =
        raw_slot_map(static_cast<uint8_t>(omgp::LIMIT_bp_slot_map_max_slots), minus_slot0,
                     minus_slot0, static_cast<uint8_t>(sizeof minus_slot0));
    rig.engine().reconcile_slot_map(0x01, gone, 10);
    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeRemoved) == 1u);
    REQUIRE(rig.engine().dropped_deliveries() == dropped_before);
    // The freed id went to the lowest still-unassigned occupied slot (R-06 first fit), so the
    // pool stays fully spent and no id was wrapped onto a live one.
    REQUIRE(rig.in_use_ids().size() == pool);

    // Edge-triggered, not one-shot: slot 0 is re-occupied while the rig is still over-full, which
    // is a NEW transition into the shortage and is reported again.
    const size_t before_reoccupy = rig.recorder().count(LifecycleKind::NodeIdPoolExhausted);
    rig.engine().reconcile_slot_map(0x01, full, 11);
    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeIdPoolExhausted) == before_reoccupy + 1u);
}

// --- R-09 / rule 7: the two bounds on a hostile READ_DESC answer -------------------------------

namespace {

// A backplane whose IDENTIFY advertises `advertised_len` bytes but whose READ_DESC serves from a
// LONGER blob: the engine asks for kDescChunkMax every time (begin_desc_chunk), so the final
// chunk of a descriptor whose length is not a multiple of it comes back carrying more bytes than
// the advertised length has room for. MockL3Node answers min(max_len, blob_len - offset)
// (contracts/mock-l3-node.md), which is exactly how a node over-serving its own advertised
// desc_len behaves on the wire — no change to the double is needed to express it.
struct OverServingBackplane {
    Descriptor blob;
    omgp::l3::IdentifyResp identify{};
    std::vector<L3Step> steps;

    OverServingBackplane(uint16_t advertised_len, uint8_t module_type)
        : blob(make_descriptor(0x7001, 1, 1, module_type,
                               static_cast<size_t>(advertised_len) + kDescChunkMax)) {
        identify.major = 1;
        identify.minor = 0;
        identify.module_type = module_type;
        identify.desc_len = advertised_len;
        // The CRC of exactly the bytes the advertised length covers, so the ONLY thing wrong with
        // this node is the over-long final chunk: a reader that silently truncated it would
        // compute a matching CRC over those first advertised_len bytes and credit the node with a
        // descriptor it never promised, which is what makes the bound observable at all.
        identify.desc_crc = omgp::l3::descriptor_crc(blob.bytes.data(), advertised_len);
    }

    void install(Rig& rig, uint8_t addr, uint8_t slot_count, uint32_t occupied) {
        steps.clear();
        steps.push_back(L3Step::of(SlotMapStep{slot_count, occupied, occupied}));
        steps.push_back(L3Step::of(StatusStep{ready_status()}));
        steps.push_back(L3Step::of(IdentifyStep{identify}));
        const uint16_t served_len = static_cast<uint16_t>(blob.bytes.size());
        steps.push_back(L3Step::of(DescChunkStep{blob.bytes.data(), served_len}));
        rig.node().set_script(addr, steps.data(), steps.size());
    }
};

} // namespace

TEST_CASE("rule 7: a READ_DESC chunk carrying more bytes than the advertised desc_len has room "
          "for is refused, and the node retried from IDENTIFY [discovery][us1][robustness]") {
    // An advertised length that is NOT a multiple of the chunk size: two chunks of kDescChunkMax
    // are asked for, the second has room for only a fraction of one, and the double serves a full
    // one from its longer blob. At desc_len == LIMIT_max_descriptor_bytes the same surplus would
    // land past DescriptorCacheEntry::blob entirely; the length here is small only so the case
    // converges quickly — the code path is the same one.
    const uint16_t advertised = static_cast<uint16_t>(kDescChunkMax + kDescChunkMax / 2u);
    REQUIRE(advertised % kDescChunkMax != 0u);
    OverServingBackplane liar(advertised, omgp::MODULE_TYPE_CODES[0]);
    // An honest backplane alongside it: a hostile answer must not cost anyone else discovery.
    // It is backplane 0x01 and the liar 0x02, so the honest modules hold the LOWER node ids.
    // The OPPOSITE arrangement — the liar at the lowest id — is the starvation case, and it is
    // covered on its own: by issue_demand()'s rotating identify_cursor_ for the IDENTIFY scan and
    // its per-node desc_stalled_superframe bound for the chunk ring, asserted by the ERR_BUSY
    // case at the end of this file. This case fixes the ids the other way round so that what it
    // measures is the refusal itself and not the scheduler's fairness.
    Descriptor honest =
        make_descriptor(0x7002, 1, 1, omgp::MODULE_TYPE_CODES[1], 2u * kDescChunkMax);
    const BackplaneScript bp1{0x01, 2, 0b11u, omgp::MODULE_TYPE_CODES[1], &honest, false, false};

    Rig rig;
    rig.install(bp1);
    liar.install(rig, 0x02, 1, 0b1u);
    rig.run_to_superframe(kConvergeSuperframes, byte_us());

    // The liar's one module is never credited with a descriptor: it is read, refused, and retried
    // from IDENTIFY for as long as it keeps over-serving.
    const std::vector<uint8_t> ids = rig.in_use_ids();
    REQUIRE(ids.size() == 3u); // two slots on 0x01, one on 0x02
    const uint8_t liar_id = ids.back();
    REQUIRE(rig.engine().discovery_state(liar_id) != DiscoveryState::Discovered);
    REQUIRE(rig.transcript().count(omgp::OP_IDENTIFY) > 3u); // retried, not abandoned
    REQUIRE(rig.transcript().count(omgp::OP_READ_DESC) > 2u);

    // The honest backplane's two modules are unaffected, and the rig keeps running.
    REQUIRE(rig.discovered_count() == 2u);
    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeDiscovered) == 2u);
    REQUIRE(rig.engine().dropped_deliveries() == 0u);
    const size_t lines = rig.transcript().lines.size();
    rig.run_to_superframe(rig.transcript().last_superframe() + kSettleSuperframes, byte_us());
    REQUIRE(rig.transcript().lines.size() > lines);
}

TEST_CASE("protocol-l3 §3.1: a READ_DESC chunk whose offset does not continue the reassembly is "
          "dropped, not written at the cursor [discovery][us1][robustness]") {
    // Three chunks, so a forged answer can arrive with a real read genuinely in progress.
    Descriptor desc = make_descriptor(0x7003, 1, 1, omgp::MODULE_TYPE_CODES[0], 3u * kDescChunkMax);
    const BackplaneScript bp{0x01, 1, 0b1u, omgp::MODULE_TYPE_CODES[0], &desc, false, false};
    Rig rig;
    rig.install(bp);

    // Run until a READ_DESC is outstanding: that is the state in which a lying answer could
    // arrive, and it fixes which node on_desc_chunk() attributes the forged chunk to.
    bool outstanding = false;
    for (uint64_t i = 0; i < 400000u && !outstanding; ++i) {
        rig.step(byte_us());
        outstanding = omgp::core::CoreEngineTestSeam::read_desc_outstanding(rig.engine());
    }
    REQUIRE(outstanding);
    REQUIRE(rig.engine().discovery_state(omgp::ADDR_module_min) ==
            DiscoveryState::ReadingDescriptor);

    // A well-formed READ_DESC response for an offset the engine is NOT at, carrying a full chunk
    // of bytes that belong nowhere in this descriptor. Encoded by the real l3/ codecs.
    uint8_t forged_bytes[kDescChunkMax];
    for (uint8_t& b : forged_bytes)
        b = 0xAA;
    omgp::l3::ReadDescResp lying{};
    lying.offset = static_cast<uint16_t>(2u * kDescChunkMax + 1u); // never a cursor value here
    lying.bytes.data = forged_bytes;
    lying.bytes.len = static_cast<uint8_t>(sizeof forged_bytes);
    uint8_t payload[omgp::LIMIT_max_l3_payload];
    size_t payload_len = 0;
    REQUIRE(omgp::l3::encode_read_desc_resp(lying, payload, sizeof payload, payload_len) ==
            omgp::l3::Status::Ok);
    // protocol-l3 §3's five-byte header, as a RESPONSE (FLAG_response set), then the payload —
    // exactly the layout link::Master would have handed up.
    omgp::l3::Header hdr{};
    hdr.opcode = omgp::OP_READ_DESC;
    hdr.node_id = omgp::ADDR_module_min;
    hdr.seq = 0;
    hdr.flags = omgp::FLAG_response;
    hdr.payload_len = static_cast<uint8_t>(payload_len);
    uint8_t message[omgp::LIMIT_max_l3_message];
    size_t message_len = 0;
    REQUIRE(omgp::l3::encode_header(hdr, message, sizeof message, message_len) ==
            omgp::l3::Status::Ok);
    for (size_t i = 0; i < payload_len; ++i) {
        message[message_len + i] = payload[i];
    }
    message_len += payload_len;
    omgp::link::MasterEvent forged{};
    forged.kind = omgp::link::MasterEvent::Answered;
    forged.response.payload = message;
    forged.response.len = static_cast<uint8_t>(message_len);
    omgp::core::CoreEngineTestSeam::deliver_desc_chunk(rig.engine(), forged, rig.now_us());

    // The forged bytes went nowhere: the read continues from where it was, completes, and the
    // reassembled blob still matches the desc_crc IDENTIFY advertised — which is the engine's own
    // oracle for "these are the right bytes in the right order". A chunk written at the cursor
    // regardless of its own offset would fail that CRC, discard the entry and send this node back
    // to IDENTIFY, so a SECOND IDENTIFY in the transcript is exactly what this case rules out.
    rig.run_to_superframe(rig.transcript().last_superframe() + kConvergeSuperframes, byte_us());
    REQUIRE(rig.engine().discovery_state(omgp::ADDR_module_min) == DiscoveryState::Discovered);
    REQUIRE(rig.transcript().count(omgp::OP_IDENTIFY) == 1u);
    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeDiscovered) == 1u);
}

// --- trunk §8 / AS3: a REFUSED READ_DESC must not starve the rest of the rig --------------------

namespace {

// A backplane that identifies its one module honestly and then answers ERR_BUSY to every
// READ_DESC, for ever. This is not a hostile shape: trunk §8 MANDATES that answer ("MUST begin
// its response within T_turn ... with the module's reply if it is ready, otherwise ERR_BUSY",
// "MUST NOT stall the trunk") and it is CLAUDE.md rule 6's own prescription for a bridge whose
// module bus is not ready, while trunk §10.5 records that a persistent busy is unbounded at L2.
// With no DescChunkStep of its own in the script, the ErrorStep is the wildcard every READ_DESC
// falls back to once the enrolment PING has consumed it (contracts/mock-l3-node.md).
struct BusyDescBackplane {
    Descriptor blob;
    omgp::l3::IdentifyResp identify{};
    std::vector<L3Step> steps;

    explicit BusyDescBackplane(uint8_t module_type)
        : blob(make_descriptor(0x9001, 1, 1, module_type, 2u * kDescChunkMax)) {
        identify = identify_for(blob, module_type);
    }

    void install(Rig& rig, uint8_t addr, uint8_t slot_count, uint32_t occupied) {
        steps.clear();
        steps.push_back(L3Step::of(SlotMapStep{slot_count, occupied, occupied}));
        steps.push_back(L3Step::of(StatusStep{ready_status()}));
        steps.push_back(L3Step::of(IdentifyStep{identify}));
        steps.push_back(L3Step::of(ErrorStep{omgp::ERR_BUSY}));
        rig.node().set_script(addr, steps.data(), steps.size());
    }
};

} // namespace

TEST_CASE("trunk §8: a backplane answering ERR_BUSY to every READ_DESC blocks no other slot's "
          "discovery [discovery][us1][robustness]") {
    // User Story 1 AS3: a slot whose module does not answer "does not block the discovery of any
    // other slot or backplane". An ERROR answer to a descriptor chunk is RE-QUEUED for the same
    // offset (READ_DESC is idempotent, CLAUDE.md rule 2) — so without a per-superframe bound on
    // that retry, one permanently-busy node keeps issue_demand()'s chunk ring non-empty for ever;
    // the ring is drained BEFORE the IDENTIFY scan, so the scan's own rotation cursor is never
    // reached at all. Measured before the bound existed: 893 READ_DESC to the busy backplane in
    // 301 superframes, 0 IDENTIFY to the honest one, 0 of 4 honest modules discovered.
    //
    // The busy backplane holds the LOWER trunk address and so the LOWER node id, deliberately:
    // that is the arrangement in which a head-of-queue monopoly is observable at all.
    BusyDescBackplane busy(omgp::MODULE_TYPE_CODES[0]);
    Descriptor honest =
        make_descriptor(0x9002, 1, 1, omgp::MODULE_TYPE_CODES[1], 2u * kDescChunkMax);
    const BackplaneScript bp2{0x02, 4, 0b1111u, omgp::MODULE_TYPE_CODES[1], &honest, false, false};

    Rig rig;
    busy.install(rig, 0x01, 1, 0b1u);
    rig.install(bp2);
    rig.run_to_superframe(kConvergeSuperframes, byte_us());

    const std::vector<uint8_t> ids = rig.in_use_ids();
    REQUIRE(ids.size() == 5u); // one slot on 0x01, four on 0x02
    // The four honest modules are discovered, and their lifecycle events are delivered.
    REQUIRE(rig.discovered_count() == 4u);
    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeDiscovered) == 4u);
    REQUIRE(rig.engine().dropped_deliveries() == 0u);

    // The busy node itself is neither discovered nor abandoned: it keeps being asked, which is
    // AS3's "retried, not abandoned", and the honest backplane's own traffic happened alongside
    // that retrying rather than behind it.
    const uint8_t busy_id = ids.front();
    REQUIRE(rig.engine().discovery_state(busy_id) != DiscoveryState::Discovered);
    REQUIRE(rig.transcript().count(omgp::OP_READ_DESC) > 2u);

    // And it stays that way: the busy node does not take the whole demand budget of every later
    // superframe either, so the rig keeps running and nothing already discovered regresses.
    const size_t lines = rig.transcript().lines.size();
    rig.run_to_superframe(rig.transcript().last_superframe() + kSettleSuperframes, byte_us());
    REQUIRE(rig.transcript().lines.size() > lines);
    REQUIRE(rig.discovered_count() == 4u);
}

// --- the same two starvations TOGETHER: a stalled read AND a slot that never answers IDENTIFY ---

namespace {

// Identifies its one module honestly, answers ERR_BUSY to the FIRST READ_DESC only (trunk §8's
// mandated answer for a bridge whose module bus is not ready), then serves the descriptor
// honestly for ever. Two ErrorSteps because the enrolment PING — OpClass::Other, with no step of
// its own — consumes the first wildcard it reaches (contracts/mock-l3-node.md); the second is
// the one refused chunk, and the DescChunkStep behind it is the Desc class's steady state from
// then on. A single transient ERR_BUSY is the cheapest possible fault, so a module lost to one
// is a module lost to nothing at all.
struct BrieflyBusyBackplane {
    Descriptor blob;
    omgp::l3::IdentifyResp identify{};
    std::vector<L3Step> steps;

    explicit BrieflyBusyBackplane(uint8_t module_type)
        : blob(make_descriptor(0x9003, 1, 1, module_type, 2u * kDescChunkMax)) {
        identify = identify_for(blob, module_type);
    }

    void install(Rig& rig, uint8_t addr, uint8_t slot_count, uint32_t occupied) {
        steps.clear();
        steps.push_back(L3Step::of(SlotMapStep{slot_count, occupied, occupied}));
        steps.push_back(L3Step::of(StatusStep{ready_status()}));
        steps.push_back(L3Step::of(IdentifyStep{identify}));
        steps.push_back(L3Step::of(ErrorStep{omgp::ERR_BUSY}));
        steps.push_back(L3Step::of(ErrorStep{omgp::ERR_BUSY}));
        steps.push_back(desc_step(blob));
        rig.node().set_script(addr, steps.data(), steps.size());
    }
};

} // namespace

TEST_CASE("AS3: a stalled READ_DESC is still retried while another slot never answers IDENTIFY "
          "[discovery][us1][robustness]") {
    // The two deprioritisations of this file have to hold in BOTH directions at once, and each
    // of the two cases above exercises only one of them: the ERR_BUSY case has no node that stays
    // Identifying, and the AS3 case has no stalled descriptor read. Put both faults in one rig and
    // the question becomes whether a pass that runs AFTER the IDENTIFY scan runs at all — the scan
    // never runs out of candidates, because a module that does not answer is RETRIED and stays in
    // Identifying for ever (User Story 1 AS3), so "after the scan" would mean "never".
    BusyDescBackplane busy(omgp::MODULE_TYPE_CODES[0]);
    Descriptor honest =
        make_descriptor(0x9004, 1, 1, omgp::MODULE_TYPE_CODES[1], 2u * kDescChunkMax);

    Rig rig;
    busy.install(rig, 0x01, 1, 0b1u);
    rig.install(
        BackplaneScript{0x02, 4, 0b1111u, omgp::MODULE_TYPE_CODES[1], &honest, false, false});
    // trunk §8's ERR_UNKNOWN_TARGET slot: occupied, given an id (FR-009), never identified.
    rig.install(BackplaneScript{0x03, 1, 0b1u, omgp::MODULE_TYPE_CODES[2], nullptr, true, false});
    rig.run_to_superframe(kStallHorizonSuperframes, byte_us());

    REQUIRE(rig.in_use_ids().size() == 6u); // 1 busy + 4 honest + 1 unanswering
    REQUIRE(rig.discovered_count() == 4u);

    const size_t reads_before = rig.transcript().count(omgp::OP_READ_DESC);
    const size_t identifies_before = rig.transcript().count(omgp::OP_IDENTIFY);
    rig.run_to_superframe(rig.transcript().last_superframe() + kSettleSuperframes, byte_us());

    // AS3 in both directions, over the SAME stretch of superframes: the refused read keeps being
    // attempted, and the unidentified slot keeps being asked. Neither fault silences the other.
    REQUIRE(rig.transcript().count(omgp::OP_READ_DESC) > reads_before);
    REQUIRE(rig.transcript().count(omgp::OP_IDENTIFY) > identifies_before);
    REQUIRE(rig.discovered_count() == 4u);
}

namespace {

// Serves the FIRST chunk of its descriptor honestly and then goes busy for a stretch — ERR_BUSY to
// whatever it is asked next, which trunk §8 allows a bridge for any request it cannot serve yet —
// before answering honestly again. Partial progress before the refusals is the whole point: it is
// the state in which this node's cache entry holds bytes that a reclaim would DESTROY, and a
// stretch of refusals rather than one keeps the node stalled — its ring item dropped by
// issue_demand() — across enough superframes that the parity alternation is certain to give
// scan_identify() a turn while it is in that state.
// ErrorStep is a WILDCARD (contracts/mock-l3-node.md): the steps below are consumed in script
// order by whichever class asks next, so the enrolment PING takes the first and this backplane's
// own status and slot-map polls take some of the rest. Only two of them reach the descriptor read,
// which is why the stretch is six and not two.
struct StallsMidReadBackplane {
    Descriptor blob;
    omgp::l3::IdentifyResp identify{};
    std::vector<L3Step> steps;

    explicit StallsMidReadBackplane(uint8_t module_type)
        : blob(make_descriptor(0x9005, 1, 1, module_type, 3u * kDescChunkMax)) {
        identify = identify_for(blob, module_type);
    }

    void install(Rig& rig, uint8_t addr, uint8_t slot_count, uint32_t occupied) {
        steps.clear();
        steps.push_back(L3Step::of(SlotMapStep{slot_count, occupied, occupied}));
        steps.push_back(L3Step::of(StatusStep{ready_status()}));
        steps.push_back(L3Step::of(IdentifyStep{identify}));
        steps.push_back(L3Step::of(ErrorStep{omgp::ERR_BUSY})); // the enrolment PING's
        steps.push_back(desc_step(blob));                       // chunk 0, honestly
        for (int i = 0; i < 6; ++i) {
            steps.push_back(L3Step::of(ErrorStep{omgp::ERR_BUSY})); // stalled, and stays stalled
        }
        steps.push_back(desc_step(blob)); // then honest for ever
        rig.node().set_script(addr, steps.data(), steps.size());
    }
};

} // namespace

TEST_CASE("data-model.md §5: a module identifying on another backplane does not take the cache "
          "entry a stalled read is still filling [discovery][us1][robustness]") {
    // A stalled descriptor read is the ONE state in which this engine holds an incomplete cache
    // entry with no chunk queued for it: issue_demand() drops a stalled node's ring item, so the
    // ring goes empty and scan_identify() runs — while the stalled node's entry still holds every
    // byte it has read. If claim_descriptor() treats that entry as reusable, the next module with a
    // DIFFERENT descriptor key takes it, and the stalled node loses its reassembly buffer: its
    // retry finds no entry for its key and sends it back to IDENTIFY (core_engine.cpp's
    // resume_desc_read), so its read restarts from offset 0 rather than resuming. That contradicts
    // what this engine claims of a stalled read in core/core_types.hpp — "resumed at the bytes its
    // cache entry already holds" — and it is a module losing work to another backplane's arrival,
    // which User Story 1 AS3 forbids in that direction too.
    //
    // The second module's type is DIFFERENT deliberately: a same-key IDENTIFY never reaches
    // claim_descriptor() at all (on_identify() finds the entry and waits on it), which is why every
    // other rig in this file — like-for-like modules per backplane — cannot observe this.
    StallsMidReadBackplane stalls(omgp::MODULE_TYPE_CODES[0]);
    Descriptor other =
        make_descriptor(0x9006, 1, 1, omgp::MODULE_TYPE_CODES[1], 2u * kDescChunkMax);

    Rig rig;
    stalls.install(rig, 0x01, 1, 0b1u); // the lower node id: it identifies and claims first
    rig.install(BackplaneScript{0x02, 1, 0b1u, omgp::MODULE_TYPE_CODES[1], &other, false, false});
    rig.run_to_superframe(kConvergeSuperframes + kSettleSuperframes, byte_us());

    const std::vector<uint8_t> ids = rig.in_use_ids();
    REQUIRE(ids.size() == 2u);
    const uint8_t stalled_id = ids.front();
    const uint8_t other_id = ids.back();

    // Both modules end up described — the stalled one by resuming, not by starting over, which is
    // what the IDENTIFY count pins: one IDENTIFY means its read was never sent back to the
    // beginning. A second one is the signature of a stolen entry.
    REQUIRE(rig.engine().discovery_state(stalled_id) == DiscoveryState::Discovered);
    REQUIRE(rig.engine().discovery_state(other_id) == DiscoveryState::Discovered);
    REQUIRE(rig.transcript().count_for(omgp::OP_IDENTIFY, stalled_id) == 1u);
    REQUIRE(rig.transcript().count_for(omgp::OP_IDENTIFY, other_id) == 1u);
    // Five READ_DESC for this node and no more: the three chunks of a three-chunk descriptor plus
    // the two refusals the read itself received. A read restarted from offset 0 needs more than
    // five, so this pins the resumption as well as the IDENTIFY count above does.
    REQUIRE(rig.transcript().count_for(omgp::OP_READ_DESC, stalled_id) == 5u);
    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeDiscovered) == 2u);
    REQUIRE(rig.engine().dropped_deliveries() == 0u);
}

TEST_CASE("data-model.md §5: claim_descriptor() reuses no entry a node is still reading, and "
          "prefers a never-used one [discovery][us1][cache]") {
    // The rule behind the rig-level case above, asserted as a rule. A node in ReadingDescriptor
    // points at its entry BY KEY (find_descriptor is that pointer) and holds no refcount — the
    // count is taken in mark_discovered(), which only a COMPLETE entry reaches — so "no node still
    // pointing at it" cannot be read off refcount alone.
    using Seam = omgp::core::CoreEngineTestSeam;
    Rig rig;
    CoreEngine& engine = rig.engine();
    const size_t entries = Seam::cache_entries(engine);
    REQUIRE(entries > 1u);

    // One in-flight read per cache entry, each a DISTINCT key, each mid-reassembly. Distinct keys
    // are the point: a second claim for a key already present never reaches claim_descriptor() at
    // all (on_identify() waits on the entry it found instead).
    for (size_t i = 0; i < entries; ++i) {
        const uint8_t module_type = static_cast<uint8_t>(i + 1u);
        const uint8_t node_id = static_cast<uint8_t>(omgp::ADDR_module_min + i);
        omgp::core::DescriptorCacheEntry* claimed = Seam::claim(engine, module_type, 2000, 0xBEEF);
        REQUIRE(claimed != nullptr);
        // A never-used entry is preferred: each of these claims must land on a DIFFERENT entry, so
        // the i-th claim is the i-th entry and none of the previous ones was recycled.
        REQUIRE(claimed == &Seam::entry(engine, i));
        Seam::put_reading(engine, node_id, module_type, 2000, 0xBEEF);
    }
    // Every one of them is still findable by its own key: the capacity is 32 in-flight reads, not
    // one.
    for (size_t i = 0; i < entries; ++i) {
        REQUIRE(Seam::find(engine, static_cast<uint8_t>(i + 1u), 2000, 0xBEEF) ==
                &Seam::entry(engine, i));
    }
    // With every entry held by a live reader, a further distinct key gets R-12's graceful
    // degradation — a null, which on_identify() turns into "back to Identifying, retry later" —
    // never another node's buffer.
    REQUIRE(Seam::claim(engine, 0xF1, 300, 0x1234) == nullptr);
    for (size_t i = 0; i < entries; ++i) {
        REQUIRE(Seam::find(engine, static_cast<uint8_t>(i + 1u), 2000, 0xBEEF) ==
                &Seam::entry(engine, i));
    }
}

TEST_CASE("R-12: a cached descriptor nobody is using is not evicted while an entry has never been "
          "used [discovery][us1][cache]") {
    // The other half of the claim rule, and the reason the never-used pass exists rather than the
    // reader check alone: an entry that is complete with refcount 0 IS reclaimable — that is what
    // makes the table a cache — but reclaiming it FIRST throws away a descriptor the rig may be
    // about to need again (SC-004's "no descriptor read at all" on a re-seated module of a type
    // already seen) for nothing, while 31 entries have never been touched.
    using Seam = omgp::core::CoreEngineTestSeam;
    Rig rig;
    CoreEngine& engine = rig.engine();
    REQUIRE(Seam::cache_entries(engine) > 1u);

    omgp::core::DescriptorCacheEntry* cached = Seam::claim(engine, 0x41, 8, 0x0101);
    REQUIRE(cached == &Seam::entry(engine, 0));
    // Complete, and no node pointing at it: the state an entry is left in when the module that was
    // read is removed from its slot (reconcile_slot_map releases the refcount, R-06).
    cached->received = cached->len;
    cached->complete = true;

    REQUIRE(Seam::claim(engine, 0x42, 8, 0x0202) == &Seam::entry(engine, 1));
    // The cached descriptor is still there, still complete, and still findable by its own key.
    REQUIRE(Seam::find(engine, 0x41, 8, 0x0101) == &Seam::entry(engine, 0));
    REQUIRE(Seam::entry(engine, 0).complete);
}

TEST_CASE("AS3: one transient ERR_BUSY chunk does not lose a module when another slot never "
          "answers IDENTIFY [discovery][us1][robustness]") {
    BrieflyBusyBackplane brief(omgp::MODULE_TYPE_CODES[0]);

    Rig rig;
    brief.install(rig, 0x01, 1, 0b1u);
    rig.install(BackplaneScript{0x02, 1, 0b1u, omgp::MODULE_TYPE_CODES[2], nullptr, true, false});
    rig.run_to_superframe(kStallHorizonSuperframes + kSettleSuperframes, byte_us());

    // The one refused chunk costs the module a place in the chunk ring, not its read: it resumes
    // at the bytes already read (READ_DESC is idempotent, CLAUDE.md rule 2) and reaches Discovered.
    const std::vector<uint8_t> ids = rig.in_use_ids();
    REQUIRE(ids.size() == 2u);
    REQUIRE(rig.engine().discovery_state(ids.front()) == DiscoveryState::Discovered);
    rig.engine().drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeDiscovered) == 1u);
}

// =================================================================================================
// Mutation-kill cases (PR #871, deep-verify-mutate). Each case below pins a field or a boundary
// that the rig-level cases above reach only by coincidence: a stored value every downstream read
// happens to tolerate. They drive the engine's own operations directly through the seam, so each
// is cheap; this file is the mutation oracle and runs once per surviving mutant.
// =================================================================================================

namespace {

using Bk = omgp::core::CoreEngineTestSeam;
using omgp::core::BackplaneRecord;
using omgp::core::DescriptorCacheEntry;
using omgp::core::NodeRecord;

bool exhaustion_bit(const BackplaneRecord& bp, unsigned slot) {
    return ((bp.exhaustion_reported[slot / 8u] >> (slot % 8u)) & 1u) != 0u;
}

// A complete cache entry keyed (module_type, len, crc) carrying a recognisable MODEL_ID.
DescriptorCacheEntry& complete_entry(CoreEngine& engine, size_t i, uint8_t module_type,
                                     uint16_t len, uint16_t crc) {
    DescriptorCacheEntry& e = Bk::entry_mut(engine, i);
    e = DescriptorCacheEntry{};
    e.in_use = true;
    e.complete = true;
    e.module_type = module_type;
    e.len = len;
    e.received = len;
    e.desc_crc = crc;
    e.model_vendor = 0x1234;
    e.model_hw_rev = 0x5678;
    e.model_fw_rev = 0x9ABC;
    return e;
}

} // namespace

TEST_CASE("mark_discovered: the node takes the entry's model and one reference, and reports its "
          "own id and slot, NodeDiscovered then NodeRediscovered [discovery][mutation]") {
    Rig rig;
    CoreEngine& engine = rig.engine();
    DescriptorCacheEntry& entry = complete_entry(engine, 0, 0x21, 100, 0xBEEF);
    const uint8_t id = static_cast<uint8_t>(omgp::ADDR_module_min + 3u);
    NodeRecord& node = Bk::node(engine, id);
    node.in_use = true;
    node.slot = 7;

    Bk::mark_discovered(engine, node, id, entry);
    REQUIRE(node.descriptor == &entry);
    REQUIRE(node.model_vendor == 0x1234);
    REQUIRE(node.model_hw_rev == 0x5678);
    REQUIRE(node.model_fw_rev == 0x9ABC);
    REQUIRE(entry.refcount == 1u);
    REQUIRE(node.discovery == DiscoveryState::Discovered);
    REQUIRE(node.ever_discovered);

    // Reaching Discovered a second time is a re-arrival: the id has history now.
    Bk::mark_discovered(engine, node, id, entry);
    REQUIRE(entry.refcount == 2u);
    engine.drain_callbacks();
    REQUIRE(rig.recorder().events.size() == 2u);
    REQUIRE(rig.recorder().events[0].kind == LifecycleKind::NodeDiscovered);
    REQUIRE(rig.recorder().events[0].node_id == id);
    REQUIRE(rig.recorder().events[0].slot == 7u);
    REQUIRE(rig.recorder().events[1].kind == LifecycleKind::NodeRediscovered);
    REQUIRE(rig.recorder().events[1].node_id == id);
}

TEST_CASE("release_descriptor: gives back exactly one reference, never below zero, and detaches "
          "the node [discovery][mutation]") {
    Rig rig;
    CoreEngine& engine = rig.engine();
    DescriptorCacheEntry& entry = complete_entry(engine, 0, 0x21, 100, 0xBEEF);
    NodeRecord& node = Bk::node(engine, omgp::ADDR_module_min);

    entry.refcount = 2;
    node.descriptor = &entry;
    Bk::release_descriptor(engine, node);
    REQUIRE(entry.refcount == 1u);
    REQUIRE(node.descriptor == nullptr);

    // A node that holds no entry releases nothing.
    Bk::release_descriptor(engine, node);
    REQUIRE(entry.refcount == 1u);

    // A count already at zero stays at zero rather than wrapping.
    entry.refcount = 0;
    node.descriptor = &entry;
    Bk::release_descriptor(engine, node);
    REQUIRE(entry.refcount == 0u);
    REQUIRE(node.descriptor == nullptr);
}

TEST_CASE("reconcile_slot_map: a first assignment records the backplane's slot count and each "
          "node's own slot, and keeps the id's history [discovery][mutation]") {
    Rig rig;
    CoreEngine& engine = rig.engine();
    // The id first-fit will pick has been discovered before, on some earlier slot.
    Bk::node(engine, omgp::ADDR_module_min).ever_discovered = true;

    const uint8_t bits[1] = {0b10101};
    engine.reconcile_slot_map(omgp::ADDR_backplane_max, raw_slot_map(5, bits, bits, 1), 0);

    BackplaneRecord& bp = Bk::backplane(engine, omgp::ADDR_backplane_max);
    REQUIRE(bp.slot_count == 5u);
    const uint8_t expect_slot[3] = {0, 2, 4};
    for (uint8_t k = 0; k < 3; ++k) {
        const uint8_t id = static_cast<uint8_t>(omgp::ADDR_module_min + k);
        const NodeRecord& n = Bk::node(engine, id);
        REQUIRE(n.in_use);
        REQUIRE(n.backplane_addr == omgp::ADDR_backplane_max);
        REQUIRE(n.slot == expect_slot[k]);
        REQUIRE(n.discovery == DiscoveryState::Undiscovered);
        REQUIRE(bp.node_id_by_slot[expect_slot[k]] == id);
    }
    REQUIRE(Bk::node(engine, omgp::ADDR_module_min).ever_discovered);
    REQUIRE_FALSE(Bk::node(engine, omgp::ADDR_module_min + 1).ever_discovered);
}

TEST_CASE("reconcile_slot_map: an emptied slot releases its id and descriptor reference, reports "
          "NodeRemoved with that id and slot, and keeps the id's history [discovery][mutation]") {
    Rig rig;
    CoreEngine& engine = rig.engine();
    const uint8_t bits3[1] = {0b111};
    engine.reconcile_slot_map(0x02, raw_slot_map(3, bits3, bits3, 1), 0);
    const uint8_t id0 = omgp::ADDR_module_min;
    const uint8_t id1 = static_cast<uint8_t>(omgp::ADDR_module_min + 1u);
    const uint8_t id2 = static_cast<uint8_t>(omgp::ADDR_module_min + 2u);
    DescriptorCacheEntry& entry = complete_entry(engine, 0, 0x21, 100, 0xBEEF);
    entry.refcount = 1;
    NodeRecord& middle = Bk::node(engine, id1);
    middle.ever_discovered = true;
    middle.descriptor = &entry;
    middle.discovery = DiscoveryState::Discovered;

    const uint8_t bits_no_middle[1] = {0b101};
    engine.reconcile_slot_map(0x02, raw_slot_map(3, bits_no_middle, bits_no_middle, 1), 0);

    REQUIRE_FALSE(Bk::node(engine, id1).in_use);
    REQUIRE(Bk::node(engine, id0).in_use);
    REQUIRE(Bk::node(engine, id2).in_use);
    REQUIRE(entry.refcount == 0u);
    REQUIRE(Bk::node(engine, id1).ever_discovered);
    REQUIRE(Bk::backplane(engine, 0x02).node_id_by_slot[1] == 0u);
    engine.drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeRemoved) == 1u);
    const LifecycleEvent& removed = rig.recorder().events.back();
    REQUIRE(removed.kind == LifecycleKind::NodeRemoved);
    REQUIRE(removed.node_id == id1);
    REQUIRE(removed.slot == 1u);
}

TEST_CASE("reconcile_slot_map: a backplane that shrinks drops every slot past its new count, "
          "releasing ids and references and reporting each [discovery][mutation]") {
    Rig rig;
    CoreEngine& engine = rig.engine();
    const uint8_t bits4[1] = {0b1111};
    engine.reconcile_slot_map(0x03, raw_slot_map(4, bits4, bits4, 1), 0);
    const uint8_t id2 = static_cast<uint8_t>(omgp::ADDR_module_min + 2u);
    const uint8_t id3 = static_cast<uint8_t>(omgp::ADDR_module_min + 3u);
    DescriptorCacheEntry& entry = complete_entry(engine, 0, 0x21, 100, 0xBEEF);
    entry.refcount = 1;
    Bk::node(engine, id2).ever_discovered = true;
    Bk::node(engine, id3).descriptor = &entry;
    // A bystander id that a mis-indexed release would clear instead.
    const uint8_t bystander = static_cast<uint8_t>(omgp::ADDR_module_min + 42u);
    Bk::node(engine, bystander).in_use = true;

    const uint8_t bits2[1] = {0b11};
    engine.reconcile_slot_map(0x03, raw_slot_map(2, bits2, bits2, 1), 0);

    REQUIRE(Bk::node(engine, omgp::ADDR_module_min).in_use);
    REQUIRE(Bk::node(engine, omgp::ADDR_module_min + 1u).in_use);
    REQUIRE_FALSE(Bk::node(engine, id2).in_use);
    REQUIRE_FALSE(Bk::node(engine, id3).in_use);
    REQUIRE(Bk::node(engine, bystander).in_use);
    REQUIRE(entry.refcount == 0u);
    REQUIRE(Bk::node(engine, id2).ever_discovered);
    BackplaneRecord& bp = Bk::backplane(engine, 0x03);
    REQUIRE(bp.slot_count == 2u);
    REQUIRE(bp.node_id_by_slot[2] == 0u);
    REQUIRE(bp.node_id_by_slot[3] == 0u);
    engine.drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeRemoved) == 2u);
    REQUIRE(rig.recorder().events[0].kind == LifecycleKind::NodeRemoved);
    REQUIRE(rig.recorder().events[0].node_id == id2);
    REQUIRE(rig.recorder().events[0].slot == 2u);
    REQUIRE(rig.recorder().events[1].kind == LifecycleKind::NodeRemoved);
    REQUIRE(rig.recorder().events[1].node_id == id3);
    REQUIRE(rig.recorder().events[1].slot == 3u);
}

TEST_CASE("reconcile_slot_map: a hostile slot_count is clamped to LIMIT_bp_slot_map_max_slots "
          "and the starved slots are reported with the backplane and slot [discovery][mutation]") {
    Rig rig;
    CoreEngine& engine = rig.engine();
    uint8_t bits[(omgp::LIMIT_bp_slot_map_max_slots + 7) / 8];
    for (uint8_t& b : bits)
        b = 0xFF;
    engine.reconcile_slot_map(0x01, raw_slot_map(0xFF, bits, bits, sizeof bits), 0);

    BackplaneRecord& bp = Bk::backplane(engine, 0x01);
    REQUIRE(bp.slot_count == omgp::LIMIT_bp_slot_map_max_slots);
    const unsigned pool = omgp::ADDR_module_max - omgp::ADDR_module_min + 1u;
    const unsigned starved = omgp::LIMIT_bp_slot_map_max_slots - pool;
    engine.drain_callbacks();
    REQUIRE(rig.recorder().count(LifecycleKind::NodeIdPoolExhausted) == starved);
    for (unsigned k = 0; k < starved; ++k) {
        const LifecycleEvent& e = rig.recorder().events[k];
        REQUIRE(e.kind == LifecycleKind::NodeIdPoolExhausted);
        REQUIRE(e.node_id == 0x01u);
        REQUIRE(e.slot == pool + k);
        REQUIRE(exhaustion_bit(bp, pool + k));
    }
    REQUIRE_FALSE(exhaustion_bit(bp, 0));
}

TEST_CASE("reconcile_slot_map: a standing exhaustion bit is cleared the moment its slot is "
          "assigned, reports empty, or falls past slot_count [discovery][mutation]") {
    Rig rig;
    CoreEngine& engine = rig.engine();
    BackplaneRecord& bp = Bk::backplane(engine, 0x01);
    for (uint8_t& b : bp.exhaustion_reported)
        b = 0xFF;

    // Slots 0-1 are occupied and get ids (cleared on assignment); slots 2-7 report empty
    // (cleared on emptiness); every slot from 8 up is past slot_count (cleared on shrink).
    const uint8_t bits[1] = {0b11};
    engine.reconcile_slot_map(0x01, raw_slot_map(8, bits, bits, 1), 0);
    for (unsigned i = 0; i < sizeof bp.exhaustion_reported; ++i) {
        REQUIRE(bp.exhaustion_reported[i] == 0u);
    }
}

TEST_CASE("reconcile_slot_map: ADDR_backplane_max is a backplane and the address past it is not "
          "[discovery][mutation]") {
    Rig rig;
    CoreEngine& engine = rig.engine();
    const uint8_t bits[1] = {0b1};
    engine.reconcile_slot_map(omgp::ADDR_backplane_max, raw_slot_map(1, bits, bits, 1), 0);
    REQUIRE(rig.in_use_count() == 1u);
    REQUIRE(Bk::backplane(engine, omgp::ADDR_backplane_max).node_id_by_slot[0] != 0u);
    engine.reconcile_slot_map(static_cast<uint8_t>(omgp::ADDR_backplane_max + 1u),
                              raw_slot_map(1, bits, bits, 1), 0);
    REQUIRE(rig.in_use_count() == 1u);
}
