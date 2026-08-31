# Feature Specification: Trunk Link Layer

**Feature Branch**: `002-trunk-link-layer`

**Created**: 2026-08-29

**Status**: Draft

**Input**: User description: "Build the OMGP trunk link layer per docs/trunk-link-layer.md: frame construction and parsing with 0x7E delimiting, byte stuffing, CRC-16 CCITT-FALSE, and mid-stream resynchronisation after corruption; a transmit/receive engine implementing strict master-poll semantics with the retry rule (same L2 sequence, retry flag, max 2 retries), the single-frame replay buffer for responders, and node health tracking (3 consecutive failures to SUSPECT, 1 s in SUSPECT to OFFLINE, reduced-rate polling of SUSPECT nodes, OFFLINE nodes remain in enrolment rotation); and bus-fault detection distinguishing a dead node from a dead bus with fallback-rate re-probe. All timing uses the injected Clock and the generated timing symbols. Success: every timing symbol in the trunk spec section 9 table has at least one test asserting it; framing survives a corrupted-byte torture corpus with correct resynchronisation; retry/replay proven safe by tests that drop, duplicate and corrupt responses; a scripted MockTransport can express delay, silence, garbage, CRC error and babble per step. Out of scope: superframe scheduling policy (F3), real UART."

**Authoritative sources**: `docs/trunk-link-layer.md` §3 (media access), §4 (framing), §5
(addressing), §6 (poll schedule — only as consumed by node health), §7 (errors, retries,
node health), §9 (conformance timing table); `protocol/omgp-protocol.yaml` `link_trunk`
section (every constant this feature uses: bit rates, flag/escape bytes, CRC variant,
`T_turn_min/max_us`, `T_resp_us`, `T_gap_us`, `T_poll_us`, `retries`,
`suspect_after_failures`, `offline_after_suspect_ms`); `docs/protocol-l3.md` §3 only for
the idempotency property that makes L2 retry safe. Where this specification and those
documents disagree, the documents win and the disagreement is a defect in this
specification.

## Clarifications

### Session 2026-08-29

- Q: §4 discards a frame at "≥ 8 consecutive stuffing violations" — does one invalid
  escape abort the frame, or are up to seven tolerated? → A: Any invalid escape aborts the
  frame immediately; the "8" is read as the bound on how long a receiver may keep
  consuming a babbling stream before abandoning the current frame, not as a tolerance.
  Recorded in `docs/OPEN-QUESTIONS.md`.
- Q: What is "all nodes fail simultaneously" for BUS_FAULT, and does one enrolled node
  qualify? → A: Every enrolled node is SUSPECT at the same time; a single enrolled node
  counts as all — the fallback re-probe is what then distinguishes a dead node from a
  dead bus. Recorded in `docs/OPEN-QUESTIONS.md`.
- Q: After BUS_FAULT, what is the rate/recovery policy? → A: Alternate enrolment probes
  between the reference and fallback bit rates; the first valid answer clears the fault
  and the rate that obtained it becomes the rate in use (restoring the reference rate
  later is an L4/human action). Recorded in `docs/OPEN-QUESTIONS.md`.
- Q: Where does the link layer sit relative to the message-level `OMGPTransport` of Spec
  §42, and does the simulator's `VirtualTransport` run the real L2 engines? → A: Below it.
  The trunk implementation of `OMGPTransport` is the L2 master engine over a **byte-wire**
  interface this feature defines (transmit bytes; receive bytes with arrival instants;
  get/set bit rate). The simulator's `VirtualTransport` (F4) runs the same L2 engines over
  a virtual byte wire with fault injection, so every §7 mode is wire-level scenario
  configuration; this feature's scripted test transport is the first byte-wire
  implementation. The host-core still sees only messages (Spec §41).
- Q: What are the trunk node-health states called? → A: The trunk document's own words:
  `UNENROLLED` (never answered), `ENROLLED` (healthy), `SUSPECT`, `OFFLINE`. The L3
  `node_states` (INIT/READY/…) are module states and are not reused for trunk health.
- Q: Should the link layer expose per-node statistics to the layers above? → A: Yes — a
  fixed-size per-address counter block (transactions, retries, timeouts, CRC-failed
  responses, discarded frames, replays served) plus bus-level counters (bit-rate changes,
  bus faults), read-only to the layer above and resettable by it; part of the interface
  note and asserted by this feature's tests (FR-011a).

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Frames survive a hostile wire (Priority: P1)

A developer building or testing any trunk node needs to turn an L3 message into the
exact bytes the trunk carries and back again, and needs the receiver to keep working
when the wire delivers noise: a flipped bit, a dropped byte, an inserted byte, a frame cut
short, a burst of garbage between frames. Every frame that arrives intact after a
corruption must be recovered; nothing corrupted may be accepted as a frame.

**Why this priority**: framing is the foundation every other story stands on, and it is
the only part of L2 whose bytes will be compared against real hardware captures. It is
also a codec, so it inherits the dual-verification and fuzzing obligations of
constitution Principle III.

