# Feature Specification: Host-Core Control Engine

**Feature Branch**: `003-host-core-engine`

**Created**: 2026-09-20

**Status**: Draft

**Input**: User description: "Build the OMGP host-core control engine: the 2 ms
superframe scheduler per trunk spec section 6 (status polls for enrolled
backplanes, budgeted demand slots with carry-over, one enrolment probe per
superframe in rotation); discovery (IDENTIFY, descriptor read with
chunking, descriptor caching keyed by MODEL_ID plus CRC so unchanged
modules skip the read); node-ID assignment mapping backplane/slot to module
node IDs from BP_SLOT_MAP; the accept-then-settle channel switch flow with
settle timeout taken from the module descriptor and CHANNEL_SETTLED event
handling; parameter set/get; event draining driven by event_pending counts
from status polls; and node lifecycle reporting to an application-facing
callback interface. Success: a simulated cold boot with 3 backplanes and 12
modules reaches a fully discovered rig with a deterministic transcript;
worst-case event latency in simulation is within the calculable bound of
backplane poll period plus two superframes; a preset-recall burst of 40
parameter sets completes without ever starving status polls; descriptor
cache hit skips the read on reinsertion; all of this runs identically fast
in simulated time. Out of scope: preset persistence format, routing policy,
CLI."

**Authoritative sources**: `docs/trunk-link-layer.md` §6 (poll schedule); `docs/protocol-l3.md`
§3.1 (opcodes — IDENTIFY, READ_DESC, SELECT_CHANNEL, SET_PARAM, GET_PARAM, GET_STATUS,
GET_EVENT, BP_SLOT_MAP), §3.2 (channel switching semantics), §3.3 (status block —
`event_pending`), §3.4 (events), §4 (descriptor format — TLV, CRC-16 caching), §2
(node-ID address ranges and `(backplane, slot) → node ID` assignment); `docs/omgp-spec-v0.7.md`
§7 (channel and parameter control), §32 (module discovery descriptor); the completed
`link/` layer (`specs/002-trunk-link-layer/`), whose `Master`, `Responder` and
`HealthTracker` engines and byte-wire interface this feature is built on rather than
re-implementing. Where this specification and those documents disagree, the documents
win and the disagreement is a defect in this specification.

## Clarifications

### Session 2026-09-20

- Q: Within a superframe's demand budget, parameter sets, descriptor-chunk reads and
  event drains all compete for the same leftover time, and the trunk spec does not
  order them — does an event drain need priority so the calculable event-latency bound
  (backplane poll period + two superframes) holds even during a concurrent parameter
  burst? → A: Yes. Event drains are scheduled ahead of parameter sets and
  descriptor-chunk reads within a superframe's demand budget, so the latency bound holds
  unconditionally rather than only when no burst is in flight. See FR-020.
- Q: When the engine invokes the application's callback and the application is slow to
  return, what should the engine assume? → A: The callback interface is non-blocking
  from the scheduler's point of view — deliveries are queued and drained independently
  of how long the application takes to process any one of them. See FR-021.
- Q: Should the engine forward trunk-level health transitions (SUSPECT, OFFLINE,
  RECOVERED, bus-fault/bus-recovered) to the application through the same lifecycle
  callback that reports discovery and slot-based removal, or does "node lifecycle" mean
  presence only? → A: Forward health transitions too. The application would otherwise be
  blind to a node the link layer already knows is unreachable, since nothing removes its
  backplane slot when it merely stops answering. Each health transition is its own
  distinct lifecycle event, separate from presence (slot occupied/not). See FR-017,
  FR-022 and User Story 5.
- Q: When a SET_PARAM or GET_PARAM operation fails — an error response, or the link
  layer's own retry gives up — is that failure reported to the application the same way
  a channel-switch timeout is (FR-013)? → A: Yes. A failed parameter operation is
  reported through the callback interface, identifying the operation and the reason,
  rather than being the one silent failure mode in an otherwise fully-reported system.
  See FR-023.
