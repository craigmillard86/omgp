// OMGP ESP32-S3 host firmware — trunk L2 LINK-TIME smoke program (spec 002 T047).
//
// Why a whole program rather than one #include. `omgp_link` is a static library, and a
// static-library member that no referenced symbol pulls in is silently omitted from the
// link — so the T003 stub this replaces (a single reference to the header-only
// link/crc16.hpp) left `./pipeline.sh esp32` green while linking NONE of frame.cpp,
// master.cpp, responder.cpp or health.cpp. Measured 2026-09-24: with that stub,
// esp32-host/build/omgp-host.map mentioned `omgp_link` exactly twice, both
// `LOAD esp-idf/omgp_link/libomgp_link.a`, with no member of it pulled in. Every
// link-only failure on Xtensa — an unresolved symbol, an -fno-exceptions/-fno-rtti ABI
// mismatch, a missing intrinsic — was therefore invisible to CLAUDE.md rule 10's target
// build. This file closes that by referencing one entry point from each of those four
// translation units and driving it.
//
// It is not a test: nothing on the target runs it under CI. What it establishes is
// link-time only — that each engine's code resolves and fits — and it establishes that by
// construction of the archive rules, demonstrated by the map/link output the PR cites.
//
// Everything here is embedded-path safe (CLAUDE.md rule 5): no heap, no exceptions, no
// RTTI. The engines live at namespace scope rather than on app_main's stack (~2 KB of
// fixed tables between them, against a 3584-byte main task stack) — .bss, still no dynamic
// allocation. Time is the injected Clock and nothing else (CLAUDE.md rule 3): no wall
// clock, no sleep. No UART is opened and no device is touched — a real RS-485 wire is out
// of scope for this feature (specs/002-trunk-link-layer/spec.md Assumptions).
//
// trunk §3 (media access, both engines), §4 (framing), §6/§7 (node health, bus fault),
// §9 (bit rates and the byte-time model). Contracts: byte-wire-and-clock.md, link-cpp.md,
// tooling.md "CMake". Source shape pinned by tools/refimpl/test_esp32_link_smoke.py.
#include "link/byte_wire.hpp"
#include "link/clock.hpp"
#include "link/crc16.hpp"
#include "link/frame.hpp"
#include "link/health.hpp"
#include "link/link_types.hpp"
#include "link/master.hpp"
#include "link/responder.hpp"

#include <cstddef>
#include <cstdint>

namespace {

// The one backplane node this smoke addresses (trunk §5).
constexpr uint8_t kNodeAddr = omgp::ADDR_backplane_min;
// Bound on every drive loop below. A Master transaction concludes within three attempts of
// T_resp + T_gap (trunk §7), i.e. well inside twelve T_poll steps; the bound is what makes
// these loops terminate by construction rather than on the engines' behaviour.
constexpr uint8_t kSteps = 12;

// The injected monotonic clock (CLAUDE.md rule 3) — a counter this file advances
// explicitly. contracts/byte-wire-and-clock.md.
class SmokeClock final : public omgp::Clock {
  public:
    uint64_t now_us() override {
        return now_;
    }
    void advance(uint32_t us) {
        now_ += us;
    }

  private:
    uint64_t now_ = 0;
};

// An in-memory ByteWire (contracts/byte-wire-and-clock.md). Transmitted bytes are counted
// and dropped; received bytes are whatever preload() put there, handed out with start-bit
// instants derived from the wire's own byte cadence (trunk §9) exactly as the contract
// requires the engines to be able to measure T_resp/T_turn/T_gap against.
//
// The contract's "return only bytes whose start instant is <= the draining call's own
// reference instant" is satisfied here the simple way, not by a check: preload() stamps the
// frame at instant 0 and omgp_link_smoke() advances the clock past a worst-case frame
// before any poll, so every preloaded byte is already due at every reference instant this
// file ever polls with. That is a property of THIS caller, not of the class.
class SmokeWire final : public omgp::link::ByteWire {
  public:
    uint64_t transmit(const uint8_t* bytes, size_t n, uint64_t now_us) override {
        (void)bytes;
        sent_ = static_cast<uint16_t>(sent_ + n);
        return now_us + static_cast<uint64_t>(n) * omgp::link::byte_time_us(rate_);
    }