**Independent Test**: encode a message with worst-case stuffing content and compare
against a golden vector; feed the receiver a seeded torture corpus (valid frames
interleaved with every corruption class) and assert that exactly the intact frames are
delivered, in order, and that the receiver never crashes, hangs or over-reads on
arbitrary bytes. Both implementations (host-core and Python reference) must agree on every
corpus element.

**Acceptance Scenarios**:

1. **Given** an L3 message of up to 64 bytes, **When** it is framed for the trunk, **Then**
   the bytes are FLAG, dst, src, ctrl, len, payload, CRC-16 (little-endian), FLAG, with
   every 0x7E and 0x7D inside dst..crc replaced by its two-byte escape, and the CRC
   computed over the unstuffed dst..payload bytes exactly as §4 states.
2. **Given** a valid frame preceded by garbage bytes and followed by another valid frame,
   **When** the receiver consumes the stream one byte at a time, **Then** both frames are
   delivered intact and the garbage produces no frame.
3. **Given** a frame whose CRC does not match, whose `len` exceeds 64, or whose escape
   sequences are invalid, **When** the receiver sees it, **Then** it is discarded with no
   output, and the very next intact frame is delivered — resynchronisation is on the next
   FLAG, never on a byte count.
4. **Given** a message containing only 0x7E and 0x7D bytes (worst-case stuffing), **When**
   it is framed, **Then** the frame is at most 142 bytes (2 flags + 2 × (4 + 64 + 2))
   and its transmission at the reference bit rate takes no more than 1.42 ms — §4's
   "≤ ~1.4 ms" bound made exact (corrected 2026-08-29 during planning; the earlier
   140 / 1.4 ms figures were an arithmetic slip).
5. **Given** the seeded torture corpus, **When** both implementations consume it, **Then**
   they deliver the same frames at the same positions and reject the same corruptions
   (differential agreement), and the host-core receiver runs for the CI fuzz budget on
   arbitrary bytes with zero findings.

---

### User Story 2 - The host polls, waits, retries — and never double-applies (Priority: P1)

The host needs to send one request to one node, wait for its response within the
timeout, and, when the response is missing or corrupt, retry in the one way the spec
allows: same sequence number, retry flag set, at most two retries. A response that is
late, duplicated, stale or corrupt must never be mistaken for the answer to a different
request.

**Why this priority**: this is the trunk's media-access rule made executable; every L3
operation the host-core will ever perform runs through it, and the retry rule is the
mechanism constitution Principle V exists to make safe.

**Independent Test**: drive the host-side engine against a scripted transport that, per
step, delays, stays silent, sends garbage, sends a CRC-corrupt response, duplicates a
response, or babbles, with simulated time advanced explicitly; assert the exact sequence
of frames the host puts on the wire (sequence numbers, retry flags, inter-frame gaps) and
the outcome reported for each transaction.

**Acceptance Scenarios**:

1. **Given** an idle bus, **When** the host sends a request and the node answers within
   the response window, **Then** the transaction succeeds with that response and no retry
   is sent.
2. **Given** a node that stays silent, **When** the response timeout elapses, **Then** the
   host retransmits the identical payload with the same L2 sequence and the retry flag
   set; after two retries with no valid response the transaction is reported as failed and
   no third retry is sent.
3. **Given** a node whose response arrives with a bad CRC, **When** the host sees it,
   **Then** the host retries exactly as for silence (a corrupt response is not a response).
4. **Given** a transaction that succeeded on its first retry, **When** a late duplicate of
   the original response arrives afterwards, **Then** it is discarded and does not affect
   the next transaction.
5. **Given** any two consecutive transactions, **When** the host transmits the second,
   **Then** at least the inter-frame gap of bus idle separates them, and a new (not
   retried) transaction carries a new sequence number.
6. **Given** a response whose source address is not the polled node, or whose destination
   is not the host, **When** it arrives during the response window, **Then** it is
   discarded and the window keeps running.
7. **Given** a request payload larger than the maximum, **When** the host is asked to send
   it, **Then** the request is refused before anything reaches the wire.

---

### User Story 3 - A node answers in its window and replays, never re-executes (Priority: P2)

A trunk node (a backplane, or a virtual backplane in the simulator) must answer only when
polled, must start its answer within the turnaround window, and, when it sees the host's
retry of a request it has already answered, must resend its previous answer rather than
act on the request again.

**Why this priority**: the replay buffer is the other half of retry safety, and the
simulator's virtual backplanes (feature F4) cannot be built without a conforming
node-side engine. It is P2 only because the host side can be tested against a scripted
transport first.

**Independent Test**: drive the node-side engine with a scripted sequence of requests
(including retries of answered sequences, retries of unanswered sequences, and requests
for other nodes) and assert what it transmits, when, and how many times the application
above it is invoked.

**Acceptance Scenarios**:

