// trunk §3: media access — the byte-level transport abstraction the Master/Responder
// engines drive, so the same L2 code runs unmodified over a scripted test wire and the
// simulator's virtual wire.
// mutation-exempt(no-body): pure abstract interface, four `= 0` declarations — no function
// body for Mull to mutate (tools/mutate_report.py; docs/OPEN-QUESTIONS.md 2026-08-30).
#pragma once
#include <cstddef>
#include <cstdint>

namespace omgp {
namespace link {

struct ByteWire {
    // Transmit n bytes back-to-back starting at `now_us`; returns the instant of the final
    // stop bit (now_us + n * byte_time_us(bit_rate())). The implementation owns the driver
    // enable; the engine never transmits while it believes the bus is busy.
    virtual uint64_t transmit(const uint8_t* bytes, size_t n, uint64_t now_us) = 0;
    // Next received byte and the instant of its START bit; false when none is pending.
    virtual bool receive(uint8_t& byte, uint64_t& start_us) = 0;
    virtual uint32_t bit_rate() const = 0;
    virtual void set_bit_rate(uint32_t bps) = 0;

  protected:
    ~ByteWire() = default;
};

} // namespace link
} // namespace omgp
