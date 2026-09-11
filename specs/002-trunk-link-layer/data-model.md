# Data Model: Trunk Link Layer (feature 002)

Phase 1 of `/speckit.plan`. Entities from spec.md "Key Entities", made concrete against
`docs/trunk-link-layer.md` §4/§5/§7/§9 and the generated `link_trunk` symbols. Names
here are the names the C++ (`omgp::link`) and Python (`omgp_link`) implementations use.

## 1. Constants (all generated; never restated)

| Symbol (C++) | YAML key | Value | Used by |
|---|---|---|---|
| `TRUNK_flag_byte` | `flag_byte` | 0x7E | codec |
| `TRUNK_escape_byte`, `TRUNK_escape_xor` | `escape_byte`, `escape_xor` | 0x7D, 0x20 | codec |
| `LIMIT_max_l3_payload` | `limits.max_l3_payload` | 64 | codec, engines |
| `TRUNK_bit_rate`, `TRUNK_bit_rate_fallback` | `bit_rate*` | 1 000 000, 115 200 | timing model, bus fault |
| `TRUNK_T_turn_min_us`, `TRUNK_T_turn_max_us` | `T_turn_*` | 20, 100 | Responder |
| `TRUNK_T_resp_us` | `T_resp_us` | 200 | Master |
| `TRUNK_T_gap_us` | `T_gap_us` | 50 | Master |
| `TRUNK_T_poll_us` | `T_poll_us` | 2000 | HealthTracker (×10 for SUSPECT) |
| `TRUNK_retries` | `retries` | 2 | Master |
| `TRUNK_suspect_after_failures` | `suspect_after_failures` | 3 | HealthTracker |
| `TRUNK_offline_after_suspect_ms` | `offline_after_suspect_ms` | 1000 | HealthTracker |

Derived (`constexpr`, `link_types.hpp`): `kHeaderLen = 4`, `kCrcLen = 2`,
`kMaxUnstuffed = kHeaderLen + LIMIT_max_l3_payload + kCrcLen` (70),
`kMaxWire = 2 + 2 * kMaxUnstuffed` (142), `kAddrCount = 16` (§5: 0x00–0x0F),
`kSeqMask = 0x0F`, `kSuspectPollPeriod_us = 10 * TRUNK_T_poll_us`,
`byte_time_us(bps) = 10_000_000 / bps` (8N1 → 10 bits; integer µs: 10 at 1 Mbit/s, 86 at
115.2 kbit/s — the fallback figure truncates 86.8; recorded as the model, tests use it).

## 2. Frame (§4)

```
FrameFields { u8 dst; u8 src; bool response; bool retry; u8 seq (0..15); u8 len; u8 payload[≤64] }
ctrl byte    = response<<0 | retry<<1 | (seq & 0x0F)<<4     (bits 2-3 zero on encode; ignored on decode)
unstuffed    = dst src ctrl len payload[len] crc_lo crc_hi   (crc = CRC-16/CCITT-FALSE over dst..payload)
wire         = FLAG stuff(unstuffed) FLAG
```

- **Validation on encode** (`Status`): `len > 64` → `PayloadTooLong`; `dst == 0xFF` →
  `ReservedAddress`; caller buffer `< needed` → `BufferTooSmall` (nothing written).
- **Validation on decode** (discard reasons, counted, never reported per frame): `BadCrc`,
  `BadLength` (accumulated < 6, or `len ≠ accumulated − 6`, or > 70 unstuffed), `BadEscape`
  (escape followed by anything but 0x5E/0x5D; also escape immediately before FLAG),
  `ReservedAddress` (`dst == 0xFF`; a delivered frame there could never be re-encoded, since
  encode refuses that address — ruling 2026-08-31, docs/OPEN-QUESTIONS.md).
- **FrameView** (decoded): fields + `const uint8_t* payload` into the deframer's
  accumulator, valid until the next byte is fed.

## 3. Deframer state machine

| State | On FLAG | On ESC | On other byte |
|---|---|---|---|
| `Hunting` | → `InFrame` (acc = 0) | stay | stay |
| `InFrame` | acc = 0 → stay (empty); else validate → deliver or discard; → `InFrame` (acc = 0) — the FLAG both closes and opens | → `Escaped` | append (acc == 70 → `TooLong`, discard, → `Hunting`) |
| `Escaped` | `BadEscape` discard → `InFrame` (this FLAG opens a frame) | `BadEscape` discard → `Hunting` | byte ∈ {0x5E, 0x5D} → append byte ^ 0x20, → `InFrame`; else `BadEscape` discard → `Hunting` |