1. **Given** a request addressed to the node, **When** it is received intact, **Then** the
   node hands the payload to its application exactly once and transmits the application's
   response starting no earlier than the minimum and no later than the maximum turnaround
   time after the request's final stop bit.
2. **Given** the node has answered sequence N, **When** a retry of sequence N arrives,
   **Then** the node retransmits its stored response byte-for-byte and the application is
   not invoked again.
3. **Given** the node has answered sequence N, **When** a request with a different
   sequence arrives, **Then** it is treated as new: the application is invoked and the
   replay buffer now holds the new response.
4. **Given** a frame addressed to a different node, or a corrupt frame, **When** the node
   receives it, **Then** it transmits nothing.
5. **Given** the node has never answered any request, **When** a frame arrives with the
   retry flag set, **Then** it is treated as a new request (there is nothing to replay).

---

### User Story 4 - Node health follows the spec's state machine (Priority: P2)

The host must track each trunk node's health from transaction outcomes alone: three
consecutive failures make a node SUSPECT; a SUSPECT node is polled at a reduced rate; a
node that spends one second SUSPECT without a valid response becomes OFFLINE and the
layer above is told; an OFFLINE node is still probed by the enrolment rotation so it can
come back.

**Why this priority**: health state is what the superframe scheduler (F3) and the audio-
safety logic (L4) consume; without it the simulator cannot show what happens when a
backplane is unplugged.

**Independent Test**: feed the health tracker a sequence of transaction outcomes with
explicit timestamps and assert the state after each, the moments at which a node is due
for polling, and the notifications emitted.

**Acceptance Scenarios**:

1. **Given** an ENROLLED node, **When** two transactions fail and the third succeeds,
   **Then** the node stays ENROLLED and the failure count resets.
2. **Given** an ENROLLED node, **When** three consecutive transactions fail, **Then** the
   node is SUSPECT and is due for polling only once per ten superframe periods.
3. **Given** a SUSPECT node, **When** one valid response arrives, **Then** the node is
   ENROLLED again with a zero failure count.
4. **Given** a SUSPECT node, **When** one second passes with no valid response, **Then**
   the node is OFFLINE and a notification names it; a node SUSPECT for less than one
   second is not OFFLINE.
5. **Given** an OFFLINE node, **When** the enrolment rotation asks which addresses to
   probe, **Then** the node's address is included; **When** it answers a probe, **Then**
   it is ENROLLED again and a notification reports its return.
6. **Given** the timing values in the definition file, **When** any of them is changed
   there, **Then** the tests that pin the thresholds fail — the values are read from the
   generated symbols, never restated.

---

### User Story 5 - A dead bus is not ten dead nodes (Priority: P3)

When every node stops answering at once, the host must conclude the trunk itself has
failed, not that every node died independently: it declares a bus fault, re-probes at the
fallback bit rate, and raises a system alert, so a wrong bit rate or a broken cable is
distinguishable from a single unplugged backplane.

**Why this priority**: bring-up and field diagnosis depend on this distinction, but it
builds entirely on Story 4 and is exercised least often.

**Independent Test**: with several enrolled nodes tracked, script simultaneous failure of
all of them and assert the bus-fault declaration, the alert, the switch to the fallback
rate for re-probing, and the recovery behaviour; script failure of a strict subset and
assert no bus fault.

**Acceptance Scenarios**:

1. **Given** several ENROLLED nodes, **When** all of them fail per the bus-fault rule
   (FR-028), **Then** the host declares BUS_FAULT once, raises one alert, and the next
   probes go out at the fallback bit rate.
2. **Given** several ENROLLED nodes, **When** only some of them fail, **Then** those nodes
   follow the Story 4 state machine and no bus fault is declared.
3. **Given** a declared bus fault, **When** probes alternate between the reference and
   fallback rates and a node answers one of them, **Then** the bus fault clears, a
   recovery notification is raised, and the bit rate in use is the one the answer arrived
   at (FR-026).
4. **Given** a single enrolled node, **When** it becomes SUSPECT, **Then** BUS_FAULT is
   declared (one node is all nodes) and the re-probe policy starts; a SUSPECT node in a
   two-node rig whose peer is ENROLLED declares nothing.

---

### Edge Cases

- A FLAG byte immediately after a FLAG byte (empty frame, or back-to-back frames sharing a
  delimiter): the empty frame is discarded silently; the shared delimiter still starts the
  next frame.
- A frame longer than the maximum (len field says ≤ 64 but more bytes arrive before the
  closing FLAG): discarded as bad length; the receiver resynchronises on the next FLAG and
  bounds its buffer so no input can make it grow.
- An escape byte as the last byte before a FLAG (`0x7D 0x7E`): the frame is discarded; the
  FLAG still opens the next frame.
- A corrupted frame whose corruption happens to yield a valid CRC (probability ~1/65536 for
  random damage): CRC cannot detect it; the torture corpus generator excludes such cases by
  construction (it checks the corrupted bytes do not form a valid frame), and the
  specification claims zero false accepts only for frames whose CRC is invalid.