- Q: How does the application receive a GET_PARAM value — synchronously, or delivered
  later through the callback interface like everything else in this feature? → A:
  Asynchronously, through the callback interface. GET_PARAM returns having queued the
  request; its value (or failure, per the previous clarification) is delivered later,
  tagged so the application can match it to its request — consistent with how every
  other outcome in this feature already arrives, rather than a call that blocks the
  application across an unpredictable number of superframes. See FR-014 and FR-024.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - A newly powered rig discovers itself (Priority: P1)

A developer or integrator powers up a rig — some number of backplanes, each with some
number of modules plugged into its slots — and needs the host to find every one of them
without being told in advance what is attached: enrol each backplane, learn which slots
are occupied, assign each occupied slot's module a node ID, identify it, and read its
descriptor so the host knows its channels, parameters and switching behaviour.

**Why this priority**: nothing else in this feature has anything to control until
discovery has found it. It is the foundation every other story in this feature depends
on, and it is the first thing a real rig does after power-up.

**Independent Test**: script a virtual rig with a fixed topology (a known number of
backplanes, a known number of modules per backplane, each with a known descriptor), run
the control engine against it from a cold start with no prior state, and confirm every
module ends up identified, described and assigned a node ID, with the sequence of
operations that got there recorded as a transcript.

**Acceptance Scenarios**:

1. **Given** a rig of 3 backplanes with 12 modules total across their slots, **When**
   the control engine starts from a cold boot, **Then** every backplane is enrolled,
   every occupied slot's module is assigned a node ID, every module is identified, and
   every module's descriptor is fully read — reaching a state where nothing further is
   discoverable.
2. **Given** the same scripted scenario run twice from the same cold-start conditions,
   **When** each run's sequence of operations is recorded, **Then** the two transcripts
   are identical.
3. **Given** a backplane slot that a slot map reports occupied but whose module never
   answers IDENTIFY, **When** discovery proceeds, **Then** that slot does not block the
   discovery of any other slot or backplane, and the engine keeps attempting it rather
   than abandoning it permanently.
4. **Given** a newly attached backplane on an already-running rig, **When** the next
   enrolment probes reach it, **Then** it and its occupied slots' modules are discovered
   the same way a cold-boot backplane is, without restarting discovery for backplanes
   already enrolled.

---

### User Story 2 - Status polling never stops for parameter traffic (Priority: P1)

An application drives a burst of parameter changes — for example, recalling a preset
that sets many parameters across several modules at once — and needs the rig's health
and status monitoring to keep running at its normal rate throughout, not pause or fall
behind because the parameter traffic is competing for the same schedule.

**Why this priority**: this is the load-bearing guarantee that makes the control engine
safe to build an application on. A scheduler that lets bulk traffic crowd out status
polling would turn every preset recall into a period of degraded node-health visibility.

**Independent Test**: script a rig already fully discovered, submit a burst of parameter
set operations far larger than one superframe's demand budget, and confirm every
enrolled backplane still receives its status poll every superframe throughout the burst.

**Acceptance Scenarios**:

1. **Given** a fully discovered rig, **When** 40 parameter-set operations are submitted
   at once, **Then** every superframe until the burst finishes still issues its status
   poll to every enrolled backplane before any of the burst's parameter sets.
2. **Given** the same burst, **When** it does not fit in one superframe's remaining
   budget after status polls and one enrolment probe, **Then** the unsent portion
   carries over to the next superframe rather than extending the current one past its
   period.
3. **Given** a status poll reports `event_pending` counts for one or more nodes while a
   parameter burst is still in progress, **When** the engine schedules that superframe's
   demand slots, **Then** the pending burst does not prevent every node's event queue
   from eventually being drained inside the bound User Story 4 defines.

---

### User Story 3 - A channel switch completes and the application is told (Priority: P2)

