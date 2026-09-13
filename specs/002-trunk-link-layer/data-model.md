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

- **Receive path**: a shared drain first reads `ByteWire::receive()` into the engine's
  `Deframer` (each byte with its start-bit instant) to exhaustion; it is the only receive
  path (analysis F1). `poll(now)` runs it, then evaluates the state machine; `begin()` runs
  the same drain before deciding whether/when to transmit, so its own "never transmits over
  an arriving frame" guarantee (see "Gap" below) does not depend on a `poll(now)` having
  immediately preceded it (#138). The same drain-then-evaluate shape holds for the Responder.

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
  bound": a retry is replayed only when it repeats the request that was answered, compared over
  `dst`, `src`, `len` and the L3 payload — `ctrl`, which carries the retry bit, and the CRC are
  EXCLUDED, since a retry is never byte-identical to the request it retries (trunk §7). So
  `ReplayBuffer` gains a fixed-size copy of those bytes. Any other retry is treated as new. A time-based expiry was rejected, because the host retries only after its
  `T_resp` window, so expiry would disable replay, and `GET_EVENT` retries depend on replay.
  A retry whose `seq` and `peer` match the buffer but whose compared bytes do NOT is
  **discarded and counted** — the handler is not invoked and the buffer is left intact. Treating
  it as new would let a spoofed frame evict the buffer and force a second `GET_EVENT` drain,
  widening the eviction surface this ruling narrows (added after the round-2 review of #375).
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
  *(Ruled 2026-09-11, `OPEN-QUESTIONS.md` "Maintainer rulings … 2026-09-11" item 5: the property
  preserved is **"the engine never keys down onto a bus it has not read, nor over another
  station's arriving frame"**, and it outranks FR-014's "at once" and this bullet's "nothing is
  dropped". That property is NOT FR-017 — a late response is by construction outside its
  response window, which is what `late_responses` counts — so FR-017 and trunk §3's absolute
  bullet stay unamended and in tension with FR-014's late-transmit clause; that tension is the
  still-open 2026-09-06 held-request-queue entry. The engine never stops reading during a late
  wait, and a completed request beyond `kHeldRequests` is discarded and counted in `stats()`, so
  its reading of the bus stays complete and the stale-belief path in (b) goes away.
  Implementation: #372.)*

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
10 × T_poll`; OFFLINE, UNENROLLED → false (reached only via `next_probe`). *(Amended
2026-09-13, F4.)* While `fault` (§7) → false for every address: no status polls are sent
while a fault is declared, only `next_probe()` drives the wire, so a node that answered at
the fallback rate is not polled at the reference rate during the reference pass. Enrolment
rotation (`next_probe(now)`): round-robin over addresses 0x01–0x0F whose state is
UNENROLLED or OFFLINE; returns `{addr, bit_rate}`. *(Amended 2026-09-13, F4.)* While `fault`
(§7) the candidate set also includes every SUSPECT address, so the alternating-rate probes
reach the nodes whose silence declared the fault at once rather than only after they age to
OFFLINE; **at most one probe is outstanding at a time while `fault`** — `next_probe()` returns
`{addr = 0, …}` ("nothing to issue"; 0x00 is the host and never a target) whenever
`BusState.outstanding ≠ 0`, i.e. until the previous probe's `on_result` has arrived — so every
`on_result` during a fault is the outcome of the one probe in flight, and no correlation token
is needed (round-10 red team on #472); and during a reference pass (§7) `next_probe()` yields
each enrolled address (state ≠ UNENROLLED) once, in address order, at `TRUNK_bit_rate`,
without alternating, one at a time under the same rule.

## 7. Bus state

```
BusState { u32 bit_rate; bool fault; bool next_probe_fallback; u32 rate_changes; u32 faults;
           u8 fallback_answerer; u8 ref_pass_left; u8 outstanding }   /* last three added 2026-09-13 (F4, below; 0 = none); implemented by T043 */
```

- Declare (`fault = true`, `BUS_FAULT` + `ALERT` notifications, `faults++`): evaluated
  after every `on_result`/`tick`: enrolled = {addr : state ≠ UNENROLLED}; if
  `|enrolled| ≥ 1` and every enrolled node ∈ {SUSPECT, OFFLINE} and `!fault`.
- While `fault`: `next_probe()` alternates `bit_rate` between reference and fallback per
  call (starting with the fallback, §7), except during a reference pass (Clear, below), when
  every probe goes out at the reference rate; `rate_changes++` on each change.
- Clear *(amended 2026-09-13: F4 ruling 2026-09-06, ruling 2026-09-13)*: while `fault`, a valid
  answer at the **reference** rate → `fault = false`, `bit_rate = TRUNK_bit_rate`, `BUS_RECOVERED`;
  the answering node → ENROLLED as in §6. A valid answer at the **fallback** rate does not clear
  the fault by itself and does **not yet** change that node's state: `fallback_answerer = addr`
  (its §6 transition is deferred — an ENROLLED node would be status-polled at the reference
  rate it cannot hear, fail into SUSPECT, and the recovery would be re-declared as a fault at
  once), and a **reference pass** starts — `ref_pass_left = |enrolled|` where enrolled =
  {addr : state ≠ UNENROLLED}. Because at most one probe is outstanding while `fault` (§6), the
  fallback answer that starts the pass IS the outcome of the only probe in flight: nothing
  else is outstanding when the pass begins, each pass probe is issued only after the previous
  outcome arrived, and every `on_result` during the pass is that pass probe's — matched by
  `outstanding` alone, no rate token (round-10 red team on #472; the round-9 `pass_addr` +
  rate matcher could not be exact while two transactions to one address could exist).
  `ref_pass_left` is decremented by each pass outcome, never when a probe is issued (a
  `tick()` between the last probe and its result must not fire the clear, round-8 red team).
  A valid answer at the reference rate during the pass → clear at the reference rate as above,
  at once (FR-026); the recorded answerer keeps the state it had (it cannot hear the reference
  rate and will be found by §6 in its own time). If
  `fallback_answerer ≠ 0` and the pass outcome that takes `ref_pass_left` to 0 is itself a
  failure → `fault = false`,
  `bit_rate = TRUNK_bit_rate_fallback`, the recorded answerer → ENROLLED (failures = 0) as in
  §6 **before** the declare rule is next evaluated, `BUS_RECOVERED`. (`ref_pass_left == 0`
  alone means no pass is running; the clear at the fallback rate is conditioned on the
  recorded answerer.) The pass length is exactly `|enrolled|` probes — the same number in
  `trunk §7`, FR-026 and here; trunk §7's two retries give each address three attempts. On
  a clear at the fallback rate every other enrolled node keeps its SUSPECT/OFFLINE state and
  timers. `rate_changes++` on each change of `bit_rate`. On declare, and on every clear at
  either rate **after the deferred enrolment above has been applied**, `fallback_answerer = 0`
  and `ref_pass_left = 0` (and `outstanding` is cleared by the outcome that clears) — a clear
  at the reference rate can fire mid-pass, and a stale pass counter must not survive it; resetting first would lose the answerer the clear still needs
  and re-declare the fault at once (round-8 red team on #472).
  The superseded **Clear** clause read "first `ok` result at any rate while `fault` →
  `fault = false`, `bit_rate` = the rate that got the answer": the answering rate pinned the
  trunk, so one node strapped at the fallback rate could downgrade a reference-rate rig (#110
  F4).
- Rate and the schedule (F4, 2026-09-06: "`T_poll` and the §6 budget derive from the rate in
  use"): a transaction issued at the fallback rate takes `TRUNK_bit_rate / TRUNK_bit_rate_fallback`
  (≈ 8.7×) the time of the same transaction at the reference rate, and the superframe that issues
  it is stretched accordingly — whether that is because `bit_rate` is the fallback rate, or
  because a fault-time probe (§7, alternation) or a pass probe is issued at a rate other than
  `bit_rate` (`bit_rate` is assigned only by a clear; during a fault the probe's rate is the
  probe's own, round-9 red team on #472). One minimal status-poll transaction alone is ≈ 2.4 ms
  at 115.2 kbit/s, so §6's 2 ms superframe cannot hold one. Implemented where the scheduler
  consumes the probe's `bit_rate` and `bit_rate()` (F3), not in this tracker.
- No automatic return (ruling 2026-09-13): nodes select their rate by strap or configuration
  (trunk §2) and cannot hear a probe at the other rate, so once `!fault` the rate in use stands
  until the layer above or a human changes it. The fallback rate is a bring-up rate. A
  cadence-and-quorum return was ruled and withdrawn the same day; see `docs/OPEN-QUESTIONS.md`.
- Fall back (while `!fault` and `bit_rate == TRUNK_bit_rate`): only through the declare rule
  above — a fault is declared only when every enrolled node is SUSPECT/OFFLINE, so the host
  never leaves the reference rate while any enrolled node answers at it.

## 8. Statistics (FR-011a)

```
AddrStats { u32 transactions, retries, timeouts, crc_failures, discards, replays_served, late_responses }   × 16
BusStats  { u32 rate_changes, bus_faults, discards }
```
Readable via `stats(addr)` / `bus_stats()`, `reset_stats()`; incremented at the point the
event is decided (e.g. `retries` when the retry frame is handed to the wire).

`AddrStats::discards` counts **decoded** frames — those that passed CRC and then failed the
acceptance screen — discarded while that address's own transaction is awaiting a response.
`BusStats::discards` counts every other frame-level discard the Master decides: a decoded
frame that fails the screen with no transaction awaiting, and a CRC-failed frame that did
not open inside an awaiting transaction's window.

The split is by what the engine has to attribute the frame with, and the two paths differ on
purpose (they disagree in the reachable `awaiting`-but-out-of-window state, pinned by
`test_link_master.cpp` "while dst's transaction is awaiting but the frame opened outside its
window …"). A decoded frame arrives during a transaction the host itself opened, so there is
an address it may charge — `dst`, the address the host addressed, whoever actually sent the
frame; its own `src` is wire-derived and unauthenticated (trunk §5 reserves only `0xFF`) and
so is never used, and with no transaction awaiting there is nothing else to charge. A
CRC-failed frame decodes to nothing, so the only thing that could attribute it is timing:
inside the awaiting address's window it is that node's `crc_failures` (never its `discards`),
and outside it there is nothing left to attribute it by.
*(Ruled 2026-09-11, `OPEN-QUESTIONS.md` "Maintainer rulings … 2026-09-11" item 8 and the
maintainer's comment on #144, settling the 2026-09-06 "claimed src is out of range" and
"idle-time discard is charged to the frame's CLAIMED in-range src" entries and the
2026-09-07 "a CRC-corrupt frame arriving with no transaction open moves no counter at all".
Implementation: #144. Structural Deframer discards — `BadLength`, `BadEscape`, `TooLong`,
`ReservedAddress` — are counted in `DeframerStats` only and remain invisible above the
engine; that residue is a separate open entry, dated 2026-09-13.)*

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