- Reserved ctrl bits (2–3) set on a received frame: accepted and ignored (forward
  compatibility, matching the L3 rule for unknown fields); never set on transmit.
- `dst = 0xFF` (reserved broadcast): never transmitted by the host in v1; a received frame
  with it is discarded.
- A response that arrives exactly at the response timeout: the timeout is exclusive — a
  start bit at or after `T_resp` is late. Tests place a response at `T_resp − 1 µs`
  (accepted) and at `T_resp` (missed).
- A node answering before the minimum turnaround: the host accepts any response whose start
  bit falls before the timeout (the minimum is a responder obligation enforced on the node
  side and asserted there); a node answering after the maximum turnaround but before the
  timeout is still a valid response to the host — the spec's turnaround window is a
  conformance requirement on nodes, its timeout is the host's rule.
- Sequence number wrap (4 bits, 0–15): the sixteenth new transaction to a node reuses
  sequence 0; a retry of the previous sequence 0 is distinguishable only by the retry
  flag plus strict master-poll ordering — the host never has two transactions to the same
  node in flight.
- A retry arriving at a node after the host has already given up (third response lost):
  the node replays; the host discards the late response (Story 2 scenario 4); nothing is
  applied twice.
- Babble (bytes from a node outside its response window, or a node that never stops
  transmitting): the host discards everything that is not the polled node's frame; the
  transaction in progress fails or succeeds on its own merits; the babbling node's
  health is not adjusted on the host side (attribution is not reliable), and a babble
  script step in the scripted transport lets tests assert both effects.
- A node-side engine polled late (first given control after `T_turn_max` has passed):
  it transmits immediately and counts a late response (FR-014); the host, seeing a start
  bit before `T_resp`, still accepts it — the late poll is the simulator's defect, not
  the protocol's, and the counter is how a scenario notices.
- Simulated time that does not advance (a test forgets to step the clock): the engine
  never spins — every wait is a comparison against the injected clock, and a transaction
  cannot time out until time is advanced past the timeout.
- Node health when the node has never been polled: UNENROLLED, not ENROLLED — a fresh address
  is neither SUSPECT nor OFFLINE nor counted towards a bus fault until it has been enrolled
  by a valid response.

## Requirements *(mandatory)*

### Functional Requirements

**Framing (§4)**

- **FR-001**: The system MUST construct trunk frames exactly as §4 lays them out: FLAG,
  dst, src, ctrl (bit0 response, bit1 retry, bits 2–3 zero, bits 4–7 sequence), len,
  payload (0–64 bytes), CRC-16/CCITT-FALSE over the unstuffed dst..payload bytes in
  little-endian order, FLAG; with 0x7E → 0x7D 0x5E and 0x7D → 0x7D 0x5D applied to every
  byte of dst..crc after the CRC is computed.
- **FR-002**: The system MUST parse frames from a byte stream incrementally (any byte at
  a time, including one), delivering each intact frame's fields and payload and
  discarding, silently, any frame with a bad CRC, a `len` outside 0–64, more payload
  bytes than `len` declares, fewer than the header and CRC require, or an invalid escape
  sequence. **Ruling (human, 2026-08-29; `docs/OPEN-QUESTIONS.md`)**: a single invalid
  escape (0x7D followed by anything but 0x5E/0x5D) aborts the frame at once; §4's "≥ 8
  consecutive stuffing violations" is read as the bound on how much of a babbling stream
  the receiver may consume before abandoning the current frame, not as a tolerance — the
  parser MUST have given up on a frame by the eighth consecutive violation at the latest,
  and in practice gives up at the first.
- **FR-003**: After any discard, the parser MUST resynchronise on the next FLAG byte and
  MUST NOT depend on byte counts, timing, or the content of the discarded bytes to do so.
- **FR-004**: The parser MUST bound its memory to one maximum-size frame plus escape
  overhead, MUST never over-read its input, and MUST NOT crash or hang on arbitrary input
  (constitution Principle III); fuzzing enforces this.
- **FR-005**: The system MUST refuse to construct a frame whose payload exceeds 64 bytes,
  whose `dst` is the reserved broadcast address, or whose reserved ctrl bits are set, and
  MUST refuse before any byte is emitted.
- **FR-006**: Both implementations (host-core and Python reference) MUST produce identical
  bytes for the same frame and MUST accept and reject the same inputs, demonstrated by
  golden vectors (immutable, under `tests/vectors/`) that include the worst-case-stuffing
  frame, the empty-payload frame and the maximum-payload frame, and by a differential run
  over a generated frame corpus and the torture corpus.

**Master transaction engine (§3, §7)**

- **FR-007**: The host-side engine MUST implement one transaction at a time to one node:
  transmit the request, then treat the transaction as answered by the first intact frame
  whose source is the polled node, whose destination is the host, whose sequence matches
  and whose response bit is set, arriving with its start bit before `T_resp` after the
  request's final stop bit.