An application asks a module to switch to a different channel and needs to know, within
a bounded time, whether the switch actually completed — not just that the module
acknowledged the request — so it can safely assume the new channel is live (or find out
it is not).

**Why this priority**: channel switching is a core control-surface interaction, but it
depends on discovery (US1) already knowing the module's descriptor (for its declared
settle time) and on the scheduler (US2) to carry the request and its outcome.

**Independent Test**: script a module whose descriptor declares a settle time and whose
simulated switch behaviour either completes within that time or does not, request a
channel switch, and confirm the application is told the outcome at the right time either
way.

**Acceptance Scenarios**:

1. **Given** a discovered module with a declared switching time, **When** the
   application requests a channel switch, **Then** the request is acknowledged
   immediately as accepted, and completion is reported separately once the module's
   `CHANNEL_SETTLED` event is drained.
2. **Given** a channel switch in progress, **When** the module's declared settle time
   elapses with no `CHANNEL_SETTLED` event drained for it, **Then** the application is
   told the switch did not complete within its declared time, rather than being left
   waiting indefinitely.
3. **Given** a channel switch already in progress for a node, **When** another channel
   switch is requested for the same node before the first is resolved, **Then** the
   second request does not corrupt or silently drop the outcome of the first.

---

### User Story 4 - An event on a module reaches the application within a bounded time (Priority: P2)

A module raises an event — a fault, a recovery, a completed channel switch — and the
application needs to learn about it within a time bound it can calculate in advance from
the rig's own configuration, regardless of how busy the schedule is otherwise.

**Why this priority**: a control engine whose event latency depends on unpredictable
scheduling contention is not usable for anything time-sensitive (fault reporting,
channel-switch completion). This depends on discovery (US1) having found the node and
on the scheduler (US2) carrying its traffic.

**Independent Test**: script a node that raises an event at a known simulated instant,
run the schedule forward, and measure the simulated time until the application's
callback receives it against the calculable bound.

**Acceptance Scenarios**:

1. **Given** a node whose status poll reports a nonzero `event_pending` count, **When**
   the next superframes run, **Then** every queued event is drained and delivered to the
   application within (that node's backplane's poll period) + (at most two superframe
   periods).
2. **Given** a node that queues more than one event before it is next polled, **When**
   its events are drained, **Then** they are delivered to the application in the order
   the module queued them.
3. **Given** a node that continues queuing events faster than one drain cycle can empty
   its queue, **When** draining proceeds across multiple superframes, **Then** the queue
   is not abandoned — draining continues until it is empty, honouring the per-superframe
   budget in User Story 2.

---

### User Story 5 - The application always knows which nodes are live (Priority: P3)

An application needs to keep an up-to-date picture of which backplanes and modules are
present, discovered and healthy — not just physically present but actually answering —
without polling the control engine itself: it is told whenever that picture changes,
whether the change is a slot filling or emptying or the trunk's own health tracking
learning a node has stopped answering or has recovered.

**Why this priority**: this makes the discovery (US1) and health information the
control engine already has usable by an application, but nothing else in this feature
depends on it, so it is the lowest-priority independently deliverable slice.

**Independent Test**: script a module being discovered, then removed, then reinserted,
and confirm the application-facing callback reports each transition in order; separately,
script a module that stops answering polls without its slot being reported empty, and
confirm the application is told it has become unreachable and later told when it recovers.

**Acceptance Scenarios**:

1. **Given** a module completing discovery, **When** it is fully identified and
   described, **Then** the application is told it is now live.
2. **Given** a live module whose backplane reports its slot no longer occupied, **When**
   that is learned, **Then** the application is told it is gone.
3. **Given** a module that returns to a slot it previously occupied, **When** it is
   rediscovered, **Then** the application is told it is live again, using the same
   reporting path as its first discovery.
4. **Given** an enrolled module that stops answering its status polls, **When** the link
   layer's own health tracking marks it SUSPECT and then OFFLINE, **Then** the
   application is told of each transition, distinct from a slot-reported removal.
