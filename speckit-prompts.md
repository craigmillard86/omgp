# OMGP — Spec Kit prompts for Claude Code

Workflow per Spec Kit current release: `/speckit.constitution` once, then per
feature: `/speckit.specify` → `/speckit.clarify` → `/speckit.plan` →
`/speckit.analyze` → `/speckit.tasks` → `/speckit.implement` → `/speckit.converge`
(repeat implement/converge until Converged).

Before starting: place `CLAUDE.md` at repo root, `protocol/omgp-protocol.yaml`,
and the three documents in `docs/` (spec v0.7, protocol-l3, trunk-link-layer).
The specify prompts below reference them.

---

## 0. Constitution (run once)

```
/speckit.constitution Create the OMGP software constitution from these
non-negotiable principles:

1. Spec-first: docs/protocol-l3.md, docs/trunk-link-layer.md and
   protocol/omgp-protocol.yaml are authoritative. Code conforms to them;
   discrepancies are raised, never silently resolved. Protocol constants,
   opcodes, TLV types and timing symbols exist in code ONLY via generation
   from protocol/omgp-protocol.yaml.
2. TDD is mandatory: every behaviour lands as failing tests before
   implementation. Bug fixes require a reproducing regression scenario
   committed before the fix. Golden byte vectors under tests/vectors/ are
   immutable evidence and are never edited to make tests pass.
3. Dual verification: protocol codecs have an independent Python reference
   implementation; differential tests require C++ and Python to agree on
   generated corpora. Parsers are fuzz-tested: arbitrary bytes must never
   crash, hang, or over-read.
4. Portability and determinism: host-core and link layer are portable C++17
   with no OS, no wall-clock access (all time via injected Clock), no
   dynamic allocation after init, no exceptions/RTTI. Native build with
   ASan/UBSan and arm-none-eabi cross-compile must both pass for every merge.
5. Idempotency invariant: all L3 operations are retry-safe; SET operations
   carry absolute values. Any design that breaks this is rejected.
6. The simulator is a first-class product: the real host-core runs against
   virtual backplanes/modules over VirtualTransport; scenarios are
   data-driven YAML executed by a table-driven runner; every fault mode in
   Spec §44 is expressible as scenario configuration.
7. Bridge discipline: backplane bridging never stalls the trunk — it
   responds within T_resp or returns ERR_BUSY. A scenario enforces this
   permanently.
8. Minimal dependencies: standard library plus explicitly approved
   third-party libraries only (test framework, YAML/JSON parsing in
   host-only code). Nothing networked, nothing licensed incompatibly with
   Apache-2.0.
9. Traceability: code comments cite spec sections; scenario files name the
   requirement they verify; commit messages reference the feature spec.
```

---

## Feature sequence

Run features in this order — each depends on the previous. One feature =
one specify→converge cycle.

### F1 — Protocol codegen + L3 codecs + Python reference

```
/speckit.specify Build the OMGP protocol foundation: a code generator that
reads protocol/omgp-protocol.yaml and emits a C++17 header of all protocol
constants and a Python module of the same constants; C++ encoders/decoders
for the L3 message header and every v1 opcode payload defined in
docs/protocol-l3.md section 3; a TLV descriptor parser and serializer per
section 4, enforcing required records, skip-by-length for unknown types,
string length limits and the 2048-byte cap; and an independent Python
reference implementation of the same codecs. Success: golden byte vectors
for every message type and a complete sample descriptor round-trip in both
languages; differential testing shows byte-identical encoding and
semantically identical decoding between C++ and Python across a generated
corpus; fuzzing the C++ decoders with arbitrary bytes produces clean
rejections only; tools/fuzz-smoke.sh runs real libFuzzer targets and
tools/mutate.sh supports --diff scoping so the CI deep-verify job bites;
codegen output is deterministic (same YAML in, identical
files out). Out of scope: any transport, any scheduling, any I/O beyond
files.
```

```
/speckit.plan C++17, CMake with presets (native with ASan/UBSan; esp32
cross-compile check using arm-none-eabi-gcc, compile-only). Test framework:
Catch2. Codegen: Python 3.11+, Jinja2 templates, output to build/gen/.
Python reference under tools/refimpl/ tested with pytest. Fuzz via
libFuzzer harness (short CI runs, longer local runs). Layout per CLAUDE.md
repo layout. No dynamic allocation after init in the C++ codec path:
caller-provided buffers, expected-style result type for errors. Follow all
CLAUDE.md golden rules.
```

### F2 — Trunk link layer

```
/speckit.specify Build the OMGP trunk link layer per docs/trunk-link-layer.md:
frame construction and parsing with 0x7E delimiting, byte stuffing, CRC-16
CCITT-FALSE, and mid-stream resynchronisation after corruption; a
transmit/receive engine implementing strict master-poll semantics with the
retry rule (same L2 sequence, retry flag, max 2 retries), the single-frame
replay buffer for responders, and node health tracking
(3 consecutive failures to SUSPECT, 1 s in SUSPECT to OFFLINE, reduced-rate
polling of SUSPECT nodes, OFFLINE nodes remain in enrolment rotation); and
bus-fault detection distinguishing a dead node from a dead bus with
fallback-rate re-probe. All timing uses the injected Clock and the
generated timing symbols. Success: every timing symbol in the trunk spec
section 9 table has at least one test asserting it; framing survives a
corrupted-byte torture corpus with correct resynchronisation; retry/replay
proven safe by tests that drop, duplicate and corrupt responses; a scripted
MockTransport can express delay, silence, garbage, CRC error and babble per
step. Out of scope: superframe scheduling policy (F3), real UART.
```