- **FR-008**: On timeout or on a CRC-failed response the engine MUST retransmit the same
  payload with the same sequence and the retry bit set, up to the number of retries in the
  definition file (2), and MUST then report the transaction failed; it MUST NOT send more
  retries than that under any input.
- **FR-009**: Every new transaction to a node MUST carry the next sequence number for that
  node (4-bit, wrapping); a retry MUST NOT advance it.
- **FR-010**: The engine MUST leave at least `T_gap` of bus idle between the end of one
  transaction (last byte transmitted or received, or the timeout) and the start of the
  next transmission.
- **FR-011**: Frames that are not the expected response (wrong source, wrong destination,
  wrong or stale sequence, response bit clear, duplicates of an already-accepted response,
  frames arriving while no transaction is open) MUST be discarded without changing the
  open transaction's outcome, and MUST be counted (FR-011a) so tests and the simulator can
  observe them.
- **FR-011a**: The link layer MUST keep, per trunk address, a fixed-size counter block —
  transactions started, retries sent, timeouts, CRC-failed responses, frames discarded,
  replays served (node side) — and, per bus, bit-rate changes and bus faults declared;
  counters MUST be readable by the layer above, resettable by it, never allocated after
  initialisation, and MUST be what this feature's own tests assert (e.g. "exactly two
  retries", "one replay served") instead of reaching into engine internals.
- **FR-012**: The engine MUST model transmission time from the frame's stuffed length and
  the bit rate in use (10 bits per byte, 8N1) so that "final stop bit" instants — from
  which turnaround and timeout are measured — are computed, not assumed, and change
  correctly when the fallback bit rate is in use.
- **FR-013**: The engine MUST hand every accepted response's payload to the layer above
  as opaque bytes; it MUST NOT inspect or depend on L3 content (CLAUDE.md architecture
  invariant: L2 is opaque to L3).
- **FR-013a**: The link layer MUST sit below the message-level `OMGPTransport` (Spec §42):
  both engines talk to the wire only through a **byte-wire** interface this feature
  defines — transmit a byte sequence; receive bytes with their arrival instants; report
  and change the bit rate in use — and the trunk implementation of `OMGPTransport` is the
  master engine over that interface. The scripted test transport (FR-030) MUST implement
  the byte-wire interface, and the interface note (SC-010) MUST document it so the
  simulator's virtual wire (F4) can be a second implementation running the same L2 code.

**Node-side engine and replay buffer (§3, §7)**

- **FR-014**: The node-side engine MUST transmit only in response to an intact frame
  addressed to it, and MUST schedule the response's first byte no earlier than
  `T_turn_min` and no later than `T_turn_max` after the request's final stop bit. If the
  engine is not given control until after that window has closed (a late-polling
  simulator), it MUST still transmit — at once — and MUST count the occurrence as a late
  response, so the violation is visible to the layer that caused it rather than hidden
  by a dropped answer.
- **FR-015**: The node-side engine MUST keep exactly one replay buffer holding its most
  recent response (sequence, and the complete frame bytes); on receiving a frame with the
  retry bit set and the same sequence as the buffered response it MUST retransmit the
  buffered frame unchanged and MUST NOT invoke the application.
- **FR-016**: Any request whose sequence differs from the buffered one — retry bit set or
  not — MUST be treated as new: the application is invoked once and its response replaces
  the buffer.
- **FR-017**: The node-side engine MUST ignore frames not addressed to it and corrupt
  frames, transmitting nothing, and MUST never transmit outside a response window.

**Node health (§7)**

- **FR-018**: The health tracker MUST maintain, per trunk address, a state in {UNENROLLED,
  ENROLLED, SUSPECT, OFFLINE}, a consecutive-failure count, and the instant the node
  entered SUSPECT; it MUST be updated solely from transaction outcomes and the injected
  clock.
- **FR-019**: A node MUST become SUSPECT when its consecutive failures reach the definition
  file's threshold (3); any valid response MUST reset the count and return a SUSPECT node
  to ENROLLED.
- **FR-020**: A SUSPECT node MUST be reported as due for polling only once per ten
  superframe periods (`10 × T_poll`, from the generated symbol), measured from the last
  poll of that node; the tracker exposes "is this node due at time t" — the scheduler (F3)
  decides what to send.
- **FR-021**: A node that has been SUSPECT for the definition file's offline threshold
  (1000 ms) without a valid response MUST become OFFLINE, and the tracker MUST emit a
  notification naming the node exactly once per transition.
- **FR-022**: OFFLINE nodes MUST remain in the enrolment rotation: the tracker's list of
  addresses to probe MUST include them; a valid response from an OFFLINE node MUST make it
  ENROLLED and emit a recovery notification.