5. **Given** an OFFLINE module that answers a poll again, **When** the link layer marks
   it recovered, **Then** the application is told it is live again.
6. **Given** every enrolled node on a backplane going silent at once, **When** the link
   layer declares a bus fault, **Then** the application is told, distinct from any single
   node's own transition; and told again when the fault clears.

---

### Edge Cases

- What happens when a module's descriptor read is interrupted partway (the module goes
  SUSPECT or OFFLINE mid-chunk)? The partial descriptor must not be cached or treated as
  complete; the read resumes or restarts when the module is next reachable.
- What happens when a module is removed and a different module — or the same module
  with different firmware — is inserted into the same slot? The descriptor cache is
  keyed by MODEL_ID plus descriptor CRC, not by slot, so a changed module is a cache
  miss and its descriptor is read in full regardless of which slot it occupies.
- What happens when a backplane's slot map is momentarily inconsistent across two
  consecutive status polls (a slot reported occupied, then unoccupied, then occupied
  again within a few superframes)? Each transition is reported as its own lifecycle
  event in the order observed; the engine does not suppress or coalesce rapid
  transitions.
- What happens when a preset-recall burst and a channel switch's settle-timeout window
  overlap for the same node? The settle timeout is measured in elapsed simulated time,
  not in superframes consumed by other traffic, so a busy schedule cannot silently
  extend a node's allowance to settle.
- What happens when a node advertises a descriptor CRC that does not match any cached
  entry, and the read comes back with a different CRC than IDENTIFY advertised (a
  changed-in-flight descriptor)? Treated as a fresh, uncached descriptor — not matched
  against any prior cache entry — since the cache key is exactly the pair the module
  itself reported as current.
- What happens when the application-facing callback is invoked for a lifecycle event,
  an event drain, or a channel-switch outcome and the application is slow to return
  control? The callback interface is non-blocking from the scheduler's point of view:
  deliveries are queued and drained independently of how long the application takes to
  process any one of them, so a slow application can never stall the superframe
  scheduler itself (clarified 2026-09-20).
- What happens when a SET_PARAM or GET_PARAM operation fails — the module answers with
  an error (unknown parameter, busy/settling, not permitted), or the link layer's own
  retry gives up without an answer? The failure is reported to the application through
  the callback interface, identifying the operation and the reason, rather than being
  silently dropped (clarified 2026-09-20).

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The control engine MUST run a fixed-period superframe scheduler with a 2 ms
  period (trunk §6, `T_poll`), driving every request the engine makes of the link layer.
- **FR-002**: Each superframe MUST issue one status poll to every enrolled backplane
  before any other traffic in that superframe, and MUST collect that poll's
  `event_pending` count for every node it reports on.
- **FR-003**: Each superframe MUST issue exactly one enrolment probe, to the next
  unenrolled address in rotation, so a newly attached backplane is discovered within 15
  superframes of being enrollable.
- **FR-004**: After a superframe's status polls and enrolment probe, the engine MUST
  schedule pending demand traffic (parameter sets, descriptor-chunk reads, event drains)
  into the superframe's remaining budget.
- **FR-005**: A superframe's total scheduled traffic MUST NOT exceed its period; demand
  traffic that does not fit MUST carry over to a later superframe rather than extending
  the current one.
- **FR-006**: The engine MUST discover a newly enrolled node's identity by issuing
  IDENTIFY, learning its module type, descriptor length and descriptor CRC.
- **FR-007**: The engine MUST read a node's descriptor in chunks via READ_DESC,
  honouring the module's advertised chunk limits, and reassemble the chunks into the
  complete descriptor.
- **FR-008**: The engine MUST cache descriptors keyed by the pair (MODEL_ID, descriptor
  CRC), and MUST skip the descriptor read for a node whose IDENTIFY response's MODEL_ID
  and CRC match an existing cache entry.
