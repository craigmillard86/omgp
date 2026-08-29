# Contract: `MockWire` scripted transport and `FakeClock` (`tests/support/`)

Host-side test infrastructure (may use the full language, but stays allocation-free so
F4 can seed its virtual wire from it). Implements `ByteWire`; drives the engine under test
through explicit time.

## Step table

```cpp
enum class Kind : uint8_t { Respond, Silence, Garbage, CrcError, Duplicate, Babble, Rate };
struct Step { uint8_t node; Kind kind; uint32_t delay_us; uint16_t count; uint32_t seed; };
```

| Kind | Effect on the next request addressed to `node` |
|---|---|
| `Respond` | the node's `RequestHandler` (usually a real `Responder`) answers; first byte at `request_end + delay_us` (delay defaults to `TRUNK_T_turn_min_us`; a delay ≥ `TRUNK_T_resp_us` makes a "late" response for timeout tests) |
| `Silence` | nothing is transmitted |
| `Garbage` | `count` PRNG bytes (never containing a valid frame — checked at generation) starting at `request_end + delay_us`, then nothing |
| `CrcError` | the real response with its last CRC byte XOR 0xFF, at `request_end + delay_us` |
| `Duplicate` | the real response, then the same bytes again `delay_us` after the first ends (used to plant a late duplicate that must be discarded) |
| `Babble` | `count` PRNG bytes at `request_end + delay_us` **regardless of addressee** (also emitted when a different node is polled, i.e. outside any window) |
| `Rate` | the node now "hears" only at `count` interpreted as bit rate (1 000 000 or 115 200); requests at another rate behave as `Silence` (or `Garbage` if `seed != 0`) |

Scripts are per-node arrays consumed in order; an exhausted script behaves as `Respond`
with the default delay. Steps with `node == 0xFF` apply to every node.

## Scheduling

- `transmit(bytes, n, now)` records the frame, deframes it with the real `Deframer`,
  looks up the addressed node's next step, and enqueues RX bytes with computed start-bit
  instants (`t0 = tx_end + delay`, byte *i* at `t0 + i × byte_time_us(rate)`). Returns
  `now + n × byte_time_us(rate)`.
- `advance_to(t)` (test helper) sets the `FakeClock` and calls the engine's `poll(t)`;
  the engine drains the mock's `receive()` itself, which returns only bytes whose
  `start_us ≤ t` (bytes "in the future" stay queued). There is no push path into the
  engines (analysis F1). Tests step time explicitly; nothing sleeps.
- Capacity: a fixed queue of `4 × kMaxWire` bytes (enough for a response, a duplicate and
  a babble burst); overflow is a test failure (`REQUIRE`), never silent truncation.
- All randomness from `Step::seed` through an xorshift32 in the mock; the same script
  reproduces byte-for-byte.

## What the tests assert through the mock

- Exact wire transcript: the sequence of transmitted frames (fields, retry bit, seq) and
  their `tx_start_us` — this is how `T_gap`, `T_turn` and retry counts are checked.
- Engine outcomes (`MasterEvent`, notifications, `AddrStats`) after each `advance_to`.
- The §7-mode → script mapping table (SC-005) lives in `tests/unit/test_link_loop.cpp` as
  a comment block naming the script for: response timeout, CRC-failed response, SUSPECT,
  OFFLINE, BUS_FAULT, babble, duplicate, wrong-rate probe.
