# Phase 0 Research: Host-Core Control Engine

Each decision: **Decision**, **Rationale**, **Alternatives considered**. Numbered R-01..R-12
(R-12 appended after `/speckit-analyze` finding I2), referenced from `plan.md`,
`data-model.md` and `contracts/`.

## R-01: `BP_SLOT_MAP`'s wire format is undefined — this feature must define it

**Context**: `protocol/omgp-protocol.yaml` has `BP_SLOT_MAP: {opaque: true}` — "format not yet
defined... v1 codecs pass it through as opaque bytes" (`docs/protocol-l3.md` §3.1, §6 open
question). This feature's User Story 1 and FR-009/FR-010 require reading occupied slots and
presence changes from exactly this opcode — there is no way to build discovery without a
concrete byte layout.

**Decision**: define `BP_SLOT_MAP`'s response payload now, as part of this feature, since the
prose description already in `docs/protocol-l3.md` §3.1 ("occupied slots, presence changes
since last poll") fully determines the shape — only the byte layout is missing:

```
u8    slot_count                      (number of physical slot positions this backplane has)
bytes occupied  [ceil(slot_count/8)]  (bit i set = slot i physically occupied)
bytes changed   [ceil(slot_count/8)]  (bit i set = slot i's occupancy changed since the
                                        backplane's last BP_SLOT_MAP response)
```

`slot_count` up to 255 covers every backplane class in Spec §5 (reference FX: 8-16; reference
preamp: up to 4) with headroom; both bitmaps together are at most 1 + 32 + 32 = 65 bytes for a
255-slot backplane, comfortably under `LIMIT_max_l3_payload` (64 bytes) for any `slot_count` up
to 248 — flagged as a real bound, not assumed unlimited (see Complexity Tracking in `plan.md`).
`changed` lets the engine trust one poll's delta instead of diffing two full bitmaps itself,
and matches BP_SLOT_MAP's `idempotent: true` flag (re-reading it is always safe — the response
describes current + since-last-poll state, not a queue that drains).