```
/speckit.plan Portable C++17 in link/, depending only on generated protocol
header, Clock interface and byte-buffer views. No dynamic allocation after
init; fixed-capacity frame buffers sized from limits.max_l3_payload plus
worst-case stuffing. MockTransport lives in tests, scripted via a simple
step table. Property tests: stuff/unstuff round-trip, CRC vectors from the
published CCITT-FALSE test vectors, resync from random cut points. Reuse F1
build presets and CI. Follow CLAUDE.md golden rules, especially rules 3-5.
```

### F3 — Host-core: superframe scheduler, discovery, control operations

```
/speckit.specify Build the OMGP host-core control engine: the 2 ms
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
CLI.
```

```
/speckit.plan Portable C++17 in core/, depending only on OMGPTransport,
Clock, and generated protocol header. Fixed-size tables for nodes
(limits.max_nodes) and per-node state; no allocation after init.
Superframe budget accounting in simulated microseconds. Application
callbacks via a narrow observer interface (node up/down, event, fault) —
no std::function in core, function-pointer-plus-context style. Tests: unit
tests with MockTransport from F2; the first end-to-end tests may stub a
minimal in-memory responder, but full rig scenarios belong to F4. Follow
CLAUDE.md golden rules.
```

### F4 — Virtual rig + fault injection + scenario runner

```
/speckit.specify Build the OMGP virtual rig and scenario system: virtual
modules instantiated from the JSON definition format in docs/omgp-spec
section 43 (channels, parameters, power declarations, switching times);
virtual backplane controllers implementing slot management, the module-bus
model, per-slot power state, event queue summaries, and the
bridge-never-stalls rule (respond within T_resp or ERR_BUSY); a
VirtualTransport binding host-core to the rig in simulated time; fault
injection covering every item in spec section 44 as declarative
configuration (heater/B+ overcurrent, overtemperature, bus timeout and
lockup, unexpected removal, invalid descriptor, power budget exceeded,
channel-switch timeout, stuck relay, protocol mismatch); and a YAML
scenario runner that loads a rig config, executes an action script
(attach, detach, set, select, inject, advance-time, expect), and reports
structured pass/fail. Success: the spec section 47 examples run verbatim as
scenarios (unknown-module discovery lifecycle; tube heater budget denial of
the fourth module); every section 44 fault has at least one scenario; a hostile-module
scenario category exists (descriptor lies, event flooding, ERR_BUSY
livelock, oversized TLVs) seeded with at least four attacks; the
scenario schema is documented in tests/scenarios/SCHEMA.md; the runner
exits nonzero with a readable diff on failure; the full suite runs in under
ten seconds. Out of scope: audio modelling, UDP transport, CLI.
```

```
/speckit.plan Host-only C++ in sim/ (full standard library allowed;
exceptions permitted here). JSON via a single-header library
(nlohmann/json), YAML via yaml-cpp or rapidyaml — choose one and record it.
Rig and scenario execution run entirely in simulated time via the shared
Clock. Virtual module behaviour is table-driven from JSON, not subclassed
per module. Ship virtual-modules/ts808.json, dual-drive.json,
british-preamp.json (tube, 3 channels), four-tube-preamp.json, delay.json.
Scenario runner is the deliverable CI entry point:
scenario_runner <dir|file>. Follow CLAUDE.md golden rules, especially
rule 6.
```

### F5 — CLI

```
/speckit.specify Build the omgp CLI per docs/omgp-spec section 40 against a
live rig over a transport: subcommands modules, backplanes, inspect <node>,
channel <node> <name>, set <node> <param> <value>, power, diagnostics, plus
sim subcommands to launch and script the virtual rig for interactive use.
Percent values map to the 0-4095 range; nodes are addressable by
discovered name or id; output is stable, column-aligned, and additionally
available as JSON with --json for tooling. Success: an interactive session
against the virtual rig can discover modules, switch a preamp channel, set
parameters, display power state including a heater-budget denial, and
inject a fault then observe it in diagnostics; every subcommand has a test
driving it against a scripted rig and asserting output; --json output
round-trips through a schema check. Out of scope: preset file management
UI, desktop app, UDP/HIL transport (next feature).
```

```
/speckit.plan Host-only C++ in cli/ linking core + sim + transport. Argument
parsing with a small vendored library or hand-rolled — no heavy framework.
Human output via a table formatter; --json via the same JSON library as F4.
CLI tests run the binary as a subprocess against scenario-scripted rigs.
This feature completes the Spec v0.7 section 53 milestone; note any
protocol document ambiguities discovered en route in docs/OPEN-QUESTIONS.md.
```

---

## Gate usage

- Run `/speckit.clarify` after every specify — these specs are dense and the
  clarifier will surface real ambiguities (answer from the docs, and if the
  docs are silent, record the ruling in docs/OPEN-QUESTIONS.md).
- Run `/speckit.analyze` before tasks on F1 and F4 at minimum (highest
  cross-artifact risk: codegen touching everything; scenario schema touching
  everything after it).
- After `/speckit.implement`, always run `/speckit.converge` and iterate
  until Converged — with the added local definition of done from CLAUDE.md:
  ctest green, scenario suite green, diffcheck green, both build presets
  compile, no new sanitizer findings.
```
