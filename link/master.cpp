// OMGP trunk L2 — Master engine (trunk §3, §7): signature-only stub (see master.hpp).
// T031 replaces every body here with the real retry/timing/acceptance state machine.
#include "link/master.hpp"

namespace omgp {
namespace link {

Master::Master(ByteWire& wire, Clock& clock, uint8_t host_addr)
    : wire_(wire), clock_(clock), host_addr_(host_addr) {
    (void)clock_;     // discarded read: see the clock_ declaration comment in master.hpp
    (void)host_addr_; // discarded read: stored for constructor parity only until T031
}

Status Master::begin(uint8_t, const uint8_t*, size_t) {
    return Status::Ok;
}

MasterEvent Master::poll(uint64_t) {
    return MasterEvent{};
}

bool Master::busy() const {
    return false;
}

uint8_t Master::attempts() const {
    return 0;
}

void Master::set_bit_rate(uint32_t bps) {
    wire_.set_bit_rate(bps);
    ++bus_stats_.rate_changes;
}

const AddrStats& Master::stats(uint8_t addr) const {
    return stats_[addr];
}

const BusStats& Master::bus_stats() const {
    return bus_stats_;
}

void Master::reset_stats() {
    for (auto& s : stats_)
        s = AddrStats{};
    bus_stats_ = BusStats{};
}

} // namespace link
} // namespace omgp