- **FR-009**: The engine MUST assign each occupied backplane slot a module node ID,
  derived from the backplane's identity and the slot number, that is stable for as long
  as that slot remains occupied by the same enrolment.
- **FR-010**: The engine MUST release a slot's node-ID assignment when the backplane
  reports that slot no longer occupied, and MUST assign a new node ID when a slot is
  subsequently reported occupied again.
- **FR-011**: The engine MUST treat a SELECT_CHANNEL request as accepted, not complete,
  on receiving its immediate acknowledgement, and MUST treat the corresponding
  `CHANNEL_SETTLED` event, once drained, as the completion of that request.
- **FR-012**: The engine MUST use the requested channel's descriptor-declared switching
  time as the timeout for that channel-switch request.
- **FR-013**: The engine MUST report a channel-switch request that is still outstanding
  when its settle timeout elapses as not completed within that time, rather than leaving
  the application without an answer.
- **FR-014**: The engine MUST provide parameter set and parameter get operations,
  addressed by parameter ID and scope (module-wide or a specific channel), scheduled as
  demand traffic under the superframe budget.
- **FR-023**: The engine MUST report a SET_PARAM or GET_PARAM operation that fails — an
  error response from the module, or exhaustion of the link layer's own retry — to the
  application through the callback interface, identifying the operation and the failure
  reason, rather than dropping it silently (clarified 2026-09-20).
- **FR-024**: A GET_PARAM call MUST return immediately having queued the request; the
  engine MUST deliver the retrieved value, or its failure per FR-023, through the
  callback interface once known, tagged so the application can match the delivery to the
  request that produced it (clarified 2026-09-20).
- **FR-015**: For any node whose most recent status poll reported a nonzero
  `event_pending` count, the engine MUST drain its queued events, continuing across
  superframes under the demand-traffic budget until that node's queue is empty.
- **FR-016**: The engine MUST deliver every drained event to the application through the
  callback interface, in the order the module reported them.
- **FR-017**: The engine MUST report node lifecycle transitions — discovered/live,
  removed, rediscovered — to the application through the callback interface as they are
  learned.
- **FR-022**: The engine MUST report the link layer's node health transitions (SUSPECT,
  OFFLINE, recovered) and bus-fault/bus-recovered notices to the application through the
  same callback interface as other lifecycle transitions, each as its own distinct event
  separate from a slot-reported presence change (clarified 2026-09-20; see User Story 5).
- **FR-018**: All scheduling, timeout and settle-time behaviour MUST be driven by a time
  source the test harness controls, so that a scripted scenario's outcome depends only
  on the number of simulated superframes elapsed and not on how much real time advancing
  them takes.
- **FR-019**: For a given rig topology and scenario script, the engine MUST produce the
  same sequence of operations on every run from the same cold-start conditions.
- **FR-021**: The callback interface MUST be non-blocking from the scheduler's point of
  view: a lifecycle event, a drained event, or a channel-switch outcome is queued for
  delivery and the scheduler proceeds regardless of how long the application takes to
  process a prior delivery (clarified 2026-09-20).
- **FR-020**: Within a superframe's demand-traffic budget, when parameter sets,
  descriptor-chunk reads and event drains are all pending, the engine MUST schedule
  event drains ahead of parameter sets and descriptor-chunk reads, so the latency bound
  in FR-015/SC-002 (event visible within backplane poll period + two superframe periods)
  holds even while a parameter-set or descriptor-chunk burst is competing for the same
  budget (clarified 2026-09-20; no priority order is stated in the authoritative
  sources' description of "demand slots" as one undifferentiated category — see
  `docs/OPEN-QUESTIONS.md`).

### Key Entities

- **Node**: an addressable module or backplane the engine tracks; has a node ID, a
  discovery state (undiscovered, discovering, discovered), and — once discovered — a
  cached descriptor reference.
- **Descriptor**: a module's capability description (identity, channels, parameters,
  switching behaviour) served by the module and cached by the engine, keyed by the pair
  (MODEL_ID, descriptor CRC) so it is shared across every node presenting the same
  identity and content.