- **FR-023**: An address that has never produced a valid response is UNENROLLED: it is
  probed by the enrolment rotation, it does not accumulate SUSPECT/OFFLINE state, and it
  does not count towards the bus-fault rule.

**Bus fault (§7)**

- **FR-024**: The host MUST declare BUS_FAULT when every enrolled node (state ENROLLED,
  SUSPECT or OFFLINE — never UNENROLLED) is SUSPECT or worse at the same time. **Ruling
  (human, 2026-08-29; `docs/OPEN-QUESTIONS.md`)**: this is §7's "all nodes fail
  simultaneously"; a single enrolled node counts as all — the fallback re-probe, not the
  node count, is what distinguishes a dead node from a dead bus. BUS_FAULT MUST be
  declared once per episode and raise one system alert.
- **FR-025**: On BUS_FAULT the host MUST re-probe **starting** at the fallback bit rate
  (115.2 kbit/s, from the generated symbol) and thereafter alternate per FR-026, and the
  transport abstraction MUST expose the bit-rate change so a scripted transport can
  observe and react to it.
- **FR-026**: While BUS_FAULT is declared, the host MUST alternate its enrolment probes
  between the reference and the fallback bit rate (one probe at each, in turn); the first
  valid response at either rate MUST clear BUS_FAULT exactly once with a recovery
  notification, and the bit rate that obtained the response MUST become the rate in use
  until the layer above changes it. **Ruling (human, 2026-08-29;
  `docs/OPEN-QUESTIONS.md`)**. Per-node health MUST resume normally afterwards: the
  answering node becomes ENROLLED; the others keep their SUSPECT/OFFLINE state and timers
  (their clocks were not paused by the fault).

**Timing and the clock (§9; CLAUDE.md rules 3–4)**

- **FR-027**: All time in this feature MUST come from an injected monotonic clock with
  microsecond resolution; nothing in `link/` reads a wall clock, sleeps, or busy-waits, and
  tests advance time explicitly (constitution Principle IV).
- **FR-028**: Every timing and limit value MUST be referenced by its generated symbol
  (`T_turn_min/max`, `T_resp`, `T_gap`, `T_poll`, retries, suspect threshold, offline
  threshold, bit rates, flag/escape bytes, max payload); the embedded-path literal check
  MUST pass over `link/`.
- **FR-029**: For every row of the §9 conformance table — bit rate (both rates), `T_turn`
  (both bounds), `T_resp`, `T_gap`, `T_poll`, retries, max payload — at least one test
  MUST assert the behaviour the symbol governs *at its boundary* (e.g. a response at
  `T_resp − 1 µs` accepted and at `T_resp` missed; a 64-byte payload framed and a 65-byte
  one refused; exactly two retries then failure), and each such test MUST name the symbol
  it pins so a reviewer can map table rows to tests.

**Test infrastructure**

- **FR-030**: A scripted transport for tests (`MockWire` in the plan, research R-07; the
  "MockTransport" of the feature description) MUST express, per script step and per node:
  respond normally after a stated delay; stay silent; emit garbage bytes (non-frame bytes,
  a stated count); respond with a CRC-corrupted frame; duplicate a response; babble
  (transmit outside the response window, or continuously for a stated duration); and
  change the bit rate it accepts, so that a probe at the wrong rate looks like silence or
  garbage. Scripts MUST be data (a step table), not per-test code.
- **FR-031**: A seeded, reproducible torture-corpus generator MUST produce byte streams
  of valid frames interleaved with every corruption class (bit flip, byte drop, byte
  insert, truncation, FLAG insertion, invalid escape, garbage burst, frame over length)
  together with the expected delivered frames, excluding by construction any corruption
  that happens to form a valid frame; the same corpus MUST be consumed by both
  implementations.
- **FR-032**: The host-core frame parser MUST be a fuzz target in the existing fuzz
  harness, seeded from the golden vectors, and MUST run inside the CI deep-verify fuzz
  budget with zero findings.
- **FR-033**: Every trunk error, retry and node-health mode named in §7 (response timeout,
  CRC-failed response, SUSPECT, OFFLINE, BUS_FAULT) MUST be expressible through the
  scripted transport and the health tracker's inputs without special-cased code paths,
  so that feature F4 can map scenario YAML onto them (constitution Principle VI).

**Constraints inherited from the repository**

- **FR-034**: The framing codec, both engines and the health tracker live on the embedded
  path (`link/`): no dynamic allocation after initialisation, no exceptions, no RTTI,
  fixed-capacity buffers sized from the generated limits; the ESP32-S3 firmware build
  MUST compile them.
- **FR-035**: New behaviour lands as failing tests first; the diff-scoped mutation gate
  (zero unlabelled survivors on changed lines) and the pipeline's executed-check floor
  apply; source comments cite the trunk section they implement.

### Key Entities

- **Frame**: the unit the trunk carries — destination, source, control (response, retry,
  sequence), length, payload (opaque L3 bytes, 0–64), CRC; exists in two forms, unstuffed
  fields and stuffed wire bytes.
