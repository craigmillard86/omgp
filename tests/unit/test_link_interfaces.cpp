// trunk §3: Clock and ByteWire are pure interfaces (T007). This is the first translation
// unit that actually compiles them — closing the gap noted in PR #91 review comment 3,
// where nothing in the tree included either header, so pipeline "green" could not have
// caught a syntax error in them. Minimal derived classes exercise both vtables; the
// destructor checks pin the contract's "protected, non-virtual destructor" requirement
// (deleting through the base pointer must not compile).
#include "catch_amalgamated.hpp"
#include "link/byte_wire.hpp"
#include "link/clock.hpp"

#include <type_traits>

namespace {

class FixedClock final : public omgp::Clock {
  public:
    explicit FixedClock(uint64_t t) : t_(t) {}
    uint64_t now_us() override {
        return t_;
    }

  private:
    uint64_t t_;
};

class ScriptedWire final : public omgp::link::ByteWire {
  public:
    uint64_t transmit(const uint8_t* bytes, size_t n, uint64_t now_us) override {
        (void)bytes;
        return now_us + n;
    }
    bool receive(uint8_t& byte, uint64_t& start_us) override {
        (void)byte;
        (void)start_us;
        return false;
    }
    uint32_t bit_rate() const override {
        return bit_rate_;
    }
    void set_bit_rate(uint32_t bps) override {
        bit_rate_ = bps;
    }

  private:
    uint32_t bit_rate_ = 0;
};

} // namespace

TEST_CASE("Clock is overridable and its destructor is not publicly accessible", "[link]") {
    FixedClock clock(42);
    omgp::Clock& base = clock;
    REQUIRE(base.now_us() == 42);

    // is_destructible<T> is false when ~T() is inaccessible from an unrelated context —
    // exactly what a protected destructor buys against `delete` through the base pointer.
    STATIC_REQUIRE_FALSE(std::is_destructible<omgp::Clock>::value);
}

TEST_CASE("ByteWire is overridable and its destructor is not publicly accessible", "[link]") {
    ScriptedWire wire;
    omgp::link::ByteWire& base = wire;

    base.set_bit_rate(31250);
    REQUIRE(base.bit_rate() == 31250);

    uint8_t byte;
    uint64_t start_us;
    REQUIRE_FALSE(base.receive(byte, start_us));

    const uint8_t payload[3] = {0x01, 0x02, 0x03};
    REQUIRE(base.transmit(payload, 3, 1000) == 1003);

    STATIC_REQUIRE_FALSE(std::is_destructible<omgp::link::ByteWire>::value);
}