- **Backplane Slot Map**: a backplane's report of which of its slots are currently
  occupied; the basis for node-ID assignment and for detecting a module's presence or
  removal.
- **Demand Item**: one unit of pending traffic competing for a superframe's demand
  budget — a parameter set, a parameter get, a descriptor-chunk read, or an event drain.
- **Parameter Request**: an outstanding SET_PARAM or GET_PARAM awaiting its outcome,
  tagged with whatever lets the application match a later callback delivery (a value, or
  a failure per FR-023) back to the request that produced it.
- **Channel-Switch Request**: an accepted SELECT_CHANNEL awaiting either its node's
  `CHANNEL_SETTLED` event or its descriptor-declared settle timeout, whichever comes
  first.
- **Lifecycle Event**: a notification delivered to the application describing a change
  in a node's presence, discovery state, trunk-level health (SUSPECT/OFFLINE/recovered,
  bus-fault/bus-recovered) or channel-switch outcome.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Starting from a rig of 3 backplanes and 12 modules with no prior state,
  the control engine reaches a state where every module is identified, described and
  assigned a node ID, and the recorded sequence of operations to get there is identical
  across repeated runs of the same scripted scenario.
- **SC-002**: The simulated time from a module event occurring to the application's
  callback receiving it never exceeds that module's backplane's status-poll period plus
  two superframe periods, in every scripted scenario exercised, including scenarios with
  concurrent parameter-set traffic.
- **SC-003**: A burst of 40 parameter-set operations submitted at once completes without
  a single superframe, from submission to completion, skipping its status poll to any
  enrolled backplane.
- **SC-004**: When a module with the same MODEL_ID and descriptor CRC as a previously
  seen module reappears (the same physical module reinserted, or a like-for-like
  replacement), its descriptor is not re-read from the module — only its presence and
  node-ID assignment are re-established.
- **SC-005**: The same scripted scenario produces the same sequence of operations and
  the same end state regardless of how quickly the simulated clock is advanced between
  steps.

## Assumptions

- The link layer (`specs/002-trunk-link-layer/`) is complete and provides the Master,
  Responder and HealthTracker engines and byte-wire interface this feature schedules
  traffic over; this feature does not re-implement L2 framing, retry or per-node health
  tracking.
- Node-ID assignment for an occupied slot is stable for as long as that slot stays
  continuously enrolled (FR-009/FR-010) and reproducible for a given scripted scenario
  (FR-019) — not necessarily a closed-form arithmetic function of backplane identity and
  slot number alone. Planning (`research.md` R-06) found a real capacity mismatch: the
  module address space (112 addresses, `ADDR_module_min..max`) split evenly across all
  15 possible backplanes gives ~7 addresses each, well under a single reference FX
  backplane's own 8–16 physical positions (Spec §5), so a fixed per-backplane block
  would be too small for a large backplane while wasting space reserved for small ones.
  The assignment algorithm itself (first-fit from a shared free pool, in a canonical
  discovery order) is a planning decision, not a spec-level one.
- The descriptor cache is in-memory for the life of the running control engine; caching
  descriptors across a full host restart is out of scope (preset persistence format is
  explicitly excluded from this feature, and descriptor-cache persistence is the same
  kind of concern).
- At most one channel-switch request is outstanding per node at a time from this
  engine's own point of view; a second request for the same node while the first is
  still outstanding is queued behind it rather than issued concurrently.
- Routing policy (`BP_ROUTE`), the preset persistence format, and the CLI are out of
  scope for this feature, per the feature description; this feature provides the
  callback interface and parameter/channel operations an application or a later CLI
  feature would build on, without building either.
- The rig topology (number of backplanes, number of modules, their descriptors) is
  supplied by a test/simulation scenario for verification purposes; how a real rig's
  topology is authored or validated outside simulation is not this feature's concern.