- **Transaction**: one request from the host to one node and the attempt to obtain its
  response: payload, target, sequence, attempts made (0–3), outcome (answered / failed),
  the instants of each transmission and of the response or timeout.
- **Response window**: the interval after a request's final stop bit in which a response
  start bit is valid (before `T_resp`) and in which a conforming node must begin
  (`T_turn_min`..`T_turn_max`).
- **Replay buffer**: a node's single stored response — the sequence it answered and the
  exact frame bytes — replayed on a retry of that sequence.
- **Node health record**: per trunk address — state (UNENROLLED / ENROLLED / SUSPECT /
  OFFLINE), consecutive failures, SUSPECT-since instant, last-poll instant; drives poll
  eligibility and notifications.
- **Bus state**: bit rate in use (reference or fallback), BUS_FAULT flag, and the
  notifications it emits (fault, alert, recovery).
- **Link statistics**: per-address counters (transactions, retries, timeouts, CRC
  failures, discards, replays served) and per-bus counters (bit-rate changes, bus
  faults); fixed size, read-only above, resettable.
- **Clock**: the injected monotonic microsecond time source; the only source of "now".
- **Transport script step**: one scripted behaviour of the test transport for one node —
  kind (respond / silence / garbage / crc-error / duplicate / babble / rate), delay, size or
  duration, count.
- **Torture corpus element**: a generated byte stream, its seed and corruption recipe, and
  the frames a correct receiver must deliver from it.
- **Timing symbols**: the generated constants for every §9 row; never restated in code or
  tests.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Every row of the §9 table (9 values: two bit rates, two turnaround bounds,
  response timeout, inter-frame gap, superframe period, retry count, maximum payload) is
  named by at least one boundary test; a mechanical listing maps each symbol to the test(s)
  that pin it, and a change to any value in the definition file fails at least one test.
- **SC-002**: Across a torture corpus of at least 10,000 frames with at least 1,000
  corruptions of every class, the receiver delivers 100% of intact frames in order and
  accepts 0 corrupted frames (CRC-invalid by construction), in both implementations, with
  zero differential mismatches, in under 60 seconds on the CI runner.
- **SC-003**: The frame parser fuzz target runs the full CI fuzz budget with zero crashes,
  hangs, sanitizer findings or over-reads, and a planted missing bounds check is found
  within that budget (the discriminating check the F1 harness already applies).
- **SC-004**: Retry/replay safety is demonstrated by tests that drop, duplicate, delay and
  corrupt responses in every position (first attempt, first retry, second retry, after
  give-up): in every case the node's application runs exactly once per new sequence, the
  host sends at most 3 transmissions per transaction, and no response is attributed to the
  wrong transaction.
- **SC-005**: The scripted transport expresses all seven behaviours of FR-030 as data
  steps, and every §7 error/health mode is reachable from a script with no test-specific
  code (a table in the test tree maps each mode to the script that produces it).
- **SC-006**: The health state machine's every transition (UNENROLLED→ENROLLED,
  ENROLLED→SUSPECT, SUSPECT→ENROLLED, SUSPECT→OFFLINE, OFFLINE→ENROLLED) and every non-
  transition at the boundary (2 failures, 999 ms SUSPECT) has a test, and each
  notification is emitted exactly once per transition.
- **SC-007**: A bus fault is declared exactly once when the enrolled nodes fail together
  and never when a strict subset fails; the re-probe bit rate is the fallback symbol; the
  fault clears exactly once on recovery.