Invariants: memory = one 70-byte accumulator + state + counters; no input can grow it;
after any discard the next FLAG opens a frame (FR-003); the "≥ 8 consecutive stuffing
violations" clause is satisfied trivially (ruling Q1).

## 4. Transaction (Master)

```
Transaction { u8 dst; u8 seq; u8 attempt (0..2); u8 payload[64]; u8 len;
              u64 tx_start_us, tx_end_us; Outcome }
Outcome ∈ { Pending, Answered, Failed(Timeout | Crc) }     -- Crc: last attempt saw a CRC-failed frame
Master state ∈ { Idle, Transmitting(until tx_end), AwaitResponse(until tx_end + T_resp), Gap(until min(last_activity + T_gap, defer_origin + max_frame + T_gap)) }
```

- **Receive path**: `poll(now)` first drains `ByteWire::receive()` into the engine's
  `Deframer` (each byte with its start-bit instant), then evaluates the state machine. It
  is the only receive path (analysis F1); the same holds for the Responder.

- **Sequence**: per-destination counter `next_seq[16]`; `begin()` uses `next_seq[dst]++ & 0x0F`
  for a new transaction; retries reuse `seq` and set `retry`.
- **Response acceptance** (all must hold): intact frame; `src == dst_of_request`;
  `dst == host_addr` (the Master's constructor argument, default `0x00` — a Master built
  with another address accepts only frames to it); `response == 1`; `seq ==
  transaction.seq`; first byte's start instant in `[tx_end_us, tx_end_us + T_resp)` — the
  closed lower bound discards a "response" whose FLAG opened inside the host's own
  transmission, as `contracts/link-cpp.md` already states (both amended in PR #137 review
  @`1057568`, LOW: this list said `dst == 0x00` and gave only the upper bound; the code,
  `link/master.cpp` `in_window` and the `host_addr_` compare, is what the tests pin). A
  CRC-failed frame in the window ends the attempt
  immediately (§7: "CRC-failed response" is a failure, no need to wait for the timeout).
  A frame that opened inside the window and is still arriving at the window's end (bytes
  still coming at byte cadence) holds the `Timeout` off until it concludes — bounded at
  `resp_open + max_frame` (one worst-case frame from the opening FLAG), whatever the poll
  cadence; no legitimate response can reach that cap. *(Amended in PR #137, red-team
  @`9547634` HIGH — pending the same ruling as "Gap" below.)*
- **Retry**: attempts 0, 1, 2 → at most 3 transmissions; after attempt 2 fails → `Failed`.
- **Gap**: `last_activity` = end of the accepted response's last byte, or the last
  byte of a discarded frame, or the timeout instant; `begin()` before `last_activity +
  T_gap` is accepted but transmission is deferred to that instant (the engine, not the
  caller, guarantees the gap). `last_activity` advances with every byte received during the
  deferral, so the engine never transmits over an arriving frame — but only up to a bound:
  `defer_origin + max_frame + T_gap`, where `defer_origin` is the instant first deferred to
  and `max_frame` is one worst-case frame (`kMaxWire` byte times at the current rate, trunk
  §4 / SC-008). trunk §3 owes the gap after the host's OWN transactions; deferring for
  anyone else's bytes is a courtesy, and past the cap the host transmits on schedule and
  the transaction fails or succeeds on its own merits (spec.md Edge Cases "Babble"). No
  outcome is ever derived from the bus state. *(Amended in PR #137 — pending a ruling, see
  `docs/OPEN-QUESTIONS.md` 2026-09-05 "bounded courtesy".)*
- **Events** returned by `poll(now)`: `None`, `Answered{payload view}`, `Failed{reason}`;
  each transaction yields exactly one terminal event.

## 5. Responder

```
ReplayBuffer { bool valid; u8 seq; u8 peer; u16 len; u8 bytes[142] }
Responder state ∈ { Listening, Scheduled(response at request_end + turnaround_us), Transmitting(until tx_end) }
```

