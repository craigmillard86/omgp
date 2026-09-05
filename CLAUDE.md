# CLAUDE.md — OMGP host-core, simulator & tooling

## What this repo is

Software tier of the Open Modular Guitar Platform (OMGP): a portable C++
host-core, a virtual rig simulator, protocol code generation, an SDK-facing
protocol library, and a CLI. Hardware does not exist yet — everything must
build, run and be fully testable natively. The same host-core later runs on
ESP32-S3.

Authoritative documents (read before implementing anything in their area):

- `docs/omgp-spec-v0.7.md` — system specification (extracted from the docx)
- `docs/protocol-l3.md` — L3 message model & descriptor format (Draft 0.1)
- `docs/trunk-link-layer.md` — L2 RS-485 trunk spec (Draft 0.1)
- `protocol/omgp-protocol.yaml` — machine-readable protocol definition

When a document and this file disagree, the documents win; flag the conflict
in your summary rather than silently choosing.

## Golden rules (never violate; if a task seems to require it, stop and say so)

1. **`protocol/omgp-protocol.yaml` is the single source of truth** for
   opcodes, error codes, TLV types, flags, limits and timing symbols.
   Generated headers (`build/gen/omgp_protocol.h`, `omgp_protocol.py`) are
   never hand-edited. To change the protocol: edit the YAML, run codegen,
   update the affected `docs/` table in the same commit.
2. **All L3 operations are idempotent.** SET operations are absolute values,
   never deltas. Retries at L2 must always be safe. Any new opcode must keep
   this property or be explicitly rejected in review.
3. **All time flows through the injected `Clock` interface.** No
   `std::chrono::system_clock::now()`, no `sleep()`, no wall-clock reads in
   host-core, link layer, or simulator logic. Tests advance time explicitly.
4. **Timing constants are referenced by their spec symbol** (`T_turn`,
   `T_resp`, `T_gap`, `T_poll`) from the generated header — never inline
   magic numbers.
5. **No dynamic allocation after init in embedded-path code** (`core/`,
   `link/`, `l3/`): fixed-size buffers, no exceptions, no RTTI. Host-only code
   (`sim/`, `cli/`, `tools/`) may use the full language.
6. **The bridge never stalls the trunk.** Any code modelling or implementing
   backplane bridging must respond within `T_resp` — respond `ERR_BUSY`
   rather than wait on the module bus. There is a scenario asserting this;
   it must never be weakened.
7. **Unknown TLV types are skipped by length; unknown event types are
   ignored.** Forward compatibility is load-bearing. Parsers must never
   crash, hang, or over-read on arbitrary bytes (fuzz tests enforce this).
8. **Tests precede implementation.** New behaviour lands as failing tests
   first (unit + a scenario file where observable at rig level). A bug fix
   lands with a scenario reproducing it, committed before the fix.
9. **Golden vectors are immutable evidence.** Never edit files under
   `tests/vectors/` to make a test pass. If a vector is genuinely wrong,
   regenerate it from the Python reference implementation and explain why
   in the commit message.
10. **Both builds must stay green:** native (with ASan/UBSan) and the
    ESP32-S3 firmware build. The ESP32-S3 is Xtensa LX7 — the target build
    uses the ESP-IDF toolchain (pinned via the `espressif/idf` Docker image
    in CI and for local target builds), never a generic ARM toolchain.
    Don't merge with either red.
11. **Label every claim about what a guard, check or change establishes.**
    State whether it is proved by construction, demonstrated by a named
    test, or assumed. Unlabelled property claims are overclaims. For any
    "cannot", "never" or "always", state what makes it true: the
    language/structure, or the current contents of the repo. The latter is
    a control, not a guarantee.

## Build & test

`./pipeline.sh` is the single build/test definition — CI runs it, you run
it, agents run it. Stages: `codegen quality build unit refimpl diffcheck
scenarios selftest esp32` (`fuzz` is optional, clang-only). No cmake
available? The build stage falls back to a bootstrap g++ build with
identical sources and sanitizers; CMake presets stay canonical.

