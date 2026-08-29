# Contract: `Clock` and `ByteWire` interfaces, and the timing model

Both are pure virtual interfaces in `link/` (research R-01); both are embedded-path safe
(no RTTI needed, no allocation). `core/` (F3) includes `link/clock.hpp`; F4's virtual wire
implements `ByteWire`.

## `omgp::Clock` (`link/clock.hpp`)

```cpp
struct Clock {
    virtual uint64_t now_us() = 0;      // monotonic microseconds; never goes backwards
protected: ~Clock() = default;
};
```
Rules: the only source of "now" in `link/` and `core/` (CLAUDE.md rule 3). Engines never
call it inside a loop waiting for a value to change — every wait is `if (now >= deadline)`
in a `poll(now)`; callers pass `now` explicitly so a single reading serves one scheduler
step. `tests/support/fake_clock.hpp`: `FakeClock { uint64_t t; now_us(); advance(us);
set(us); }`.

## `omgp::link::ByteWire` (`link/byte_wire.hpp`)

```cpp
struct ByteWire {
    // Transmit n bytes back-to-back starting at `now_us`; returns the instant of the final
    // stop bit (now_us + n * byte_time_us(bit_rate())). The implementation owns the driver
    // enable; the engine never transmits while it believes the bus is busy.
    virtual uint64_t transmit(const uint8_t* bytes, size_t n, uint64_t now_us) = 0;
    // Next received byte and the instant of its START bit; false when none is pending.
    virtual bool receive(uint8_t& byte, uint64_t& start_us) = 0;
    virtual uint32_t bit_rate() const = 0;
    virtual void set_bit_rate(uint32_t bps) = 0;
protected: ~ByteWire() = default;
};
```

## Timing model (research R-04; asserted by `[timing:bit_rate]` / `[timing:bit_rate_fallback]` tests)

- One byte occupies `byte_time_us(bps) = 10 000 000 / bps` µs (8N1: start + 8 data +
  stop). 10 µs at 1 Mbit/s; 86 µs at 115.2 kbit/s (integer model; the true 86.8 µs is
  within the T_turn/T_resp margins and the value is a single function, not a literal).
- A frame's **final stop bit** = start instant of its last byte + `byte_time_us`.
- `transmit()` returns that instant for the frame just sent; the engine measures
  `T_turn`, `T_resp` and `T_gap` from it.
- Received bytes carry their **start-bit** instant; the response window test is
  `first_byte.start_us < tx_end_us + TRUNK_T_resp_us` (exclusive).
- Worst-case frame: 142 bytes → 1420 µs at the reference rate, 12 212 µs at the
  fallback rate.
- A `set_bit_rate()` takes effect for the next `transmit()`; the mock and the simulator
  decide what a node "hears" at a given rate (contract `mock-wire.md`).

## What the engines guarantee to the wire

- Never call `transmit()` before the previous transmission's returned instant, nor within
  `TRUNK_T_gap_us` after the last received byte's final stop bit (Master).
- The Responder calls `transmit()` only inside `[request_end + T_turn_min, request_end +
  T_turn_max]` and only once per accepted request (or replay).
- `receive()` is the only receive path: each engine drains it at the start of every
  `poll(now)` until it returns false, and consumes every byte in that same `poll()`. An
  implementation must return only bytes whose start instant is ≤ the caller's `now`
  (a virtual wire that knows the future keeps later bytes queued).