    bool receive(uint8_t& byte, uint64_t& start_us) override {
        if (head_ >= len_) {
            return false;
        }
        start_us = base_us_ + static_cast<uint64_t>(head_) * omgp::link::byte_time_us(rate_);
        byte = buf_[head_++];
        return true;
    }

    uint32_t bit_rate() const override {
        return rate_;
    }

    void set_bit_rate(uint32_t bps) override {
        rate_ = bps;
    }

    // Stage `n` bytes as if they had arrived starting at `base_us`. Truncated at kMaxWire,
    // the codec's own sizing bound (trunk §4) — nothing legitimate is longer.
    void preload(const uint8_t* bytes, size_t n, uint64_t base_us) {
        len_ = n < omgp::link::kMaxWire ? n : omgp::link::kMaxWire;
        for (size_t i = 0; i < len_; ++i) {
            buf_[i] = bytes[i];
        }
        head_ = 0;
        base_us_ = base_us;
    }

    uint16_t sent() const {
        return sent_;
    }

  private:
    uint32_t rate_ = omgp::TRUNK_bit_rate;
    uint64_t base_us_ = 0;
    uint8_t buf_[omgp::link::kMaxWire] = {};
    size_t len_ = 0;
    size_t head_ = 0;
    uint16_t sent_ = 0;
};

// The application side of a node (trunk §3): answers every request with its own address.
// The real firmware's seat for the module-bus bridge.
class SmokeHandler final : public omgp::link::RequestHandler {
  public:
    size_t handle(const uint8_t* req, size_t len, uint8_t* resp, size_t cap) override {
        (void)req;
        (void)len;
        if (cap == 0) {
            return 0;
        }
        resp[0] = kNodeAddr;
        return 1;
    }
};

// trunk §6/§7 transition notices. F3's scheduler takes this seat in the real firmware; here
// it only tallies, and must not re-enter the tracker (link/health.hpp).
class SmokeListener final : public omgp::link::HealthListener {
  public:
    void on_notice(omgp::link::Notice notice, uint8_t addr) override {
        tally_ = static_cast<uint16_t>(tally_ + static_cast<uint8_t>(notice) + addr);
    }

    uint16_t tally() const {
        return tally_;
    }

  private:
    uint16_t tally_ = 0;
};

// Static storage, constructed in declaration order within this one translation unit (so
// each engine's wire/clock/handler exists before the engine referencing it). Destructors
// are trivial throughout, so none of this registers an exit handler.
SmokeClock g_clock;
SmokeWire g_master_wire;
SmokeWire g_node_wire;
SmokeHandler g_handler;
SmokeListener g_listener;

omgp::link::Deframer g_deframer;
omgp::link::Master g_master(g_master_wire, g_clock);
omgp::link::Responder g_responder(g_node_wire, g_clock, g_handler, kNodeAddr);
omgp::link::HealthTracker g_health(g_clock, g_listener);

uint8_t g_wire_bytes[omgp::link::kMaxWire];

} // namespace