- **SC-008**: Golden vectors exist for the empty, maximum and worst-case-stuffing frames;
  the worst-case-stuffing vector is exactly **140 bytes** on the wire — the structural
  ceiling, achieved — and its modelled transmission time at the reference rate is
  **1.40 ms** (§4's "≤ ~1.4 ms" met exactly). Trunk §4's layout makes `ctrl` (low
  nibble ≤ 0x3) and `len` (≤ 0x40) incapable of requiring stuffing, so at most 68 of
  the 70 body bytes escape: ceiling 2 + 70 + 68 = 140. 140 is demonstrated by
  execution with the frame pinned in contracts/frame-vectors.md (dst = src = 0x7D,
  response = 1, retry = 1, seq = 11, a recorded mixed 0x7D/0x7E payload whose CRC is
  0x7D7E — both CRC bytes escape); an all-0x7E payload maxes out at 139 (exhaustive
  over all 4 177 920 encoder-legal frames). "Legal" here means encoder-legal (any
  dst ≠ 0xFF) — on a §5-conformant trunk (addresses 0x00–0x0F) the achievable maximum
  is lower still; the vector exercises the codec domain. `kMaxWire = 142`
  (2 + 2 × (4 + 64 + 2)) remains the deliberately conservative buffer-sizing BOUND and
  is unchanged — no wire frame reaches it. (Ruling 2026-08-31, docs/OPEN-QUESTIONS.md;
  an earlier revision claimed "exactly 142 bytes / 1.42 ms", conflating the bound with
  the achievable maximum.)
- **SC-009**: Both builds are green with the new code (native with sanitizers, ESP32-S3
  firmware), the embedded-path scan finds zero forbidden constructs or restated literals
  in `link/`, the diff-scoped mutation run on this feature's pull request reports zero
  unlabelled survivors, and `UNIT_TEST_FLOOR` is raised to the new total.
- **SC-010**: Feature F4 can wire a virtual backplane to the node-side engine and the
  host-core to the host-side engine using only the interfaces this feature documents —
  verified by a written interface note that names every call F4 needs — the byte-wire
  interface (FR-013a), the clock, the notifications and the statistics block (FR-011a)
  — and by the scenario-schema mapping table of SC-005.

## Assumptions

- **Scope boundary**: this feature delivers the link layer as a library plus test
  infrastructure: framing, both engines, health tracking and bus-fault logic, a scripted
  test transport, a torture-corpus generator, golden vectors, a Python reference for the
  frame codec, a fuzz target, and an interface note for F3/F4. It does not decide what to
  poll when (superframe composition, demand slots, enrolment order — F3), does not open a
  UART or any device, and does not implement bridging (§8, F4) — though the node-side
  engine's response-window obligation is what §8's bridge discipline will sit on.
- **Transport layering (clarified 2026-08-29)**: the engines talk to the wire only
  through the byte-wire interface of FR-013a; the message-level `OMGPTransport` of Spec
  §42 sits above the master engine, and the host-core never sees bytes. The scripted test
  transport is the first byte-wire implementation; the simulator's virtual wire (F4) is
  the second, so `VirtualTransport` exercises the real L2 code and §7's faults are
  scenario configuration rather than message-level approximations.
- **Clock**: no injected clock exists in the repository yet; this feature defines the
  minimal monotonic microsecond interface it needs, in a location the plan chooses so that
  `core/` (F3) can share it. Tests drive it explicitly; nothing sleeps.
- **Sequence numbers are per destination node**, incremented per new transaction, wrapping
  mod 16; the retry flag plus strict master-poll ordering disambiguates wrap — §7 does
  not say per-node or global, and per-node is the reading under which a node's single
  replay buffer is sufficient.
- **Node-side replay buffer scope**: one buffer per node, holding the last response
  regardless of requesting sequence — §7's "single-frame replay buffer per node".
- **Turnaround enforcement is asymmetric**: the node-side engine must start its response
  inside `T_turn_min..T_turn_max` (asserted in its tests); the host accepts any response
  whose start bit precedes `T_resp` and does not police the minimum (a UART cannot
  reliably see a too-early start; the simulator can, and the scripted transport lets a
  test express an early responder to show the host still copes).
- **"Valid response" for health purposes** means a frame that passed FR-007's checks for
  the open transaction; a frame that is intact but stale or mis-addressed is neither a
  success nor a failure for anyone's health.
- **Reduced-rate polling counts superframe periods from the node's last poll**, using
  `T_poll` as the period, since the scheduler is not part of this feature; F3 may replace
  the clock-based rule with an actual superframe count if that proves more faithful, but
  the threshold (10) stays a single symbol.
- **Notifications** (SUSPECT, OFFLINE, recovery, BUS_FAULT, alert, bus recovery) are
  delivered to the layer above as values through an interface the plan defines (no
  callbacks that allocate, no exceptions); L4's reaction (muting) is out of scope.
- **Garbage and babble** in the scripted transport are deterministic from the script's
  seed so failures reproduce; babble is attributed to a node in the script only so tests
  can assert the polled node's transaction outcome, not to model host-side attribution.
- **Frame transmission time model**: 10 bits per byte at the bit rate in use, no inter-
  byte gaps, measured over the stuffed frame including both FLAGs; this is the model
  §4's "≤ ~1.4 ms" bound uses and it is what the simulator will use until Rev A measurements
  replace the provisional values. Byte time is computed in **integer microseconds**
  (`10 000 000 / bit_rate`): 10 µs at the reference rate, 86 µs at the fallback rate
  (true value 86.8 µs — about 1 % short over a worst-case frame, well inside the
  `T_turn`/`T_resp` margins); tests pin the integer model, and the function is the single
  place to refine it if Rev A measurements call for sub-microsecond accuracy.
- **Python reference scope**: the frame codec (stuff/unstuff, CRC, incremental parse and
  resync) gets a reference implementation and differential coverage, as constitution
  Principle III requires of every codec; the engines and health tracker are behavioural,
  not codecs, and are verified by the scripted-transport tests rather than a second
  implementation.
- **Pending ruling on §8**: `docs/OPEN-QUESTIONS.md` (2026-08-29) records that §8's
  "holds the trunk response" wording contradicts its own T_resp clause; this feature
  depends only on §3/§7/§9 and is unaffected by how §8 is reworded.