This is a protocol change (`protocol/omgp-protocol.yaml` `l3_payloads.BP_SLOT_MAP`, a
`docs/protocol-l3.md` §3.1 table update, `python tools/codegen.py`, an `l3` codec pair
`encode_bp_slot_map_resp`/`decode_bp_slot_map_resp` and its own golden vector) — CLAUDE.md
golden rule 1 ("edit the YAML, run codegen, update the affected docs table in the same
commit") applies, and it is recorded in `docs/OPEN-QUESTIONS.md` per the working agreement
(recommended default stated above; proceeding on it, ruling pending) rather than resolved
silently in `core/` code. It is `tasks.md`'s first task, ahead of anything that reads it.

**Alternatives considered**: (a) a flat list of occupied slot numbers (`u8 count, u8[count]
slots`) — cannot express "changed" without the host keeping its own previous bitmap and
diffing, which duplicates exactly the information the backplane already has; (b) per-slot TLV
records mirroring the descriptor format — no other opcode in v1 uses TLV outside the
descriptor itself, and a fixed bitmap is simpler for firmware to produce from GPIO presence
lines without new machinery.

## R-02: `core/`'s dependency surface is `Master` + `HealthTracker` + `Clock`, not a literal `OMGPTransport` wrapper class

**Context**: CLAUDE.md's architecture invariants say "`core/` depends only on `OMGPTransport`
and `Clock` interfaces," naming a `transport/` directory holding "OMGPTransport interface +
Virtual/UDP implementations." But `specs/002-trunk-link-layer/contracts/link-cpp.md`'s own
"What F3/F4 need" section — written specifically for this feature — states plainly: "F3
(scheduler): `Master::begin/poll/feed`, `HealthTracker::poll_due/next_probe/on_result/
tick/mark_polled`, `HealthListener`, `Clock`." `link/README.md` independently identifies "The
trunk's `OMGPTransport` (Spec §42) ... the `Master` engine over a [byte-wire]" — i.e. for the
trunk case, `OMGPTransport`'s *role* is realised by `Master` itself, not a separate wrapper
type. `transport/` is empty (`.gitkeep` only); nothing has built a distinct wrapper class
there, and `tests/unit/test_link_loop.cpp` already exercises exactly the pattern link-cpp.md
describes: a real `Master` and `HealthTracker` driven together over one wire.

**Decision — a stated divergence from CLAUDE.md's literal wording, not a silent one**: `core/`
includes `link/master.hpp`, `link/health.hpp`, `link/clock.hpp`, `link/link_types.hpp`
directly, plus `l3/` (message and descriptor codecs) and the generated protocol header.
`core/` does **not** include `link/responder.hpp` (that is the node side — F4's virtual
modules), `link/frame.hpp`, `link/byte_wire.hpp`, or anything from `sim/`, `cli/`, or a
platform header — the ports-and-adapters *intent* (core/ never sees wire bytes, never touches
a platform header, is swappable onto a different wire) is honoured; the swap point the
codebase actually built is `ByteWire` (one layer below `Master`, per F2's own research.md
R-01), not a `Message`-level `OMGPTransport` wrapper one layer above it. `transport/` stays
empty for this feature; nothing here needs it, and F4's `VirtualTransport` is the next thing
that would populate it (per `speckit-prompts.md`'s F4 description, "a `VirtualTransport`
binding host-core to the rig") — bit-for-bit the same `Master`/`HealthTracker`/`Clock` code F3
builds against, over F4's own `ByteWire` implementation instead of a real one.

**Alternatives considered**: (a) build a literal `OMGPTransport` abstract class in `transport/`
now, with `core/` depending only on it — rejected for this feature: it would need to expose
not just send/receive but `HealthListener`-shaped notifications and multiple concurrently
tracked traffic classes (status polls, enrolment probes, demand items), which is not what the
Spec §42 sketch describes ("Production embedded code may use an equivalent C/C++ interface...
architectural, not a requirement to use C++ virtual methods literally") and would duplicate
`Master`+`HealthTracker`'s own public API behind a second layer of indirection for no test or
portability benefit this feature needs — `ByteWire` already is that swap point. (b) Read
CLAUDE.md literally and block on building the wrapper first — rejected as scope creep onto a
feature (`transport/`) nothing has claimed and F2's own contract explicitly didn't need.

## R-03: Backplane health (forwarded) and module liveness (derived) are two different signals

**Context**: `link/health.hpp`'s `HealthTracker` is a fixed 16-entry table keyed by **trunk
address** (`kAddrCount`, `ADDR_backplane_min..max` plus host) — it tracks the 15 possible
*backplanes*, never individual modules. Module addresses (`ADDR_module_min..max`, 112 of them)
never appear as L2 trunk addresses at all (`docs/trunk-link-layer.md` §5: "frames for modules
are addressed to their backplane, which bridges by L3 node ID"). So "SUSPECT, OFFLINE,
RECOVERED, bus-fault/bus-recovered" (spec FR-022, clarified with the user) can only ever be a
literal forward of `HealthTracker`'s own notices for a *backplane* — there is no L2 signal for
an individual module going quiet.

**Decision**: `core/` implements `link::HealthListener` itself and translates its `on_notice`
calls 1:1 into the engine's own lifecycle callback (backplane-addressed events). Separately,
`core/` derives **module**-level liveness purely from its own observations at the L3 level —
consecutive failed or absent answers to a module-addressed request (a status poll, or any
demand item) — using the *same* generated threshold symbols `HealthTracker` uses
(`TRUNK_suspect_after_failures`, `TRUNK_offline_after_suspect_ms`) for consistency of meaning,
but counted independently per module in `core/`'s own node table (CLAUDE.md rule 4: symbols,
not new magic numbers). A module's liveness state also degrades immediately, without waiting
out its own counter, when its backplane itself goes SUSPECT/OFFLINE/BUS_FAULT — a node behind
an unreachable backplane cannot be answering regardless of its own counter — and recovers only
when both its backplane and its own polling say so.

**Alternatives considered**: (a) give `core/` its own address-keyed health table but reuse
`HealthTracker`'s exact state machine as a library over the module ID space too — rejected:
`HealthTracker` is hard-sized to `kAddrCount` (16) and is L2's own type, tied to `next_probe()`
rotation semantics (enrolment rotation, alternating-rate re-probe) that make no sense at the
module layer (modules are never probed directly at L2; the host learns about them only via
messages bridged through their backplane). (b) don't derive module liveness at all, relying
solely on BP_SLOT_MAP presence — rejected: a module can sit in an occupied slot and simply stop
answering (a hung module MCU) without its slot ever reporting empty, which is exactly the gap
the FR-022 clarification exists to close.

## R-04: Callback interface — function-pointer-plus-context, no `std::function`

**Decision**: one struct of plain function pointers plus one opaque context pointer, passed
once at construction (per the user's plan instruction: "no `std::function` in `core/`,
function-pointer-plus-context style" — a portability constraint stricter than `link/`'s own
`HealthListener`, which uses a virtual interface; `core/`'s embedded-path constraints in
CLAUDE.md rule 5 permit virtual dispatch, but this feature's own plan input is more specific
than the baseline and is followed here):

```cpp
struct CoreCallbacks {
    void* ctx;
    void (*on_lifecycle)(void* ctx, LifecycleEvent ev);
    void (*on_param_result)(void* ctx, ParamRequestId id, ParamResult result);
};
```

Every outcome this feature reports — discovery/removal, backplane health forwarded (R-03),
derived module liveness, channel-switch completion/timeout, drained module events, failed
parameter operations — is one `LifecycleEvent` variant (a tagged union / discriminated struct,
not a class hierarchy, matching `link/link_types.hpp`'s own `Notice`-plus-`addr` pattern);
`GetParam`'s asynchronous result (spec FR-024) is the one outcome that also needs a
caller-supplied correlation tag, so it gets its own narrow callback rather than being folded
into `LifecycleEvent`'s node-keyed shape. Two function pointers, not one router function with
a kind tag in the payload, because the two shapes genuinely differ (node-keyed vs.
request-keyed) and a caller only wanting one is not forced to switch on the other's cases.

**Alternatives considered**: (a) one `on_event` callback for everything, `GetParam` results
included, tagged by a `kind` enum — rejected: conflates two independent identities (a node ID,
a request ID) into one payload shape for no benefit, and callers of one almost never want the
other. (b) a virtual `CoreListener` interface like `HealthListener` — rejected per the explicit
plan instruction; also, unlike `HealthListener` (one listener, injected once, link-internal), a
host application is exactly the audience a v-table adds needless coupling for on a
resource-constrained target.

**Correction (`/speckit-analyze`, finding I1): the shape above is not enough on its own.**
Spec FR-021 (clarified with the user) requires delivery to be "queued and drained
independently of how long the application takes to process any one of them" — a slow
`on_lifecycle`/`on_param_result` call must never stall `run_superframe()`. A bare struct of
function pointers, called inline the moment `CoreEngine` decides an outcome occurred, does not
give that guarantee by itself — nothing so far decouples *detecting* an outcome from
*invoking* the callback for it. `CoreEngine` therefore owns two small fixed-capacity rings
(`Ring<LifecycleEvent, LIMIT_max_nodes>` and `Ring<ParamResultDelivery, LIMIT_max_nodes>`,
`ParamResultDelivery` = `{ParamRequestId id; ParamResult result;}`) and a new public method:

```cpp
// Invokes at most `max_deliveries` queued callbacks (drain_callbacks() with the default
// drains everything currently pending). Never called from inside run_superframe() — the
// caller decides when application code runs, exactly as it decides superframe cadence
// (spec FR-018). This is what makes "the scheduler proceeds regardless of how long the
// application takes" true BY CONSTRUCTION: run_superframe() enqueues only, and physically
// cannot invoke a callback itself, so nothing application-side is ever on its call stack.
void drain_callbacks(size_t max_deliveries = SIZE_MAX);
```

A full ring on enqueue refuses the newest item (mirrors R-07's `CoreStatus::QueueFull`
philosophy: fail closed and count it, never silently reorder or evict an older, possibly
more urgent, notice) and increments a `dropped_deliveries` counter `drain_callbacks` cannot
see or un-drop — a real, stated loss under sustained application-side neglect, not hidden.

**Alternatives considered (this correction)**: (a) drain automatically at the end of
`run_superframe()` — rejected: still puts an unbounded number of application callbacks on
`run_superframe()`'s own call stack before it can return, which delays the *caller's* ability
to start the next superframe exactly as much as calling back inline would; the guarantee has
to come from the caller choosing when `drain_callbacks()` runs, not from where inside this
engine the call happens to sit. (b) one unbounded ring, no `QueueFull`-style refusal — rejected:
an application that never calls `drain_callbacks()` would then grow the ring without bound,
which is a heap-free feature's version of an allocation.

## R-05: Node table sizing and indexing

**Decision**: a fixed array of `LIMIT_max_nodes` (128) entries, indexed by `node_id -
ADDR_module_min` (matching the user's plan instruction literally: "Fixed-size tables for nodes
(`limits.max_nodes`)"). Only `ADDR_module_min..ADDR_module_max` (112 of the 128 slots) are
reachable node ids; the remaining 16 index positions are permanently unused headroom rather
than a second, smaller bound to keep in sync with the generated symbol — one constant, one
table size, matching `link/health.cpp`'s own `is_node_addr()` guard-not-modulo pattern (never
alias an out-of-range id onto a real record).

**Alternatives considered**: size the table at exactly 112 (`ADDR_module_max -
ADDR_module_min + 1`) — rejected: two symbols to keep in step (`LIMIT_max_nodes` and the
addressing range) where the plan input names one; the 16 extra entries cost 16 ×
`sizeof(NodeRecord)` of statically-allocated headroom, immaterial against the embedded budget
this feature targets (no dynamic allocation either way).

## R-06: Node-ID assignment — first-fit from a shared free pool, not a fixed per-backplane arithmetic formula

**Context**: `docs/protocol-l3.md` §2 says only "the host maps `(backplane, slot) → node ID`,"
without a formula. The spec's own drafting (`/speckit-specify`) assumed a closed-form function
of backplane identity and slot number; planning found this does not fit the numbers. The
module address space is 112 addresses (`ADDR_module_min..max`) across up to 15 backplanes —
evenly divided, ~7 addresses each — while a single reference FX backplane alone may have 8-16
physical positions (Spec §5). A fixed per-backplane block sized to the worst case (16) fits
only 7 backplanes total; sized to the average (7) it is too small for a single large FX
backplane coexisting with anything else.

**Decision — corrects the spec's drafting-time assumption, noted there**: node IDs are
assigned first-fit from one shared free pool (`ADDR_module_min..ADDR_module_max`), in a
canonical order — ascending backplane address, then ascending slot index within a backplane's
`BP_SLOT_MAP` — whenever a slot is newly observed occupied. An id, once assigned, is stable for
as long as that slot stays continuously occupied (FR-009); freed back to the pool when the slot
empties (FR-010); reproducible for a given scripted scenario (FR-019) because the assignment
order is a pure function of the sequence of BP_SLOT_MAP observations, which a scripted scenario
fixes. Pool exhaustion (more concurrently occupied slots than free ids — the theoretical
worst case above) is reported through the lifecycle callback as a discovery failure for that
slot, not a crash or a wraparound reuse of a live id.

**Alternatives considered**: (a) the original fixed-block formula — rejected, shown above not
to fit the protocol's own stated slot-count range. (b) size blocks per backplane *class*
(FX vs. preamp) from Spec §5's numbers — rejected: nothing in the wire protocol (not even the
new `BP_SLOT_MAP` payload from R-01) identifies a backplane's *class*, only its `slot_count`;
inferring a block size from `slot_count` at discovery time collapses to the free-pool
allocator's own first-fit behaviour, without the fixed-formula's reproducibility benefit
first-fit already provides via canonical ordering.

## R-07: Demand-item scheduling — a small per-kind FIFO, drained event-first

**Decision**: three FIFOs (parameter operations, descriptor-chunk reads, event drains), each a
fixed-capacity ring sized generously against the 40-item burst the spec's SC-003 names (128
entries — `LIMIT_max_nodes`, reused rather than inventing a new bound — is enough for one
pending item per node plus headroom). Each superframe's demand budget is spent by draining the
event FIFO first, then the descriptor-chunk FIFO, then the parameter FIFO, one item at a time,
re-checking the remaining budget after every item, stopping the moment an item would not fit
(FR-005's carry-over) — implementing the spec's clarified priority (FR-020) directly as queue
order, not a weighted scheduler.

**Alternatives considered**: one combined FIFO with a priority field sorted on push — rejected:
three fixed-capacity rings emptied in a fixed order is simpler, allocates nothing, and gives
the same observable order for this feature's three kinds without a per-item comparison.

## R-08: Test double — this feature builds its own message-level `MockTransport`

**Context**: the roadmap (`speckit-prompts.md`, F3's own plan line) says "unit tests with
`MockTransport` from F2." F2, as actually delivered, built `MockWire` — a byte-level scripted
transport for `link/`'s own unit tests (`tests/support/mock_wire.hpp`) — and no message-level
double. There is no `MockTransport` anywhere in the tree.

**Decision — a stated correction to the roadmap's assumption, not a silent gap**: this feature
builds its own `tests/support/mock_l3_node.hpp` (naming it distinctly from the roadmap's
`MockTransport` to avoid implying it is F2's own artifact): a scripted, in-memory node that
answers `Master::begin`/`poll` transactions at the L3-decoded level — script entries are typed
L3 responses (an `IdentifyResp`, a `ReadDescResp` chunk, a `StatusBlock`, an `ErrorResp`, or
silence/corruption), not raw bytes, since `core/` never sees raw bytes either. It is driven
through the *real* `Master` and `HealthTracker` (R-02) over a `FakeClock` and a trivial
in-memory `ByteWire`, exactly mirroring `test_link_loop.cpp`'s own "real engines over
`MockWire`" pattern one layer up — this is also exactly "the first end-to-end tests may stub a
minimal in-memory responder" the plan input names; full rig scenarios (many nodes, fault
injection, the real virtual backplane bridging model) stay F4's, per the plan input and the
original roadmap.

**Alternatives considered**: wait and treat this as a blocking dependency on amending F2 —
rejected: F2 is merged and out of scope to reopen for a test-infrastructure gap; building the
one message-level double this feature itself needs, in its own `tests/support/`, is a smaller
and more honest fix than either blocking or silently building byte-level test doubles that
duplicate `MockWire`.

## R-09: Descriptor-chunk read size

**Decision**: request `max_len = 61` bytes per `READ_DESC` chunk — the largest that fits
`ReadDescResp` (`u16 offset, u8 len, u8[len] bytes` = 3 + len bytes) inside
`LIMIT_max_l3_payload` (64 bytes), matching the existing recorded default in
`docs/OPEN-QUESTIONS.md` ("READ_DESC len ≤ 61") rather than introducing a second one. A full
`LIMIT_max_descriptor_bytes` (2048) descriptor takes at most 34 chunk reads; each is one demand
item (R-07), so a large descriptor may legitimately span several superframes — exactly what
User Story 1's Acceptance Scenario 1 and the "budgeted demand slots with carry-over" scheduling
already account for.

**Alternatives considered**: the module-bus chunk size (`LIMIT_module_bus_chunk` = 28) —
rejected: that bound is the module bus's own SMBus block-size constraint
(`docs/protocol-l3.md` §4: "fits one SMBus block with headers"), explicitly distinct from the
trunk's own larger allowance ("the trunk may use larger chunks") — using the smaller bound
would triple the chunk count for no correctness benefit.

## R-10: `GetParam` correlation tag

**Decision**: `GetParam(node_id, param_id, scope)` returns a caller-visible `ParamRequestId`
(an opaque small integer, assigned by `core/` when the request is queued) immediately; the
later `on_param_result` callback (R-04) carries that same id back. `ParamRequestId` is scoped
to outstanding requests only — reused once its result has been delivered — not a
monotonically increasing global counter, keeping it a `uint8_t` sized against the parameter
FIFO's own fixed capacity (R-07) rather than an unbounded value.

**Alternatives considered**: key by `(node_id, param_id)` instead of an opaque id — rejected:
two outstanding `GetParam` calls for the same `(node_id, param_id)` pair (a legitimate,
if unusual, caller pattern — nothing in this feature's scope forbids it) would be
indistinguishable to the caller on delivery.

## R-11: Superframe budget accounting

**Decision**: the budget is tracked in simulated microseconds (matching the plan input
literally and `Clock`'s own unit), not "items" or "bytes" — `TRUNK_T_poll_us` (2000) per
superframe, debited by each transaction's own worst-case time bound (the same
`byte_time_us(rate) × wire length` reasoning `link/health.cpp`'s `kOutcomeWindowUs` already
uses, at the rate `Master` reports via `bit_rate()`), so a scheduling decision never depends on
how long a transaction *actually* took (which is not knowable in advance) — only on its
worst-case bound, matching FR-005's "never exceed its period" as a guarantee, not a statistical
average.

**Alternatives considered**: a fixed item-count budget (e.g. "N demand items per superframe") —
rejected: item cost varies enormously by kind (an event drain's `GetEventResp` vs. a full
`ReadDescResp` chunk) and by the rate in use (a fallback-rate transaction is ≈8.7× a
reference-rate one, per `docs/trunk-link-layer.md` §7), so a fixed count either wastes budget
at the reference rate or overruns it at the fallback rate.

## R-12: Descriptor cache capacity

**Decision**: 32 entries (`LIMIT_max_nodes` / 4) — generous against realistic rigs with many
identical modules (a 12-module success benchmark, spec SC-001, needs at most 12 distinct
descriptors and typically far fewer), while bounded rather than sized 1:1 with the node table.
A full cache refuses new entries (`data-model.md` §5); every subsequent unique descriptor is
read fresh rather than cached — graceful degradation, not a crash, and not silently unbounded.

**Alternatives considered**: size 1:1 with `LIMIT_max_nodes` (128) — rejected: wastes
`(128 - 32) × LIMIT_max_descriptor_bytes` (up to ~196 KB) of static allocation against a
target this feature has no evidence needs it; the realistic case (many nodes, few distinct
models) is exactly what a smaller, shared cache is for.