- Request acceptance: intact frame, `dst == my_addr`, `response == 0`. *(Amended in PR #149,
  red-team @`6c2fe4b` / @`71caba0` — also refused: `src == my_addr`, `src` outside trunk §5's
  L2 range, and every request when the node's OWN address is outside it. Ruled 2026-09-11:
  adopted, since trunk §5's address range is the higher authority; see `docs/OPEN-QUESTIONS.md`
  2026-09-07 "the Responder's acceptance screen" and the 2026-09-11 rulings entry.)*
- `retry == 1 && valid && seq == buffer.seq` → retransmit buffer (no handler call).
  *(Amended in PR #149, red-team @`033182a` — the buffer additionally records the requester
  (`ReplayBuffer.peer`) and a retry from a DIFFERENT station with a colliding sequence is
  treated as new. Pending the ruling on file, `OPEN-QUESTIONS.md` 2026-09-06.)*
  *(Ruled 2026-09-11, `OPEN-QUESTIONS.md` 2026-09-07 "the Responder's replay entry has no age
  bound": a retry is replayed only when its request bytes also equal the request that was
  answered, so `ReplayBuffer` gains a fixed-size copy of that request. Any other retry is
  treated as new. A time-based expiry was rejected, because the host retries only after its
  `T_resp` window, so expiry would disable replay, and `GET_EVENT` retries depend on replay.
  Implementation: #373.)*
- otherwise → `handler.handle(payload, len, out, cap) → resp_len` once; encode response
  (`src = my_addr`, `dst = request.src`, `response = 1`, `retry` echoed, `seq` echoed);
  store in buffer; schedule.
- `turnaround_us` clamped to `[T_turn_min, T_turn_max]` at construction.
- **Late poll** (spec FR-014): if the first `poll(now)` after a request has
  `now > request_end + T_turn_max`, the response is transmitted at `now` and
  `AddrStats.late_responses` is incremented; nothing is dropped. *(Amended in PR #149,
  red-team @`71caba0` HIGH — "at `now`" is qualified by a bounded courtesy resembling, but
  **weaker than**, the one §4 records for the Master: outside its window the engine owes trunk
  §3 a bus that is idle, so it transmits at
  `min(last_activity + T_gap, defer_origin + max_frame + T_gap)` when its reading of the bus is
  COMPLETE, and at `defer_origin + max_frame + T_gap` — the cap alone, whatever `last_activity`
  says — when the drain stopped with requests held and the reading is therefore partial.
  Two corrections to earlier wording of this marker, both from review rounds on #149:
  (a) it is NOT "the same bounded courtesy §4 records for the Master". §4's property rests on
  a drain that never exits early, so `last_activity` advances with every byte and the engine
  "never transmits over an arriving frame"; the stale-belief path here has no such
  precondition — Master's cap without Master's precondition (`link/responder.cpp` says the same
  in terms). (b) It does **not** follow that the engine never keys down inside another
  station's frame: on the stale-belief path it demonstrably can, which is an OPEN defect with a
  reproduced counterexample, not a property. Pending a ruling, see `OPEN-QUESTIONS.md`
  2026-09-07 "the Responder's late path defers for an idle bus" for the deferral itself, and
  "the Responder's late path CAN transmit into a frame it has not read" for the open defect in
  (b) — the entry the ruling should be made from, as `contracts/link-cpp.md` also points out.)*
  *(Ruled 2026-09-11: FR-017 and trunk §3 outrank FR-014's "at once" and this bullet's
  "nothing is dropped". The engine never stops reading during a late wait, and a completed
  request beyond `kHeldRequests` is discarded and counted in `stats()`. Its reading of the bus
  therefore stays complete, and the stale-belief path in (b) goes away. Implementation: #372.)*

## 6. Node health record

```
HealthRecord { HealthState state; u8 consecutive_failures; u64 suspect_since_us; u64 last_poll_us; bool ever_answered }
HealthState ∈ { UNENROLLED, ENROLLED, SUSPECT, OFFLINE }
```

Transitions (`on_result(addr, ok, now)`):

| From | Input | To | Notification |
|---|---|---|---|
| UNENROLLED | ok | ENROLLED (failures = 0) | `ENROLLED` |
| UNENROLLED | fail | UNENROLLED (no count) | — |
| ENROLLED | fail, failures+1 < 3 | ENROLLED | — |
| ENROLLED | fail, failures+1 == 3 | SUSPECT (suspect_since = now) | `SUSPECT` |
| ENROLLED | ok | ENROLLED (failures = 0) | — |
| SUSPECT | ok | ENROLLED (failures = 0) | `RECOVERED` |
| SUSPECT | fail, now − suspect_since < 1000 ms | SUSPECT | — |
| SUSPECT | fail, now − suspect_since ≥ 1000 ms | OFFLINE | `OFFLINE` |
| SUSPECT | tick(now) with ≥ 1000 ms elapsed (no result needed) | OFFLINE | `OFFLINE` |
| OFFLINE | ok | ENROLLED (failures = 0) | `RECOVERED` |
| OFFLINE | fail | OFFLINE | — |

Poll eligibility (`poll_due(addr, now)`): ENROLLED → true; SUSPECT → `now − last_poll ≥
10 × T_poll`; OFFLINE, UNENROLLED → false (reached only via `next_probe`). Enrolment
rotation (`next_probe(now)`): round-robin over addresses 0x01–0x0F whose state is
UNENROLLED or OFFLINE; returns `{addr, bit_rate}`.

## 7. Bus state

```
BusState { u32 bit_rate; bool fault; bool next_probe_fallback; u32 rate_changes; u32 faults }
```

- Declare (`fault = true`, `BUS_FAULT` + `ALERT` notifications, `faults++`): evaluated
  after every `on_result`/`tick`: enrolled = {addr : state ≠ UNENROLLED}; if
  `|enrolled| ≥ 1` and every enrolled node ∈ {SUSPECT, OFFLINE} and `!fault`.
- While `fault`: `next_probe()` alternates `bit_rate` between reference and fallback per
  call (starting with the fallback, §7); `rate_changes++` on each change.
- Clear: first `ok` result at any rate while `fault` → `fault = false`, `bit_rate` = the
  rate that got the answer, `BUS_RECOVERED`; the answering node → ENROLLED as in §6.

## 8. Statistics (FR-011a)

```
AddrStats { u32 transactions, retries, timeouts, crc_failures, discards, replays_served, late_responses }   × 16
BusStats  { u32 rate_changes, bus_faults }
```
Readable via `stats(addr)` / `bus_stats()`, `reset_stats()`; incremented at the point the
event is decided (e.g. `retries` when the retry frame is handed to the wire).

## 9. Notification kinds (HealthListener)

`ENROLLED`, `SUSPECT`, `OFFLINE`, `RECOVERED`, `BUS_FAULT`, `ALERT`, `BUS_RECOVERED`;
each carries `addr` (0 for bus-level). Exactly one per transition (SC-006).

## 10. MockWire step

```
Step { u8 node; Kind kind; u32 delay_us; u16 count; u32 seed }
Kind ∈ { Respond, Silence, Garbage, CrcError, Duplicate, Babble, Rate }
```
Semantics in `contracts/mock-wire.md`. A script is an array of steps consumed in order
per node; an exhausted script behaves as `Respond` with `delay_us = T_turn_min`.

## 11. Torture corpus element

```
Element { seed; segments: [ {kind: valid|flip|drop|insert|truncate|flag|bad_escape|garbage|overlength, bytes} ];
          expected: [FrameFields...] ; expected_discards: {reason: n} }
```
Generated by `torture.py`; an element whose corruption still parses as a valid frame is
regenerated with the next sub-seed (never emitted). The differential compares the
sequence of delivered canonical frame lines and the total discard count.

## 12. Golden vector, kind `frame`

Schema amendment (feature-001 contract): `kind: "frame"`, `name: frame_*`, `fields =
{dst, src, response, retry, seq, payload}` (payload hex, may be empty), `canonical` per
`contracts/frame-vectors.md`, `bytes` = stuffed wire bytes with both FLAGs.

## 13. Status vocabulary (`omgp::link::Status`, separate from `omgp::l3::Status`)

`Ok, PayloadTooLong, ReservedAddress, BufferTooSmall, Busy` (engine already has a
transaction), `NotIdle`; deframer discard reasons `BadCrc, BadLength, BadEscape, TooLong,
ReservedAddress` are counters, not return values (a discard is silent by §4).
`status_name()` mirrors the L3 helper for tools and tests.