// Links every translation unit of the omgp_link component into the firmware image and
// drives one entry point of each, so the Xtensa link must resolve them (CLAUDE.md rule 10).
// Called once from app_main() (main.c): WITHOUT that call this object file is itself
// dropped from libmain.a — an ordinary archive too — and none of the below is linked at
// all, which is the state the T003 stub was in. Returns an accumulator over the engines'
// observable outputs so no call can be elided.
extern "C" uint16_t omgp_link_smoke(void) {
    const uint8_t payload[] = {0x00, 0x01, 0x02, 0x03};
    uint16_t acc = omgp::crc16_ccitt_false(payload, sizeof(payload));

    // trunk §4 — frame codec (link/frame.cpp): encode a request, then deframe it back.
    omgp::link::FrameFields f{};
    f.dst = kNodeAddr;
    f.src = omgp::ADDR_host;
    f.response = false;
    f.retry = false;
    f.seq = 0;
    f.len = static_cast<uint8_t>(sizeof(payload));
    f.payload = payload;

    size_t written = 0;
    if (omgp::link::encode_frame(f, g_wire_bytes, sizeof(g_wire_bytes), written) !=
        omgp::link::Status::Ok) {
        return acc;
    }
    omgp::link::FrameView view{};
    for (size_t i = 0; i < written; ++i) {
        if (g_deframer.feed(g_wire_bytes[i], view)) {
            acc = static_cast<uint16_t>(acc + view.f.len);
        }
    }
    acc = static_cast<uint16_t>(acc + g_deframer.stats().delivered);

    // trunk §3/§7 — Master engine (link/master.cpp): one transaction over a wire that never
    // answers, driven to its terminal event (the T_resp timeout and retry path).
    if (g_master.begin(kNodeAddr, payload, sizeof(payload)) == omgp::link::Status::Ok) {
        for (uint8_t i = 0; i < kSteps && g_master.busy(); ++i) {
            g_clock.advance(omgp::TRUNK_T_poll_us);
            const omgp::link::MasterEvent ev = g_master.poll(g_clock.now_us());
            acc = static_cast<uint16_t>(acc + static_cast<uint8_t>(ev.kind));
        }
    }
    g_master.set_bit_rate(omgp::TRUNK_bit_rate);
    acc = static_cast<uint16_t>(acc + g_master.attempts() + g_master.stats(kNodeAddr).timeouts +
                                g_master.bus_stats().rate_changes);

    // trunk §3 — Responder engine (link/responder.cpp): the request encoded above, staged on
    // this node's wire, is decoded, answered by the handler and transmitted on FR-014's LATE
    // path — not inside the turnaround window. Staged at instant 0 with the clock already well
    // past one worst-case frame, so every byte is due on the first poll (see SmokeWire); but
    // the master block above has already advanced the clock by at least one TRUNK_T_poll_us,
    // while past_window() turns true one TRUNK_T_turn_max_us after the staged frame's last
    // byte (link/responder.cpp, past_window/transmit_if_due). So the first poll here is
    // already outside the window and stats().late_responses — accumulated below — is what
    // records the transmit. Which branch runs is immaterial to what this file is for: either
    // way responder.cpp is linked and driven.
    g_node_wire.preload(g_wire_bytes, written, 0);
    for (uint8_t i = 0; i < kSteps; ++i) {
        g_clock.advance(omgp::TRUNK_T_turn_max_us);
        g_responder.poll(g_clock.now_us());
    }
    acc = static_cast<uint16_t>(acc + g_responder.stats().transactions +
                                g_responder.stats().late_responses +
                                g_responder.stats().replays_served);

    // trunk §6/§7 — node health (link/health.cpp): one enrolment-rotation probe, its
    // outcome, the time-only transitions and the poll schedule.
    const omgp::link::Probe probe = g_health.next_probe(g_clock.now_us());
    acc = static_cast<uint16_t>(acc + (probe.bit_rate == g_master_wire.bit_rate()));
    if (probe.addr != omgp::ADDR_host) { // the "no candidate" sentinel (link/health.hpp)
        g_health.on_result(probe.addr, true, g_clock.now_us());
    }
    g_clock.advance(omgp::TRUNK_T_poll_us);
    g_health.tick(g_clock.now_us());
    if (g_health.poll_due(kNodeAddr, g_clock.now_us())) {
        g_health.mark_polled(kNodeAddr, g_clock.now_us());
    }
    acc = static_cast<uint16_t>(acc + static_cast<uint8_t>(g_health.state(kNodeAddr)) +
                                g_health.bus_stats().bus_faults + g_health.bus_fault());

    return static_cast<uint16_t>(acc + g_listener.tally() + g_master_wire.sent() +
                                 g_node_wire.sent());
}
