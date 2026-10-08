// OMGP host-core — CoreEngine implementation. See core/core_engine.hpp for the contract, the
// dependency-surface ruling (research.md R-02) and the stated divergences.
// trunk §6: run_superframe() walks one superframe's plan — status polls, demand items, one
// enrolment probe, in that order; trunk §7's retry/timeout/health rules live in link/, which
// this file never reaches past (core/ sees no frame byte). protocol-l3 §3/§3.1 is the message
// vocabulary encoded here; §4 the descriptor TLV bytes the cache holds verbatim.
// Makes tests/unit/test_core_callback_queue.cpp (T014) and tests/unit/test_core_discovery.cpp
// (T017) pass.
#include "core/core_engine.hpp"

#include "l3/l3_descriptor.hpp"
#include "l3/l3_header.hpp"
#include "l3/l3_payload.hpp"

namespace omgp {
namespace core {
namespace {

// research.md R-09, corrected 2026-09-21: the largest READ_DESC chunk the protocol can carry is
// what is left of LIMIT_max_l3_payload after the response's own `u16 offset, u8 len` head
// (protocol-l3 §3.1). Derived from the symbol, never restated — the number has already moved
// once (61 -> 56) when max_l3_payload split from max_l3_message.
constexpr uint8_t kDescChunkMax = static_cast<uint8_t>(LIMIT_max_l3_payload - 3);

// data-model.md §9 / R-11: the conservative seed for a target never yet measured — one
// worst-case frame each way at the rate now in use, plus trunk §9's turnaround and inter-frame
// gap. Deliberately pessimistic: it is what a target costs if it behaves as badly as trunk §4
// allows, and it is replaced by a real measurement the first time that target answers.
uint64_t worst_case_transaction_us(uint32_t bit_rate) {
    const uint64_t frame_us = static_cast<uint64_t>(link::kMaxWire) * link::byte_time_us(bit_rate);
    return 2u * frame_us + TRUNK_T_turn_min_us + TRUNK_T_gap_us;
}

// protocol-l3 §3.1 / data-model.md §10: bit (i % 8) of byte i/8, LSB first, is slot i. Reads
// false for every index the bitmap does not actually cover, which is what makes an over-claimed
// slot_count and a short bitmap harmless rather than an over-read (CLAUDE.md rule 7).
bool slot_bit(const l3::Bytes& bitmap, uint8_t slot) {
    const size_t byte = static_cast<size_t>(slot) / 8u;
    if (bitmap.data == nullptr || byte >= bitmap.len)
        return false;
    return ((bitmap.data[byte] >> (slot % 8u)) & 1u) != 0u;
}

// data-model.md §4 / R-06: BackplaneRecord::exhaustion_reported, bit `slot`. Bounded by the
// record's own array, never by a peer's slot_count — a slot index the array cannot hold reads
// false and writes nothing, the same rule slot_bit() above follows (CLAUDE.md rule 7).
bool exhaustion_reported(const BackplaneRecord& bp, uint8_t slot) {
    const size_t byte = static_cast<size_t>(slot) / 8u;
    // slot < slot_count <= LIMIT_bp_slot_map_max_slots here (reconcile_slot_map() clamps
    // slot_count before any slot is used), so byte never reaches sizeof: the >= / > boundary is
    // not reachable today. A CONTROL on that clamp, not a guarantee (CLAUDE.md rule 11).
    // mutant-ok(equivalent, cxx_ge_to_gt): byte == sizeof is unreachable behind the caller's clamp.
    if (byte >= sizeof bp.exhaustion_reported)
        return false;
    return ((bp.exhaustion_reported[byte] >> (slot % 8u)) & 1u) != 0u;
}

void set_exhaustion_reported(BackplaneRecord& bp, uint8_t slot, bool reported) {
    const size_t byte = static_cast<size_t>(slot) / 8u;
    // mutant-ok(equivalent, cxx_ge_to_gt): byte == sizeof is unreachable; see above.
    if (byte >= sizeof bp.exhaustion_reported)
        return;
    const uint8_t mask = static_cast<uint8_t>(1u << (slot % 8u));
    if (reported) {
        bp.exhaustion_reported[byte] = static_cast<uint8_t>(bp.exhaustion_reported[byte] | mask);
    } else {
        bp.exhaustion_reported[byte] =
            static_cast<uint8_t>(bp.exhaustion_reported[byte] & static_cast<uint8_t>(~mask));
    }
}

} // namespace

CoreEngine::CoreEngine(link::ByteWire& wire, Clock& clock, uint8_t host_addr,
                       CoreCallbacks callbacks)
    // R-02: both collaborators are owned BY VALUE — no references, no pointers, no heap.
    // R-03: `*this` is the link::HealthListener the tracker notifies. Legal here: the
    // HealthListener base subobject is fully constructed before any member initialiser runs, and
    // HealthTracker's own constructor does not notify (link/health.cpp).
    : master_(wire, clock, host_addr), health_(clock, *this), clock_(clock), callbacks_(callbacks),
      // Every table's start-of-life value is the default member initialiser declared beside the
      // field in core/core_types.hpp, so these are empty braces rather than positional lists to
      // keep in step with those structs (the idiom link/health.cpp:73-77 uses one layer down).
      nodes_{}, backplanes_{}, descriptors_{}, event_queue_{}, desc_queue_{}, param_queue_{},
      request_id_used_{}, pending_lifecycle_{}, pending_param_results_{}, budget_{} {
    // Discarded reads of the state this engine declares but does not yet touch — the
    // link/health.cpp:78 idiom, kept for the same reason: a private member nothing in the TU
    // references is what clang's -Wunused-private-field (in -Wall, and this project builds
    // -Werror) reports, and the contract requires these declarations to exist from T015 on.
    // PRECAUTIONARY, not measured: the native build is g++, which has no such warning. Each
    // named consumer is the task that removes its line:
    //   clock_        — T029's superframe cadence bookkeeping (every time this engine reads
    //                   today arrives as run_superframe()'s own now_us argument, rule 3)
    //   event_queue_  — T029's demand drain for User Story 4 (R-07); param_queue_ left this
    //                   list at T027, which fills it from set_param()/get_param()
    (void)clock_;
    (void)event_queue_;
}

void CoreEngine::run_superframe(uint64_t now_us) {
    // trunk §6, in order. link::Master::poll() is the only receive path
    // (specs/002-trunk-link-layer/data-model.md §4), so it runs first whatever else happens:
    // an outstanding transaction's terminal event must be applied before the plan can move on.
    const link::MasterEvent ev = master_.poll(now_us);
    if (ev.kind != link::MasterEvent::None) {
        complete_request(ev, now_us);
    }
    // trunk §3: one transaction at a time. Nothing else can be issued while one is open — and
    // busy() is already true for a transaction link::Master has merely DEFERRED to satisfy
    // T_gap, which is what keeps this engine from queue-jumping its own inter-frame gap.
    if (master_.busy()) {
        return;
    }
    if (!superframe_open_) {
        open_superframe(now_us);
    }
    if (issue_next(now_us)) {
        return;
    }
    // The plan is spent. The NEXT call opens the next superframe rather than this one doing it
    // and issuing again: one link::Master::begin() per call is what makes the transcript a
    // function of the plan and not of the caller's cadence (spec SC-005).
    // The `x = false` stores below on a bool: Mull's cxx_assign_const writes 42, and a bool reads
    // its low bit, which is 0 — the value the original stores. ASSUMED from Mull's IR semantics,
    // not measured (CLAUDE.md rule 11).
    // mutant-ok(equivalent, cxx_assign_const): 42 stored in a bool reads false (assumed).
    superframe_open_ = false;
    // phase_ is already Closed at every `return false` of issue_next() — it is set at the top of
    // the probe phase, before anything that can return false — and nothing reads it between here
    // and the next open_superframe(), which sets it again. Proved by reading issue_next(), a
    // structural property of today's code (a control, not a guarantee).
    // mutant-ok(equivalent, cxx_assign_const): a redundant store; phase_ is already Closed.
    phase_ = Phase::Closed;
}

void CoreEngine::open_superframe(uint64_t now_us) {
    ++superframe_;
    superframe_open_ = true;
    phase_ = Phase::Poll;
    poll_cursor_ = ADDR_backplane_min;
    // mutant-ok(equivalent, cxx_assign_const): 42 stored in a bool reads false (assumed).
    probe_issued_ = false;
    // mutant-ok(equivalent, cxx_assign_const): 42 stored in a bool reads false (assumed).
    demand_issued_ = false;
    // data-model.md §9: the period is TRUNK_T_poll_us, read from the generated header and never
    // restated (CLAUDE.md rule 4); the budget is reset to it at the start of every superframe
    // and debited by each transaction's own MEASURED duration as it concludes (FR-027).
    budget_.period_us = TRUNK_T_poll_us;
    budget_.remaining_us = budget_.period_us;
    // trunk §7's one time-only transition (SUSPECT -> OFFLINE). Once per superframe, on the
    // tracker this engine owns; the engine itself never ages anything.
    health_.tick(now_us);
}

bool CoreEngine::issue_next(uint64_t now_us) {
    // trunk §6 part 1: one status poll per enrolled backplane, GET_STATUS and BP_SLOT_MAP on
    // alternate superframes. Before anything else in this superframe (spec FR-002).
    if (phase_ == Phase::Poll) {
        while (poll_cursor_ <= ADDR_backplane_max) {
            const uint8_t addr = poll_cursor_++;
            const size_t idx = backplane_index(addr);
            if (idx == kNoNodeIndex || !backplanes_[idx].enrolled) {
                continue;
            }
            // F3 obligation 1 (contracts/core-cpp.md): poll_due() is already false for every
            // address while health_.bus_fault(), so a declared fault silences the whole of this
            // phase without a second test here.
            if (!health_.poll_due(addr, now_us)) {
                continue;
            }
            const bool slot_map = (superframe_ & 1u) != 0u;
            // F3 obligation 2: exactly once per poll ACTUALLY issued. Called before begin()
            // because begin() cannot fail for a well-formed request to a trunk address — and if
            // it did, the poll is spent either way: the alternative (mark after a successful
            // begin) leaves poll_due() true and re-polls the same address later in the same
            // superframe, which FR-002 reads as one poll per backplane, not one per attempt.
            health_.mark_polled(addr, now_us);
            if (begin_request(addr, addr, slot_map ? OP_BP_SLOT_MAP : OP_GET_STATUS, nullptr, 0,
                              slot_map ? TxKind::SlotMap : TxKind::StatusPoll, now_us)) {
                return true;
            }
        }
        phase_ = Phase::Demand;
    }
    // trunk §6 part 2: demand slots, under the superframe budget (FR-004/FR-005/FR-027).
    if (phase_ == Phase::Demand) {
        if (issue_demand(now_us)) {
            return true;
        }
        phase_ = Phase::Probe;
    }
    // trunk §6 part 3: exactly one enrolment probe (spec FR-003), unconditional (FR-028) and
    // outside the demand budget — see run_superframe()'s note in the header.
    if (phase_ == Phase::Probe) {
        phase_ = Phase::Closed;
        if (!probe_issued_) {
            probe_issued_ = true;
            // F3 obligation 2 again: one next_probe() call per probe issued, never
            // speculatively — so this is called here and its result is used or discarded, never
            // re-requested.
            const link::Probe probe = health_.next_probe(now_us);
            // link/health.hpp: ADDR_host is the "no candidate" sentinel and is never a real
            // target. Checked before the enrolment slot is spent, as the contract requires.
            if (probe.addr != ADDR_host) {
                if (probe.bit_rate != wire_rate_) {
                    // F3 obligation 3's wire half: the probe's own rate goes to the wire in the
                    // same step it is handed out. NOT to health_.set_bit_rate() — that is a rate
                    // SELECTION by the layer above (link/health.hpp), which this feature does
                    // not expose in v1, and calling it here would clear-classify a fault-time
                    // probe's own alternation as a selection.
                    master_.set_bit_rate(probe.bit_rate);
                    wire_rate_ = probe.bit_rate;
                }
                return begin_request(probe.addr, probe.addr, OP_PING, nullptr, 0, TxKind::Probe,
                                     now_us);
            }
        }
    }
    return false;
}

bool CoreEngine::issue_demand(uint64_t now_us) {
    // F3 obligation 1: while a bus fault is declared, probes are the only traffic (trunk §7).
    if (health_.bus_fault()) {
        return false;
    }
    // R-07's fixed order is event drains, then descriptor chunks, then parameter operations.
    // User Story 1 owns only the middle one; the event queue (US4) and the parameter queue
    // (US2) are drained by T029's loop, which lands with their own stories. The IDENTIFY scan
    // below is discovery traffic with no ring of its own (R-07 names three kinds, not four) and
    // sits after the descriptor chunks deliberately: finishing a descriptor read already in
    // flight releases a node to Discovered, while starting another node's identity does not.
    // Iterative, not recursive: the queue holds up to LIMIT_max_nodes items and a stale-item
    // chain must not become that many stack frames on a target with a fixed task stack
    // (CLAUDE.md rule 5's fixed-size reading). Terminates because every pass either returns or
    // consumes one item from a finite ring.
    while (desc_held_ || desc_queue_.pop(desc_hold_)) {
        desc_held_ = true;
        const size_t idx = node_index(desc_hold_.node_id);
        if (idx == kNoNodeIndex || !nodes_[idx].in_use ||
            nodes_[idx].discovery != DiscoveryState::ReadingDescriptor) {
            // The slot emptied (FR-010) or the read was abandoned while this item waited: drop
            // it rather than addressing a node id that no longer names what queued it.
            // mutant-ok(equivalent, cxx_assign_const): 42 stored in a bool reads false (assumed).
            desc_held_ = false;
            continue;
        }
        if (nodes_[idx].desc_stalled_superframe != 0) {
            // This node's last chunk made NO progress (NodeRecord::desc_stalled_superframe): an
            // ERROR answer, an answer for the wrong offset, or a timeout. Such an answer re-queues
            // the same offset, so a node whose READ_DESC is refused for ever — trunk §8's mandated
            // ERR_BUSY above all — keeps this ring permanently non-empty; and because the ring is
            // drained FIRST, it would take the one demand item a busy superframe admits, every
            // superframe, and no other node would ever be identified at all (User Story 1 AS3: a
            // slot whose module does not answer "does not block the discovery of any other slot or
            // backplane"). Measured before this: 893 READ_DESC to one busy backplane in 301
            // superframes, 0 IDENTIFY to the other, 0 of 4 honest modules discovered.
            //
            // So a stalled node loses its PLACE, not its retry: the item is dropped here, and
            // retry_stalled_desc() below retries the node — at most once per superframe, from its
            // own rotating cursor, and alternating with the IDENTIFY scan by superframe parity.
            // Dropping is safe because the item carries nothing the entry does not: the node stays
            // in ReadingDescriptor and is resumed at the bytes already read, and READ_DESC is
            // idempotent (CLAUDE.md rule 2).
            // mutant-ok(equivalent, cxx_assign_const): 42 stored in a bool reads false (assumed).
            desc_held_ = false;
            continue;
        }
        NodeRecord& node = nodes_[idx];
        if (!admit_demand(cost_estimate(node.last_measured_duration_us))) {
            return false; // FR-005: carries over, keeping its place at the head of the queue
        }
        if (begin_desc_chunk(desc_hold_.node_id, desc_hold_.offset, now_us)) {
            // mutant-ok(equivalent, cxx_assign_const): 42 stored in a bool reads false (assumed).
            desc_held_ = false;
            return true;
        }
        return false;
    }
    // Recovery sweep, reached only with the chunk ring EMPTY. A node in ReadingDescriptor with
    // nothing queued for it is one whose read was interrupted: the entry it was waiting on was
    // abandoned, or the node that was filling it had its own slot emptied (FR-010) and its
    // chunk item was dropped with it. Without this, every node waiting on that entry — a
    // like-for-like backplane's whole complement — would sit in ReadingDescriptor for ever.
    // It cannot interfere with a healthy read: while one is in progress the ring is non-empty
    // (each answered chunk queues the next), so this loop is not reached at all. Ascending node
    // id, with no cursor of its own: every node it can serve is one whose LAST chunk made
    // progress (a stalled one is skipped here and retried by retry_stalled_desc()), so no node
    // can hold this sweep's head against the others the way a permanently-failing one could.
    for (uint8_t id = ADDR_module_min; id <= ADDR_module_max; ++id) {
        NodeRecord& node = nodes_[node_index(id)];
        if (!node.in_use || node.discovery != DiscoveryState::ReadingDescriptor) {
            continue;
        }
        if (node.desc_stalled_superframe != 0) {
            continue; // retry_stalled_desc()'s business, not this one's
        }
        const Resume outcome = resume_desc_read(id, node, now_us);
        if (outcome == Resume::Issued) {
            return true;
        }
        if (outcome == Resume::Refused) {
            return false;
        }
    }
    // The IDENTIFY scan and the stalled-read retry ALTERNATE by superframe parity, and neither is
    // permanently ahead of the other. Fixing an order between them starves whichever comes second,
    // in whichever direction the order is fixed, because BOTH can have work for ever:
    //
    //   * the IDENTIFY scan includes nodes already Identifying (see its own note), and a module
    //     that never answers stays Identifying by design (AS3: retried, never abandoned) — so with
    //     the scan first, a single unanswering slot means the retry pass is never REACHED at all,
    //     and one spec-mandated ERR_BUSY (trunk §8) loses an otherwise-honest module for ever;
    //   * a node whose READ_DESC is refused for ever is stalled again the moment it is retried —
    //     so with the retry first and no parity, it takes the first demand item of every
    //     superframe and, when the budget admits only one, no node is ever identified.
    //
    // Both directions are AS3's own criterion ("does not block the discovery of any other slot or
    // backplane"), so the schedule has to satisfy it both ways. Parity does: each pass gets the
    // whole demand phase of every second superframe ahead of the other, so neither can be held off
    // for more than one superframe however the other behaves. The parity is read from superframe_,
    // which is also what makes the transcript a pure function of the plan (FR-019/SC-001) — no
    // wall-clock, no call count.
    const bool retry_first = (superframe_ & 1u) != 0u;
    // `pass < 2u` -> `pass <= 2u` adds a third pass, and for pass >= 1 `retry` is !retry_first, so
    // it repeats pass 1's call. That call returned Nothing (anything else returned above) and
    // left nothing a repeat could act on: scan_identify() changes no state when it finds nothing,
    // and retry_stalled_desc() leaves every node it passed over out of ReadingDescriptor. Argued
    // from the two callees, not measured.
    // mutant-ok(equivalent, cxx_lt_to_le): a third pass repeats pass 1, which found nothing.
    for (unsigned pass = 0; pass < 2u; ++pass) {
        const bool retry = ((pass == 0u) == retry_first);
        const Resume outcome = retry ? retry_stalled_desc(now_us) : scan_identify(now_us);
        if (outcome == Resume::Issued) {
            return true;
        }
        if (outcome == Resume::Refused) {
            return false;
        }
    }
    return false;
}

CoreEngine::Resume CoreEngine::scan_identify(uint64_t now_us) {
    // spec FR-006: a node id whose module has not yet answered IDENTIFY. Ascending node id, but
    // from a ROTATING start (identify_cursor_, advanced past each id this scan actually issues
    // for) rather than from ADDR_module_min every time. Fairness is load-bearing here, not a
    // refinement: a node stays in Identifying until it answers with an identity, so with a fixed
    // start the lowest such id takes EVERY demand slot of every superframe and no higher id is
    // ever identified — which is exactly what trunk §8's ERR_UNKNOWN_TARGET slot (User Story 1
    // AS3) or a module whose descriptor answer keeps being refused looks like. Measured before
    // this rotation existed: one unanswering module at ADDR_module_min took all 264 IDENTIFY
    // requests of 90 superframes and four honest modules on another backplane were never
    // identified at all. The transcript stays reproducible (FR-019/SC-001): the cursor moves only
    // when a request is issued, so the order remains a pure function of the engine's own state
    // sequence, not of wall-clock or caller cadence.
    // Identifying is included, not only Undiscovered: a module that did not answer is RETRIED,
    // never abandoned (User Story 1 AS3), and data-model.md §2 keeps a node whose IDENTIFY is
    // in flight or unanswered in exactly that state.
    const uint16_t id_pool = static_cast<uint16_t>(ADDR_module_max - ADDR_module_min + 1);
    // n == id_pool would revisit the node n == 0 named, which this loop already passed over as
    // ineligible (an eligible one returned). Argued from the loop body, not measured.
    // mutant-ok(equivalent, cxx_lt_to_le): n == id_pool revisits the n == 0 node, already skipped.
    for (uint16_t n = 0; n < id_pool; ++n) {
        const uint16_t offset = static_cast<uint16_t>((identify_cursor_ + n) % id_pool);
        const uint8_t id = static_cast<uint8_t>(ADDR_module_min + offset);
        NodeRecord& node = nodes_[node_index(id)];
        if (!node.in_use) {
            continue;
        }
        if (node.discovery != DiscoveryState::Undiscovered &&
            node.discovery != DiscoveryState::Identifying) {
            continue;
        }
        if (!admit_demand(cost_estimate(node.last_measured_duration_us))) {
            return Resume::Refused;
        }
        if (begin_request(node.backplane_addr, id, OP_IDENTIFY, nullptr, 0, TxKind::Identify,
                          now_us)) {
            identify_cursor_ = static_cast<uint8_t>((offset + 1u) % id_pool);
            // data-model.md §2: Identifying is "IDENTIFY in flight, or its result known but not
            // yet checked against the cache" — entered when the request goes out, left only for
            // ReadingDescriptor or Discovered.
            node.discovery = DiscoveryState::Identifying;
            demand_issued_ = true;
            return Resume::Issued;
        }
        return Resume::Refused;
    }
    return Resume::Nothing;
}

CoreEngine::Resume CoreEngine::retry_stalled_desc(uint64_t now_us) {
    // One retry for a node whose descriptor read is STALLED — its last chunk was refused (trunk
    // §8's ERR_BUSY), answered for the wrong offset, or timed out. The chunk ring dropped its item
    // and the recovery sweep skips it, so this is the ONLY pass that can serve such a node; it is
    // a DIVERGENCE from R-07's fixed ring order, stated: R-07 orders the three RINGS, and this is
    // not a fourth ring but the same descriptor work rescheduled against the IDENTIFY scan.
    // At most one retry per superframe per node (desc_stalled_superframe), from a rotating cursor
    // so two stalled nodes take turns, and "retried, not abandoned" (AS3) is what the retry itself
    // is: the node keeps its cache entry and its place, for as long as it keeps answering that way.
    const uint16_t id_pool = static_cast<uint16_t>(ADDR_module_max - ADDR_module_min + 1);
    // As in scan_identify(): n == id_pool revisits the n == 0 node. If resume_desc_read() returned
    // Nothing for it, it has left ReadingDescriptor (Identifying or Discovered), so the revisit
    // skips it. Argued from resume_desc_read(), not measured.
    // mutant-ok(equivalent, cxx_lt_to_le): n == id_pool revisits the n == 0 node, already skipped.
    for (uint16_t n = 0; n < id_pool; ++n) {
        const uint16_t offset = static_cast<uint16_t>((desc_retry_cursor_ + n) % id_pool);
        const uint8_t id = static_cast<uint8_t>(ADDR_module_min + offset);
        NodeRecord& node = nodes_[node_index(id)];
        if (!node.in_use || node.discovery != DiscoveryState::ReadingDescriptor) {
            continue;
        }
        if (node.desc_stalled_superframe == 0 || node.desc_stalled_superframe == superframe_) {
            continue; // not stalled at all, or already had this superframe's one attempt
        }
        const Resume outcome = resume_desc_read(id, node, now_us);
        if (outcome == Resume::Issued) {
            desc_retry_cursor_ = static_cast<uint8_t>((offset + 1u) % id_pool);
            return Resume::Issued;
        }
        if (outcome == Resume::Refused) {
            return Resume::Refused;
        }
    }
    return Resume::Nothing;
}

CoreEngine::Resume CoreEngine::resume_desc_read(uint8_t node_id, NodeRecord& node,
                                                uint64_t now_us) {
    DescriptorCacheEntry* entry = find_descriptor(node.module_type, node.desc_len, node.desc_crc);
    if (entry == nullptr) {
        node.discovery = DiscoveryState::Identifying; // start again from IDENTIFY
        return Resume::Nothing;
    }
    if (entry->complete) {
        mark_discovered(node, node_id, *entry);
        return Resume::Nothing;
    }
    if (!admit_demand(cost_estimate(node.last_measured_duration_us))) {
        return Resume::Refused;
    }
    // Resumes at what the entry already holds, not from zero: the bytes read so far are still the
    // right bytes, and READ_DESC is idempotent (CLAUDE.md rule 2).
    return begin_desc_chunk(node_id, entry->received, now_us) ? Resume::Issued : Resume::Refused;
}

bool CoreEngine::begin_desc_chunk(uint8_t node_id, uint16_t offset, uint64_t now_us) {
    const size_t idx = node_index(node_id);
    if (idx == kNoNodeIndex) {
        return false;
    }
    // protocol-l3 §3.1: READ_DESC's request is `u16 offset, u8 max_len`. R-09's chunk size.
    l3::ReadDescReq req{};
    req.offset = offset;
    req.max_len = kDescChunkMax;
    uint8_t payload[LIMIT_max_l3_payload] = {};
    // `written` is set by the encoder on Ok, the only branch that reads it, so its initial value
    // is never observed (the encoder's contract, shown by every READ_DESC frame the discovery
    // tests decode; not a guarantee of the encoder).
    // mutant-ok(equivalent, cxx_init_const): overwritten by the encoder before any read.
    size_t written = 0;
    if (l3::encode_read_desc_req(req, payload, sizeof payload, written) != l3::Status::Ok) {
        return false;
    }
    tx_offset_ = offset;
    if (!begin_request(nodes_[idx].backplane_addr, node_id, OP_READ_DESC, payload,
                       static_cast<uint8_t>(written), TxKind::ReadDesc, now_us)) {
        return false;
    }
    demand_issued_ = true;
    return true;
}

uint64_t CoreEngine::cost_estimate(uint64_t last_measured_us) const {
    // data-model.md §9 / R-11 as corrected by spec FR-027: a real measurement once one exists,
    // the conservative seed until then — never a fixed per-item cost, and never the worst case
    // once this target has actually been timed.
    return last_measured_us != 0 ? last_measured_us : worst_case_transaction_us(wire_rate_);
}

bool CoreEngine::admit_demand(uint64_t estimate) const {
    // The first-item exception (see run_superframe()'s header note): without it, three
    // backplanes' mandatory status polls alone can leave less than one unmeasured target's
    // conservative seed, and no demand item is EVER admitted — discovery livelocks rather than
    // running slowly. Every later item of the same superframe is admitted only if its own
    // estimate still fits what the measured cost of this superframe's traffic has left.
    if (!demand_issued_) {
        return true;
    }
    return estimate <= budget_.remaining_us;
}

void CoreEngine::drain_callbacks(size_t max_deliveries) {
    // contracts/core-cpp.md §Engine: the ONLY site that invokes either CoreCallbacks pointer.
    //
    // Not re-entrant, BY CONSTRUCTION rather than by precondition: a call made from inside a
    // callback delivers nothing and returns at once. Without this latch each nested call would
    // deliver one more item and recurse again, so the stack would grow with the queue depth — up
    // to LIMIT_max_nodes frames in one top-level call, which on the target's fixed task stack is
    // an overflow rather than a deep-but-survivable call (CLAUDE.md rule 5's "fixed-size" reading
    // of the same rule that bans the heap). Nothing is lost: the item a suppressed nested call
    // would have delivered is still queued, and the drain already in progress or the next
    // top-level one delivers it. Recorded, with the alternatives, in docs/OPEN-QUESTIONS.md
    // (2026-09-27).
    if (draining_) {
        return;
    }
    draining_ = true;

    // The bound is min(max_deliveries, what was pending AT ENTRY) — ONE budget spent across both
    // rings, not one per ring (contracts/core-cpp.md §Engine: "at most `max_deliveries` queued
    // CoreCallbacks calls"). Snapshotting the two sizes is what makes "everything currently
    // pending" literal: a callback that enqueues (only an engine-internal producer or T014's seam
    // can, since the rings are private) has its item delivered by the NEXT call, not this one, so
    // this loop cannot be extended by its own deliveries. True by construction — the counters are
    // read once, before any callback runs.
    size_t remaining = max_deliveries;
    size_t lifecycle_budget = pending_lifecycle_.size();
    size_t param_budget = pending_param_results_.size();

    // The two rings are served in ALTERNATION, with the starting ring carried across calls in
    // next_is_param_. data-model.md §8a fixes FIFO within each ring and says nothing across the
    // two, so the cross-ring order is this engine's choice — but "all of one ring, then the
    // other" is not a free choice: under a bounded drain whose budget is at or below one ring's
    // arrival rate, the second ring is reached NEVER, and its deliveries are then destroyed by
    // the drop-newest rule (per R-10 a ParamRequestId is only released when its result is
    // delivered, so a permanently starved result ring is how the get_param id pool would wedge).
    // Alternating bounds each stream's starvation at one delivery behind the other instead.
    // Nothing should be built on WHICH ring a given drain starts with; the guarantee here is
    // per-ring FIFO plus this fairness, not a fixed interleaving.
    LifecycleEvent ev{};
    ParamResultDelivery delivery{};
    while (remaining > 0 && (lifecycle_budget > 0 || param_budget > 0)) {
        const bool take_param = param_budget > 0 && (next_is_param_ || lifecycle_budget == 0);
        if (take_param) {
            if (!pending_param_results_.pop(delivery)) {
                break; // unreachable while param_budget <= size(): defensive, not a known path
            }
            --param_budget;
            --remaining;
            // R-10: an id is outstanding until its result is DELIVERED, and this is that
            // moment — released before the callback runs, so a get_param() made from inside
            // on_param_result correctly sees the id as free again. Released whether or not the
            // pointer below is null: the delivery is consumed either way, so an id held back
            // for a null callback would be one the pool never got back.
            release_request_id(delivery.id);
            // Argued like link/master.cpp:400 and link/health.cpp:700, not assumed: the mutant
            // either reads back false and coincides with the original, or leaves next_is_param_
            // set through every param delivery — which serves the param ring until its budget is
            // spent and starves the lifecycle ring behind it, so "a bounded drain serves both
            // rings" (hi - lo <= 1 across 16 ticks) would see 1 and 15. That case ran, and this
            // mutant survived it (CI run 36321491019), so it reads false.
            // mutant-ok(equivalent, cxx_assign_const): the mutation and the original coincide.
            next_is_param_ = false;
            // R-10: the result is correlated to its request by the id it is delivered with.
            if (callbacks_.on_param_result != nullptr) {
                callbacks_.on_param_result(callbacks_.ctx, delivery.id, delivery.result);
            }
        } else {
            if (!pending_lifecycle_.pop(ev)) {
                break; // likewise
            }
            --lifecycle_budget;
            --remaining;
            next_is_param_ = true;
            // A null pointer is a host that wants the polling loop without this stream (R-04:
            // CoreCallbacks' members default to nullptr). The delivery is still CONSUMED —
            // calling through the null pointer would be undefined behaviour, and leaving the item
            // queued would fill the ring and start counting drops against an application that
            // asked for nothing. The budget is charged either way: `max_deliveries` bounds the
            // deliveries a call consumes, not only the ones that reach a function pointer.
            if (callbacks_.on_lifecycle != nullptr) {
                callbacks_.on_lifecycle(callbacks_.ctx, ev);
            }
        }
    }

    // Same argument, same evidence: the mutant either reads back false, or leaves the latch set
    // after this top-level drain returns and makes every later drain_callbacks() a no-op — which
    // "drain_callbacks(n) delivers at most n and leaves the remainder pending" (5 delivered after
    // the second drain) and the re-entrancy case's closing "a later top-level drain still works"
    // would both fail on. Both ran, and this mutant survived them (CI run 36321491019).
    // mutant-ok(equivalent, cxx_assign_const): the mutation and the original coincide.
    draining_ = false;
}

bool CoreEngine::begin_request(uint8_t dst, uint8_t node_id, uint8_t opcode, const uint8_t* payload,
                               uint8_t len, TxKind kind, uint64_t now_us) {
    // protocol-l3 §3: opcode, node_id, seq, flags, payload_len — a REQUEST, so flags bit0
    // (FLAG_response) is clear and every reserved bit stays 0 (the encoder refuses otherwise).
    l3::Header header{};
    header.opcode = opcode;
    header.node_id = node_id;
    header.seq = l3_seq_;
    header.flags = 0;
    header.payload_len = len;
    // mutant-ok(equivalent, cxx_init_const): overwritten by encode_header() before any read.
    size_t written = 0;
    if (l3::encode_header(header, request_buf_, sizeof request_buf_, written) != l3::Status::Ok) {
        return false;
    }
    // encode_header() accepted payload_len == len, so len <= LIMIT_max_l3_payload and written ==
    // HEADER_LEN; request_buf_ is LIMIT_max_l3_message bytes (core_engine.hpp), so
    // len <= sizeof request_buf_ - written already. The guard is a second bound that cannot fire
    // while those constants hold (a control on the constants), with either operator.
    // mutant-ok(equivalent, cxx_sub_to_add): the guard cannot fire with - or +; see above.
    if (static_cast<size_t>(len) > sizeof request_buf_ - written) {
        return false;
    }
    for (uint8_t i = 0; i < len; ++i) {
        request_buf_[written + i] = payload[i];
    }
    if (master_.begin(dst, request_buf_, written + len) != link::Status::Ok) {
        return false;
    }
    ++l3_seq_;
    tx_kind_ = kind;
    tx_addr_ = dst;
    tx_node_ = node_id;
    tx_issued_us_ = now_us;
    if (transcript_ != nullptr) {
        const TranscriptEntry entry{superframe_, opcode, dst, node_id};
        transcript_(transcript_ctx_, entry);
    }
    return true;
}

void CoreEngine::complete_request(const link::MasterEvent& ev, uint64_t now_us) {
    const TxKind kind = tx_kind_;
    tx_kind_ = TxKind::None;
    if (kind == TxKind::None) {
        return; // a terminal event with nothing outstanding: not a path this engine creates
    }
    const bool answered = ev.kind == link::MasterEvent::Answered;
    // spec FR-027: the ACTUAL elapsed time of the transaction, from the injected time source
    // only (CLAUDE.md rule 3). Monotonic by the Clock contract, so the subtraction cannot wrap;
    // written the defensive way anyway, since a caller that rewinds now_us would otherwise turn
    // one bad call into a budget of ~2^64 microseconds.
    // At equality both forms give 0: now_us - tx_issued_us_ is 0 and so is the else branch.
    // mutant-ok(equivalent, cxx_ge_to_gt): at now_us == tx_issued_us_ both branches give 0.
    const uint64_t duration_us = now_us >= tx_issued_us_ ? now_us - tx_issued_us_ : 0u;
    budget_.remaining_us =
        // At equality both forms give 0: remaining_us - duration_us is 0 and so is the then branch.
        // mutant-ok(equivalent, cxx_ge_to_gt): at duration_us == remaining_us both branches give 0.
        duration_us >= budget_.remaining_us ? 0u : budget_.remaining_us - duration_us;

    // trunk §6/§7: every transaction's outcome is this trunk address's outcome, whether it
    // carried a status poll, a probe or a bridged module request — the health table is keyed by
    // L2 address (link/health.hpp) and knows nothing of module ids (R-03).
    health_.on_result(tx_addr_, answered, now_us);

    const size_t bp_idx = backplane_index(tx_addr_);
    if (bp_idx != kNoNodeIndex) {
        backplanes_[bp_idx].last_measured_duration_us = duration_us;
    }
    const size_t node_idx = node_index(tx_node_);
    if (node_idx != kNoNodeIndex) {
        nodes_[node_idx].last_measured_duration_us = duration_us;
    }
    if (!answered) {
        // trunk §7 already retried this inside link::Master. Nothing is re-queued here: a node
        // still Identifying is picked up again by the next superframe's own scan, and an
        // unfinished descriptor read re-queues its chunk below only on an answer — the node
        // stays in ReadingDescriptor and its leader's entry keeps what it has, so the read
        // resumes rather than restarting. AS3's "retried, not abandoned" is that scan.
        if (kind == TxKind::ReadDesc && node_idx != kNoNodeIndex) {
            // No progress was made, so this node has spent its one chunk attempt for this
            // superframe (NodeRecord::desc_stalled_superframe): the re-queued item below must not
            // be able to hold issue_demand()'s chunk ring against every other node.
            nodes_[node_idx].desc_stalled_superframe = superframe_;
            DescChunkItem again{};
            again.node_id = tx_node_;
            again.offset = tx_offset_;
            (void)desc_queue_.push(again);
        }
        return;
    }
    switch (kind) {
    case TxKind::StatusPoll:
        // on_status_block() is empty today (it only discards now_us), so removing the call changes
        // nothing observable. It becomes killable the moment User Story 4 gives it a body.
        // mutant-ok(equivalent, cxx_remove_void_call): on_status_block() has an empty body today.
        on_status_block(now_us);
        break;
    case TxKind::SlotMap:
        on_slot_map(ev, now_us);
        break;
    case TxKind::Identify:
        on_identify(ev, now_us);
        break;
    case TxKind::ReadDesc:
        on_desc_chunk(ev, now_us);
        break;
    case TxKind::Probe:
    case TxKind::None:
        // A probe's whole outcome is health_.on_result() above (trunk §6's enrolment rotation);
        // its payload carries nothing this engine reads.
        break;
    }
}

void CoreEngine::on_status_block(uint64_t now_us) {
    // trunk §6: the status poll "also collects event_pending counts" (spec FR-002). Draining
    // those events is User Story 4's (T038 and the event ring), not this story's — and a
    // backplane's own status block is decoded there, where the fields are used. Nothing here
    // depends on its contents; the transaction's success is the whole of what discovery needs.
    (void)now_us;
}

void CoreEngine::on_slot_map(const link::MasterEvent& ev, uint64_t now_us) {
    l3::Header hdr{};
    l3::Bytes payload{};
    if (l3::decode_message(ev.response.payload, ev.response.len, hdr, payload) != l3::Status::Ok) {
        return;
    }
    // protocol-l3 §3.1: an ERROR answer is opcode OP_ERROR with flags.error set, not a
    // BP_SLOT_MAP payload. A backplane that refuses the poll is simply not reconciled this
    // superframe; its occupancy is absolute state and re-converges on the next one.
    if (hdr.opcode != OP_BP_SLOT_MAP) {
        return;
    }
    l3::BpSlotMapResp resp{};
    if (l3::decode_bp_slot_map_resp(payload.data, payload.len, resp) != l3::Status::Ok) {
        return;
    }
    reconcile_slot_map(tx_addr_, resp, now_us);
}

void CoreEngine::on_identify(const link::MasterEvent& ev, uint64_t now_us) {
    (void)now_us;
    const size_t idx = node_index(tx_node_);
    if (idx == kNoNodeIndex || !nodes_[idx].in_use) {
        return; // the slot emptied while this IDENTIFY was in flight
    }
    NodeRecord& node = nodes_[idx];
    l3::Header hdr{};
    l3::Bytes payload{};
    if (l3::decode_message(ev.response.payload, ev.response.len, hdr, payload) != l3::Status::Ok) {
        return;
    }
    if (hdr.opcode != OP_IDENTIFY) {
        // trunk §8: a backplane answers on a slot's behalf — ERR_UNKNOWN_TARGET for a slot with
        // no live module, ERR_BUSY while the module bus is still working — rather than holding
        // the trunk response open. Either is a valid L2 answer and not an identity: the node
        // stays Identifying and the next superframe's scan tries again (User Story 1 AS3).
        return;
    }
    l3::IdentifyResp resp{};
    if (l3::decode_identify_resp(payload.data, payload.len, resp) != l3::Status::Ok) {
        return;
    }
    // spec FR-006: module type, descriptor length and descriptor CRC learned here.
    node.module_type = resp.module_type;
    node.desc_len = resp.desc_len;
    node.desc_crc = resp.desc_crc;

    // spec FR-008 (T021): the descriptor cache, keyed by what an IdentifyResp can actually
    // form — see core/core_types.hpp's note on NodeRecord::module_type for why that is not the
    // MODEL_ID triple FR-008's wording names.
    DescriptorCacheEntry* entry = find_descriptor(resp.module_type, resp.desc_len, resp.desc_crc);
    if (entry != nullptr && entry->complete) {
        mark_discovered(node, tx_node_, *entry); // SC-004: no descriptor read at all
        return;
    }
    node.discovery = DiscoveryState::ReadingDescriptor;
    // A fresh read, whatever the last one did: a node that comes back through IDENTIFY starts
    // un-stalled, so one refused chunk does not deprioritise it for the rest of its life.
    node.desc_stalled_superframe = 0;
    if (entry != nullptr) {
        // Another node is already reading this exact descriptor. This one waits for that read
        // instead of issuing a second copy of it: attach_descriptor() picks up every waiter
        // when the entry completes.
        return;
    }
    entry = claim_descriptor(resp.module_type, resp.desc_len, resp.desc_crc);
    if (entry == nullptr) {
        // R-12's graceful degradation, in the only shape core/ can offer it: a cache entry IS
        // the reassembly buffer (core/core_types.hpp), so with none free there is nowhere to
        // put the bytes. The node drops back to Identifying and is retried; it is never
        // reported Discovered on a descriptor that was never read. A DIVERGENCE from
        // data-model.md §5's "read fresh rather than cached", which assumes a staging area this
        // engine deliberately does not carry (128 x LIMIT_max_descriptor_bytes) — recorded in
        // docs/OPEN-QUESTIONS.md (2026-09-28).
        node.discovery = DiscoveryState::Identifying;
        return;
    }
    if (resp.desc_len == 0) {
        // A module with an empty descriptor: complete on arrival, nothing to read (protocol-l3
        // §4 does not require any record, and l3::descriptor_crc over zero bytes is defined).
        entry->complete = true;
        attach_descriptor(*entry);
        return;
    }
    DescChunkItem first{};
    first.node_id = tx_node_;
    first.offset = 0;
    if (!desc_queue_.push(first)) {
        // R-07: the ring refused. Nothing is partially queued; the node returns to Identifying
        // and the whole read is re-decided next time, rather than leaving a claimed entry with
        // no reader.
        // mutant-ok(equivalent, cxx_assign_const): 42 stored in a bool reads false (assumed).
        entry->in_use = false;
        node.discovery = DiscoveryState::Identifying;
    }
}

void CoreEngine::on_desc_chunk(const link::MasterEvent& ev, uint64_t now_us) {
    (void)now_us;
    const size_t idx = node_index(tx_node_);
    if (idx == kNoNodeIndex || !nodes_[idx].in_use) {
        return;
    }
    NodeRecord& node = nodes_[idx];
    DescriptorCacheEntry* entry = find_descriptor(node.module_type, node.desc_len, node.desc_crc);
    if (entry == nullptr || entry->complete) {
        return; // the entry was completed or reclaimed while this chunk was in flight
    }
    l3::Header hdr{};
    l3::Bytes payload{};
    if (l3::decode_message(ev.response.payload, ev.response.len, hdr, payload) != l3::Status::Ok ||
        hdr.opcode != OP_READ_DESC) {
        // An ERROR answer (trunk §8) or an undecodable one: re-ask for the SAME offset. The
        // request is idempotent (CLAUDE.md rule 2) and the entry keeps what it already holds.
        // No byte was added, so this node has spent its one chunk attempt for this superframe —
        // trunk §8 MANDATES ERR_BUSY for a bridge whose module bus is not ready (CLAUDE.md rule
        // 6) and trunk §10.5 leaves a persistent busy unbounded at L2, so an endlessly refused
        // read is a spec-conformant rig, not a hostile one, and it must not be able to hold the
        // chunk ring against every other node (NodeRecord::desc_stalled_superframe).
        node.desc_stalled_superframe = superframe_;
        DescChunkItem again{};
        again.node_id = tx_node_;
        again.offset = tx_offset_;
        (void)desc_queue_.push(again);
        return;
    }
    l3::ReadDescResp resp{};
    if (l3::decode_read_desc_resp(payload.data, payload.len, resp) != l3::Status::Ok) {
        // Nothing is re-queued: retry_stalled_desc() picks this node up from what its entry holds
        // (the recovery sweep skips a stalled node — it is that pass's business). It is still a
        // superframe's attempt spent, or the retry — which resumes from the entry either way —
        // would re-issue for this same node for the rest of the superframe and starve the others.
        node.desc_stalled_superframe = superframe_;
        return;
    }
    // protocol-l3 §3.1: the response carries its own offset. A chunk that does not continue the
    // reassembly exactly where it left off is DROPPED rather than written somewhere else in the
    // blob — a node answering an offset it was not asked for must not be able to scatter bytes
    // through another module's cached descriptor (CLAUDE.md rule 7).
    if (resp.offset != entry->received) {
        node.desc_stalled_superframe = superframe_; // no progress: one attempt per superframe
        DescChunkItem again{};
        again.node_id = tx_node_;
        again.offset = entry->received;
        (void)desc_queue_.push(again);
        return;
    }
    const uint16_t room = static_cast<uint16_t>(entry->len - entry->received);
    // protocol-l3 §3.1 / §4.1: a chunk carrying MORE bytes than the desc_len this node's own
    // IDENTIFY advertised still has room for is a node contradicting itself — every request this
    // engine sends asks for kDescChunkMax, so the last chunk of a descriptor whose length is not
    // a multiple of it is where an over-serving node shows up (with desc_len at
    // LIMIT_max_descriptor_bytes the surplus would be written past the blob entirely). The read
    // is ABANDONED and the node retried from IDENTIFY — where a changed desc_len is re-learned —
    // rather than the surplus being silently truncated and the node credited with a descriptor it
    // did not serve (CLAUDE.md rule 7: never over-write on arbitrary bytes). Same disposition as
    // the zero-length and CRC-mismatch answers below, for the same reason.
    if (resp.bytes.len > room) {
        // mutant-ok(equivalent, cxx_assign_const): 42 stored in a bool reads false (assumed).
        entry->in_use = false;
        node.discovery = DiscoveryState::Identifying;
        return;
    }
    // take <= room holds by the guard above; the min() is a redundant second bound kept so the
    // write stays inside blob[LIMIT_max_descriptor_bytes] by construction, not only by the guard.
    // At resp.bytes.len == room both operands are equal, so `<` and `<=` select the same value.
    // mutant-ok(equivalent, cxx_lt_to_le): at len == room both arms of the ?: are the same value.
    const uint16_t take = resp.bytes.len < room ? resp.bytes.len : room;
    for (uint16_t i = 0; i < take; ++i) {
        entry->blob[entry->received + i] = resp.bytes.data[i];
    }
    entry->received = static_cast<uint16_t>(entry->received + take);
    if (take != 0) {
        // Progress: this node is no longer stalled, so its next chunk goes back in the chunk
        // ring's own pass rather than retry_stalled_desc()'s rationed one. Cleared on the
        // FIRST good chunk, not on completion — a node recovering from one refused chunk should
        // not finish its descriptor at one chunk per superframe.
        node.desc_stalled_superframe = 0;
    }
    if (entry->received < entry->len) {
        if (take == 0) {
            // A node that answers a zero-length chunk short of its own advertised desc_len
            // would otherwise re-ask for the same offset for ever. The read is abandoned and
            // the node retried from IDENTIFY, which is where a changed descriptor length would
            // be learned anyway.
            // mutant-ok(equivalent, cxx_assign_const): 42 stored in a bool reads false (assumed).
            entry->in_use = false;
            node.discovery = DiscoveryState::Identifying;
            return;
        }
        DescChunkItem next{};
        next.node_id = tx_node_;
        next.offset = entry->received;
        if (!desc_queue_.push(next)) {
            // mutant-ok(equivalent, cxx_assign_const): 42 stored in a bool reads false (assumed).
            entry->in_use = false;
            node.discovery = DiscoveryState::Identifying;
        }
        return;
    }
    // protocol-l3 §4.1: IDENTIFY's desc_crc is l3::descriptor_crc over the whole blob exactly
    // as READ_DESC served it. A mismatch means the bytes are not the descriptor that was
    // advertised — the entry is discarded rather than cached and handed to a node.
    if (l3::descriptor_crc(entry->blob, entry->len) != entry->desc_crc) {
        // mutant-ok(equivalent, cxx_assign_const): 42 stored in a bool reads false (assumed).
        entry->in_use = false;
        node.discovery = DiscoveryState::Identifying;
        return;
    }
    // data-model.md §5: the cache key's MODEL_ID triple is only knowable now, from the blob's
    // own MODEL_ID record (protocol-l3 §4.1). A descriptor without one keeps the zeroes: §4's
    // unknown-record rule is skip-by-length, and a missing record is not a parse failure here.
    l3::RecordCursor cursor(entry->blob, entry->len);
    l3::RecordView view{};
    while (!cursor.at_end() && cursor.next(view) == l3::Status::Ok) {
        if (view.type != TLV_MODEL_ID) {
            continue;
        }
        l3::ModelIdRec model{};
        if (l3::decode_model_id(view, model) == l3::Status::Ok) {
            entry->model_vendor = model.vendor_model;
            entry->model_hw_rev = model.hw_rev;
            entry->model_fw_rev = model.fw_rev;
        }
        break;
    }
    entry->complete = true;
    attach_descriptor(*entry);
}

DescriptorCacheEntry* CoreEngine::find_descriptor(uint8_t module_type, uint16_t desc_len,
                                                  uint16_t crc) {
    for (DescriptorCacheEntry& entry : descriptors_) {
        if (entry.in_use && entry.module_type == module_type && entry.len == desc_len &&
            entry.desc_crc == crc) {
            return &entry;
        }
    }
    return nullptr;
}

DescriptorCacheEntry* CoreEngine::claim_descriptor(uint8_t module_type, uint16_t desc_len,
                                                   uint16_t crc) {
    if (desc_len > LIMIT_max_descriptor_bytes) {
        return nullptr; // protocol-l3 §4's own cap; blob[] is exactly that long
    }
    // data-model.md §5: an entry is reusable only with no node still pointing at it. TWO passes,
    // because "free" and "reusable" are not the same thing and the never-used entry must win: with
    // one first-fit pass a fresh claim recycles descriptors_[0] while 31 entries are untouched, so
    // a cached descriptor is thrown away for nothing — and, before the second pass's own predicate
    // below, so was another node's half-read one.
    for (DescriptorCacheEntry& entry : descriptors_) {
        if (!entry.in_use) {
            return key_descriptor(entry, module_type, desc_len, crc);
        }
    }
    for (DescriptorCacheEntry& entry : descriptors_) {
        // "No node still pointing at it" is NOT refcount == 0 alone. The refcount counts the nodes
        // whose NodeRecord::descriptor is set, which mark_discovered() does and only a COMPLETE
        // entry reaches — so an entry still being FILLED by a chunked READ_DESC (in_use, !complete,
        // refcount 0) has a node pointing at it BY KEY, through find_descriptor(), and nothing in
        // the refcount says so. Reclaiming it would discard that node's reassembly buffer, which IS
        // the entry (core/core_types.hpp): resume_desc_read() would then find no entry for the
        // node's key and send it back to IDENTIFY to read the whole descriptor again, contradicting
        // "resumed at the bytes its cache entry already holds". Reachable on two spec-conformant
        // backplanes: one trunk §8 ERR_BUSY leaves a node stalled with its ring item dropped (see
        // issue_demand()), so the ring is empty, scan_identify() runs, and the next module with a
        // DIFFERENT descriptor key claims. So a live reader is looked for by key, the same way the
        // reader itself finds the entry.
        if (entry.refcount != 0 || descriptor_has_reader(entry)) {
            continue;
        }
        return key_descriptor(entry, module_type, desc_len, crc);
    }
    return nullptr;
}

bool CoreEngine::descriptor_has_reader(const DescriptorCacheEntry& entry) const {
    for (const NodeRecord& node : nodes_) {
        if (!node.in_use || node.discovery != DiscoveryState::ReadingDescriptor) {
            continue;
        }
        if (node.module_type == entry.module_type && node.desc_len == entry.len &&
            node.desc_crc == entry.desc_crc) {
            return true; // find_descriptor() would hand this node exactly this entry
        }
    }
    return false;
}

DescriptorCacheEntry* CoreEngine::key_descriptor(DescriptorCacheEntry& entry, uint8_t module_type,
                                                 uint16_t desc_len, uint16_t crc) {
    entry = DescriptorCacheEntry{};
    entry.in_use = true;
    entry.module_type = module_type;
    entry.desc_crc = crc;
    entry.len = desc_len;
    return &entry;
}

void CoreEngine::attach_descriptor(DescriptorCacheEntry& entry) {
    for (uint8_t id = ADDR_module_min; id <= ADDR_module_max; ++id) {
        NodeRecord& node = nodes_[node_index(id)];
        if (!node.in_use || node.discovery != DiscoveryState::ReadingDescriptor) {
            continue;
        }
        if (node.module_type != entry.module_type || node.desc_len != entry.len ||
            node.desc_crc != entry.desc_crc) {
            continue;
        }
        mark_discovered(node, id, entry);
    }
}

void CoreEngine::mark_discovered(NodeRecord& node, uint8_t node_id,
                                 const DescriptorCacheEntry& entry) {
    // data-model.md §5: the refcount is incremented wherever a NodeRecord::descriptor is SET to
    // this entry — a cache hit and a fresh read's completion alike.
    node.descriptor = &entry;
    node.model_vendor = entry.model_vendor;
    node.model_hw_rev = entry.model_hw_rev;
    node.model_fw_rev = entry.model_fw_rev;
    ++const_cast<DescriptorCacheEntry&>(entry).refcount;
    node.discovery = DiscoveryState::Discovered;
    // data-model.md §7 / spec FR-017: presence is reported on reaching Discovered ("fully
    // identified and described"), never on the slot merely being occupied — and this and
    // reconcile_slot_map()'s NodeRemoved are the only two emission points for it.
    LifecycleEvent lifecycle{};
    lifecycle.kind =
        node.ever_discovered ? LifecycleKind::NodeRediscovered : LifecycleKind::NodeDiscovered;
    lifecycle.node_id = node_id;
    lifecycle.slot = node.slot;
    node.ever_discovered = true;
    (void)enqueue_lifecycle(lifecycle);
}

void CoreEngine::release_descriptor(NodeRecord& node) {
    if (node.descriptor == nullptr) {
        return;
    }
    DescriptorCacheEntry& entry = const_cast<DescriptorCacheEntry&>(*node.descriptor);
    if (entry.refcount != 0) {
        --entry.refcount;
    }
    node.descriptor = nullptr;
}

void CoreEngine::reconcile_slot_map(uint8_t backplane_addr, const l3::BpSlotMapResp& resp,
                                    uint64_t now_us) {
    (void)now_us;
    const size_t bp_idx = backplane_index(backplane_addr);
    if (bp_idx == kNoNodeIndex) {
        return; // trunk §5: not a backplane address, so it names no record
    }
    BackplaneRecord& bp = backplanes_[bp_idx];
    // CLAUDE.md rule 7 / data-model.md §4: node_id_by_slot is sized to
    // LIMIT_bp_slot_map_max_slots, never to a slot_count a peer chose, so a backplane cannot
    // overrun it by over-claiming. Everything past the table is ignored by bound.
    uint8_t slot_count = resp.slot_count;
    // At slot_count == LIMIT_bp_slot_map_max_slots the clamp stores the value it already has.
    // mutant-ok(equivalent, cxx_gt_to_ge): at slot_count == the limit the clamp is a no-op.
    if (static_cast<uint32_t>(slot_count) > LIMIT_bp_slot_map_max_slots) {
        slot_count = static_cast<uint8_t>(LIMIT_bp_slot_map_max_slots);
    }
    bp.slot_count = slot_count;
    for (uint8_t slot = 0; slot < slot_count; ++slot) {
        // Ruled 2026-09-22 (docs/OPEN-QUESTIONS.md): `occupied` is absolute state and always
        // re-converges; `changed` is a drain-on-send delta a lost response can silently drop.
        // So the diff is against this engine's own record, and `changed` triggers nothing.
        const bool occupied = slot_bit(resp.occupied, slot);
        const uint8_t assigned = bp.node_id_by_slot[slot];
        if (!occupied) {
            // No slot that reports empty has a standing id shortage, whether or not it ever held
            // an id: a later re-occupation is a NEW transition, and is reported again.
            set_exhaustion_reported(bp, slot, false);
        }
        if (occupied && assigned == 0) {
            // R-06: first fit from the one shared pool, in this response's own ascending slot
            // order — the canonical order that makes a scripted rig reproducible (FR-019).
            uint8_t chosen = 0;
            for (uint8_t id = ADDR_module_min; id <= ADDR_module_max; ++id) {
                if (!nodes_[node_index(id)].in_use) {
                    chosen = id;
                    break;
                }
            }
            if (chosen == 0) {
                // Never a wraparound onto a live id (R-06). The slot keeps no assignment and is
                // reconsidered on the next BP_SLOT_MAP, when an id may have been freed.
                //
                // Reported EDGE-triggered — once per transition into the shortage for this slot,
                // not once per sweep that re-observes it (BackplaneRecord::exhaustion_reported).
                // A shortage is a STANDING condition: every later BP_SLOT_MAP re-reports the
                // same occupancy, so a level-triggered notice would enqueue one notice per
                // occupied-but-unassignable slot per sweep, without bound, and §8a's drop-newest
                // ring would then refuse the genuine presence events behind the repeats. The
                // slot is still reconsidered every sweep; only the NOTICE is deduplicated.
                //
                // The bit records "the application has been TOLD", so it is set only when the
                // enqueue succeeded. §8a's ring is drop-newest and holds LIMIT_max_nodes items,
                // so a rig with more unassignable slots than that has notices REFUSED on a sweep;
                // setting the bit regardless would make those slots silent for ever, even while
                // the shortage stands and the application drains every superframe. A refused
                // notice is therefore re-offered by the next sweep that still observes the
                // shortage, and the sweeps go quiet once each slot has actually been delivered.
                if (!exhaustion_reported(bp, slot)) {
                    LifecycleEvent exhausted{};
                    exhausted.kind = LifecycleKind::NodeIdPoolExhausted;
                    exhausted.node_id = backplane_addr;
                    exhausted.slot = slot;
                    set_exhaustion_reported(bp, slot, enqueue_lifecycle(exhausted));
                }
                continue;
            }
            // The shortage, if this slot was ever in one, is over: it has an id now, so a future
            // one is a new transition to report.
            set_exhaustion_reported(bp, slot, false);
            NodeRecord& node = nodes_[node_index(chosen)];
            const bool ever = node.ever_discovered;
            node = NodeRecord{};
            node.ever_discovered = ever; // a question about the ID's history, not this slot's
            node.in_use = true;
            node.backplane_addr = backplane_addr;
            node.slot = slot;
            node.discovery = DiscoveryState::Undiscovered;
            bp.node_id_by_slot[slot] = chosen;
        } else if (!occupied && assigned != 0) {
            // spec FR-010: the id goes back to the pool the moment the slot reports empty.
            const size_t idx = node_index(assigned);
            if (idx != kNoNodeIndex) {
                NodeRecord& node = nodes_[idx];
                release_descriptor(node);
                const bool ever = node.ever_discovered;
                node = NodeRecord{};
                node.ever_discovered = ever;
            }
            bp.node_id_by_slot[slot] = 0;
            // data-model.md §7 / contracts/core-cpp.md: ALWAYS reported, whether or not that
            // node had reached Discovered.
            LifecycleEvent removed{};
            removed.kind = LifecycleKind::NodeRemoved;
            removed.node_id = assigned;
            removed.slot = slot;
            (void)enqueue_lifecycle(removed);
        }
    }
    // A backplane that shrinks its slot_count: every slot past the new count is gone, and its
    // id must not be stranded in_use for ever. Same release path as an emptied slot.
    for (size_t slot = slot_count; slot < LIMIT_bp_slot_map_max_slots; ++slot) {
        // A slot outside the backplane's own slot_count is not occupied, so it carries no
        // standing shortage either (same rule as an emptied slot above).
        set_exhaustion_reported(bp, static_cast<uint8_t>(slot), false);
        const uint8_t assigned = bp.node_id_by_slot[slot];
        if (assigned == 0) {
            continue;
        }
        const size_t idx = node_index(assigned);
        if (idx != kNoNodeIndex) {
            NodeRecord& node = nodes_[idx];
            release_descriptor(node);
            const bool ever = node.ever_discovered;
            node = NodeRecord{};
            node.ever_discovered = ever;
        }
        bp.node_id_by_slot[slot] = 0;
        LifecycleEvent removed{};
        removed.kind = LifecycleKind::NodeRemoved;
        removed.node_id = assigned;
        removed.slot = static_cast<uint8_t>(slot);
        (void)enqueue_lifecycle(removed);
    }
}

bool CoreEngine::enqueue_lifecycle(const LifecycleEvent& ev) {
    // data-model.md §8a: drop-NEWEST and count. Ring<T, N>::push() already refuses rather than
    // evicting (core/core_types.hpp); the counting is this engine's, per §8a.
    if (!pending_lifecycle_.push(ev)) {
        ++dropped_deliveries_;
        return false;
    }
    return true;
}

// --- parameters (spec FR-023/FR-024, R-10) -------------------------------------------------------

bool CoreEngine::param_target_ready(uint8_t node_id) const {
    // protocol-l3 §3.1 addresses SET_PARAM/GET_PARAM to a module, and a module's parameter set
    // is what its descriptor declares — so an operation is only admissible once the descriptor
    // is known. TOTAL for any uint8_t (CLAUDE.md rule 7): node_index() returns kNoNodeIndex for
    // every id outside the module window, so a backplane address or the broadcast address is
    // refused by this bound rather than indexed into the table.
    const size_t idx = node_index(node_id);
    if (idx == kNoNodeIndex) {
        return false;
    }
    return nodes_[idx].in_use && nodes_[idx].discovery == DiscoveryState::Discovered;
}

bool CoreEngine::claim_request_id(ParamRequestId& out_id) {
    // First free, not a rotating counter: R-10's ids are reused once delivered, so a counter
    // would need to test occupancy anyway, and a scan cannot hand out an id already outstanding.
    // LIMIT_max_nodes passes, so this is bounded by the pool, which is bounded by the ring.
    for (size_t i = 0; i < LIMIT_max_nodes; ++i) {
        if (!request_id_used_[i]) {
            request_id_used_[i] = true;
            out_id = static_cast<ParamRequestId>(i);
            return true;
        }
    }
    return false;
}

void CoreEngine::release_request_id(ParamRequestId id) {
    // Guarded rather than trusted: the only caller is drain_callbacks() with an id that came
    // out of pending_param_results_, but the ring's contents are engine state a seam can write,
    // and an out-of-range index here would be a silent write past the pool.
    //
    // claim_request_id() is the only source of an id and returns only i < LIMIT_max_nodes, so
    // id == LIMIT_max_nodes is not reachable through this engine's own API: over every id the
    // pool can hold, `<` and `<=` admit the same set. A CONTROL on that allocation, not a
    // guarantee (CLAUDE.md rule 11) — which is what this guard exists for.
    // mutant-ok(equivalent, cxx_lt_to_le): id == LIMIT_max_nodes is unreachable; see above.
    if (static_cast<size_t>(id) < LIMIT_max_nodes) {
        // Mull's cxx_assign_const stores 42, and a bool reads its low bit, which is 0 — the
        // value this store already writes. ASSUMED from Mull's IR semantics, not measured.
        // mutant-ok(equivalent, cxx_assign_const): 42 stored in a bool reads false (assumed).
        request_id_used_[id] = false;
    }
}

CoreStatus CoreEngine::set_param(uint8_t node_id, uint8_t param_id, uint8_t scope, uint16_t value) {
    if (!param_target_ready(node_id)) {
        return CoreStatus::NotDiscovered;
    }
    // protocol/omgp-protocol.yaml gives SET_PARAM's value `max: 4095` (LIMIT_param_value_max);
    // l3::encode_set_param refuses above it with Status::OutOfRange. Checked HERE so the refusal
    // reaches the caller as a return value — see CoreStatus::InvalidValue in core_types.hpp.
    if (value > LIMIT_param_value_max) {
        return CoreStatus::InvalidValue;
    }
    ParamOpItem item{};
    item.node_id = node_id;
    item.kind = ParamOpItem::Kind::Set;
    item.param_id = param_id;
    item.scope = scope;
    item.value = value;
    // A Set carries no request id (data-model.md §6): it is fire-and-forget except for its own
    // failure report (spec FR-023), which is a LifecycleEvent, not a correlated result.
    if (!param_queue_.push(item)) {
        return CoreStatus::QueueFull;
    }
    // Idempotent by construction (CLAUDE.md rule 2): the queued item holds the ABSOLUTE value
    // the caller passed, and no path here reads the current value to compute it, so two
    // identical calls queue two items that encode identically.
    return CoreStatus::Ok;
}

CoreStatus CoreEngine::get_param(uint8_t node_id, uint8_t param_id, uint8_t scope,
                                 ParamRequestId& out_id) {
    if (!param_target_ready(node_id)) {
        return CoreStatus::NotDiscovered;
    }
    // Ring first, pool second, and the ORDER matters: claiming an id for an operation the ring
    // then refuses would leak it, since an id is released only when its result is delivered and
    // a refused operation produces no result. Nothing is claimed until the push can be made.
    if (param_queue_.full()) {
        return CoreStatus::QueueFull;
    }
    // claim_request_id() writes this on Ok, and the refusal below returns without reading it,
    // so its initial value is never observed. The label is the LAST comment line before the
    // statement it covers: a label above further prose covers that prose, not the statement.
    // mutant-ok(equivalent, cxx_init_const): overwritten by claim_request_id() before any read.
    ParamRequestId id = 0;
    if (!claim_request_id(id)) {
        return CoreStatus::RequestIdReused;
    }
    ParamOpItem item{};
    item.node_id = node_id;
    item.kind = ParamOpItem::Kind::Get;
    item.param_id = param_id;
    item.scope = scope;
    item.request_id = id;
    if (!param_queue_.push(item)) {
        // Unreachable while full() is the ring's own predicate and nothing else can push
        // between the two: defensive, not a known path. The id is handed straight back, so even
        // if it were reachable the pool would not leak.
        // mutant-ok(equivalent, cxx_remove_void_call): this branch is unreachable; see above.
        release_request_id(id);
        return CoreStatus::QueueFull;
    }
    out_id = id; // written only on Ok, so a refusal leaves the caller's variable as it was
    return CoreStatus::Ok;
}

bool CoreEngine::enqueue_param_result(ParamRequestId id, const ParamResult& result) {
    ParamResultDelivery delivery{};
    delivery.id = id;
    delivery.result = result;
    if (!pending_param_results_.push(delivery)) {
        ++dropped_deliveries_; // §8a: one counter for both rings
        return false;
    }
    return true;
}

size_t CoreEngine::node_index(uint8_t node_id) {
    // data-model.md §3 / R-05: the table is indexed by `node_id - ADDR_module_min`, and
    // core/core_types.hpp's static_asserts already pin that the whole module address range fits
    // inside LIMIT_max_nodes entries. Any other id — the host's own address, a backplane
    // address, 0xFF — names no entry.
    if (node_id < ADDR_module_min || node_id > ADDR_module_max) {
        return kNoNodeIndex;
    }
    return static_cast<size_t>(node_id) - static_cast<size_t>(ADDR_module_min);
}

size_t CoreEngine::backplane_index(uint8_t addr) {
    // data-model.md §4 / trunk §5: 0x01..0x0F are the backplane addresses; the host's own
    // address and every module id name no BackplaneRecord.
    if (addr < ADDR_backplane_min || addr > ADDR_backplane_max) {
        return kNoNodeIndex;
    }
    return static_cast<size_t>(addr) - static_cast<size_t>(ADDR_backplane_min);
}

DiscoveryState CoreEngine::discovery_state(uint8_t node_id) const {
    const size_t idx = node_index(node_id);
    if (idx == kNoNodeIndex) {
        return DiscoveryState::Undiscovered;
    }
    return nodes_[idx].discovery;
}

bool CoreEngine::node_in_use(uint8_t node_id) const {
    const size_t idx = node_index(node_id);
    if (idx == kNoNodeIndex) {
        return false;
    }
    return nodes_[idx].in_use;
}

bool CoreEngine::backplane_enrolled(uint8_t addr) const {
    const size_t idx = backplane_index(addr);
    if (idx == kNoNodeIndex) {
        return false;
    }
    return backplanes_[idx].enrolled;
}

uint32_t CoreEngine::dropped_deliveries() const {
    return dropped_deliveries_;
}

void CoreEngine::on_notice(link::Notice notice, uint8_t addr) {
    // R-03 / trunk §6: the enrolment half only. data-model.md §4's BackplaneRecord::enrolled is
    // "link::Notice::ENROLLED seen for this address", which is what gates this backplane's own
    // status poll (spec FR-002) — so it is set here, at the one place the tracker announces it,
    // rather than inferred from a transaction outcome this engine would have to re-classify.
    // Forwarding the SUSPECT/OFFLINE/RECOVERED and bus notices as LifecycleEvents, and degrading
    // every node behind `addr`, is still T042 (#713).
    //
    // link/health.hpp forbids re-entering the tracker from here, and nothing below does: this
    // writes one bool in this engine's own table. In particular it does NOT call back — §8a's
    // rings are the only delivery path and only drain_callbacks() reads them (spec FR-021).
    const size_t idx = backplane_index(addr);
    if (idx == kNoNodeIndex) {
        return;
    }
    if (notice == link::Notice::ENROLLED) {
        backplanes_[idx].enrolled = true;
    }
}

} // namespace core
} // namespace omgp