```bash
./pipeline.sh                    # all local stages
./pipeline.sh unit diffcheck     # just these stages
cmake --preset native            # configure (native, sanitizers on)
cmake --build --preset native    # build
ctest --preset native            # all unit + property tests
./build/native/scenario_runner tests/scenarios/          # full scenario suite
./build/native/scenario_runner tests/scenarios/f04-bridge-busy.yaml -v  # one scenario
docker run --rm -v $PWD:/w -w /w/esp32-host espressif/idf:v5.3 \
  bash -c "idf.py set-target esp32s3 && idf.py build"   # target build (pinned toolchain)
python tools/codegen.py          # regenerate protocol headers from YAML
python -m pytest tools/refimpl/  # Python reference implementation tests
python tools/diffcheck.py        # differential: C++ codecs vs Python reference
```

Definition of done for any task: unit tests green, scenario suite green,
diffcheck green, both builds compile, no new sanitizer findings, docs/YAML
updated if the protocol changed.

## Repo layout

```
protocol/         omgp-protocol.yaml + codegen templates
core/             host-core: scheduler, node health, discovery, presets (portable)
link/             trunk L2: framing, stuffing, CRC16, retry/replay (portable)
l3/               L3 message + descriptor codecs (portable; shared by host-core, sim, SDK)
transport/        OMGPTransport interface + Virtual/UDP implementations
sim/              virtual backplanes, virtual modules (JSON-defined), fault injection
cli/              omgp command-line tool
tools/            codegen.py, diffcheck.py, refimpl/ (Python reference codecs)
tests/            unit/, property/, vectors/ (golden), scenarios/ (YAML)
docs/             the three authoritative documents
virtual-modules/  ts808.json, dual-drive.json, british-preamp.json, ...
```

## Architecture invariants

- Ports-and-adapters: `core/` depends only on `OMGPTransport` and `Clock`
  interfaces. It must not include anything from `sim/`, `cli/`, or platform
  headers.
- L2 is opaque to L3 and vice versa: `core/` never sees frame bytes; `link/`
  never interprets payloads.
- The scenario runner drives the *real* host-core against the virtual rig
  over `VirtualTransport`. Scenarios are data (YAML), not code; the runner
  is table-driven. Scenario schema: `tests/scenarios/SCHEMA.md`.
- The virtual rig implements the trunk spec's failure modes (timeout,
  garbage, CRC error, silence, babble) so fault injection is configuration,
  not special code paths.

## Style

- C++17, portable subset. `clang-format` file is authoritative; run it.
- Errors by return value (`expected`-style result type in `core/`/`link/`);
  exceptions only in host-only code.
- Naming: spec vocabulary exactly — `NodeId`, `Superframe`, `Descriptor`,
  `SUSPECT`/`OFFLINE`. Don't invent synonyms for spec concepts.
- Comments cite spec sections (e.g. `// trunk §7: retry with same seq`).

## Operating policy

`docs/GOVERNANCE.md` maps decision rights, gates and risk tiers;
`docs/OPERATING-POLICY.md` governs what agents may do unattended, what
requires a human, and the standing autonomous loops. Read it at session
start. PRs you author are labelled `agent-authored` and `feature:<id>`,
where `<id>` is one of the ids `tools/gh-setup.sh` creates
(`f1-codecs`, `f2-link`, `f3-core`, `f4-simrig`, `f5-cli`; Spec Kit
directory `specs/001-protocol-foundation` ↔ `feature:f1-codecs`) —
delivery telemetry depends on those labels. Tasks are GitHub issues:
apply `in-progress` when you start one, remove it when you pause, commit
with `Refs #n`, and close it via `Closes #n` in the PR — the timing
telemetry reads exactly these signals and nothing else.

## Working agreements for Claude Code sessions

- Prefer small vertical slices: codec + tests + vector in one task, not all
  codecs then all tests.
- When a spec ambiguity blocks you, implement nothing speculative: add the
  question to `docs/OPEN-QUESTIONS.md` with your recommended answer and
  proceed only if a safe default exists. Entries are append-only and dated;
  supersede a prior entry by appending a new one that references it, never
  by editing history.
- Never modify `.specify/` artifacts outside the Spec Kit workflow.
- Mutation testing (`tools/mutate.sh`) runs weekly, not per-commit; if asked
  to improve coverage, kill surviving mutants rather than chasing line %.
- Chain build and test in one invocation (`build && test`) so a failed
  build can never report a stale binary's results. For any manual check
  offered as evidence, state what the output would have been if the claim
  were false; if identical, the evidence supports nothing and must not be
  cited.
