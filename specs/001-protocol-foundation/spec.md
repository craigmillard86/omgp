# Feature Specification: Protocol Foundation

**Feature Branch**: `001-protocol-foundation`

**Created**: 2026-08-28

**Status**: Draft

**Input**: User description: "Build the OMGP protocol foundation: a code generator that reads protocol/omgp-protocol.yaml and emits a C++17 header of all protocol constants and a Python module of the same constants; C++ encoders/decoders for the L3 message header and every v1 opcode payload defined in docs/protocol-l3.md section 3; a TLV descriptor parser and serializer per section 4, enforcing required records, skip-by-length for unknown types, string length limits and the 2048-byte cap; and an independent Python reference implementation of the same codecs. Success: golden byte vectors for every message type and a complete sample descriptor round-trip in both languages; differential testing shows byte-identical encoding and semantically identical decoding between C++ and Python across a generated corpus; fuzzing the C++ decoders with arbitrary bytes produces clean rejections only; tools/fuzz-smoke.sh runs real libFuzzer targets and tools/mutate.sh supports --diff scoping so the CI deep-verify job bites; codegen output is deterministic (same YAML in, identical files out). Out of scope: any transport, any scheduling, any I/O beyond files."

**Authoritative sources**: `protocol/omgp-protocol.yaml` (constants — constitution Principle I),
`docs/protocol-l3.md` §3 (message model, opcodes, status block, events) and §4 (descriptor
format, record types, limits). Where this specification and those documents disagree, the
documents win and the disagreement is a defect in this specification.

## Clarifications

### Session 2026-08-28

- Q: May this feature add third-party test-only tooling (C++ test framework, mutation
  tool), or must it use tooling already in the repo and CI image? → A: Both approved,
  restricted to open-source tools under an Apache-2.0-compatible licence; specific tools
  chosen in planning; this answer is the human dependency approval (GOVERNANCE.md §1),
  recorded in `docs/OPEN-QUESTIONS.md`.
- Q: Which CRC-16 variant, over which bytes, produces IDENTIFY's `desc_crc`, and does
  this feature compute it? → A: CRC-16/CCITT-FALSE over the entire descriptor blob
  exactly as served by READ_DESC; both implementations expose `descriptor_crc()`; the
  variant is recorded in the YAML (FR-034; ruling in `docs/OPEN-QUESTIONS.md`).
- Q: After FR-008/FR-034 add layouts and the CRC variant to the YAML, does the protocol
  version stay 1.0 or become 1.1? → A: Stays 1.0 — the draft is being completed, not
  extended; minor-version discipline starts at the first tagged protocol release.
- Q: In what form are golden vectors stored so both implementations consume them and a
  human can check them? → A: One JSON file per vector holding the field values, the
  expected bytes as hex, and the spec section exercised; the reference implementation
  reads the JSON directly; host-core tests read a header generated from the same files
  (no JSON parser on the C++ side).

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Protocol constants exist in exactly one place (Priority: P1)

A developer changes a protocol value — adds an opcode, renames an error code, adjusts a limit —
by editing `protocol/omgp-protocol.yaml` only. Running the generator produces the host-core
constants header and the reference-implementation constants module, both carrying every value
from the definition file. Running the generator twice on the same definition file produces
byte-identical output, so the CI drift guard can prove that committed generated artefacts and
the definition file agree.

**Why this priority**: every other story consumes these constants. Constitution Principle I
makes generation the *only* permitted route for protocol values into code; nothing else in
this feature can be built honestly until that route is complete and provably deterministic.

**Independent Test**: delete the generated outputs, run the generator, and assert (a) every
key in every section of the definition file appears in both outputs with the same value,
(b) a second run produces files with identical bytes, (c) a deliberately introduced
non-deterministic ordering is caught by the determinism test.

**Acceptance Scenarios**:

1. **Given** the definition file, **When** the generator runs, **Then** every entry under
   `protocol`, `limits`, `addressing`, `l3_flags`, `opcodes`, `reserved_opcode_ranges`,
   `error_codes`, `node_states`, `events`, `param_kinds`, `tlv`, `module_types`,
   `link_trunk` and `module_bus` is present in both the header and the module, with the
   value the definition file gives it.
2. **Given** generated outputs exist, **When** the generator runs again on an unchanged
   definition file, **Then** the outputs are byte-identical to the previous run (verified by
   hash comparison, not by "looks the same").
3. **Given** the definition file gains a new opcode, **When** the generator runs, **Then**
   both outputs contain the new opcode and nothing else changed except that addition.
4. **Given** the definition file contains an entry with a non-numeric value (e.g.
   `link_trunk.crc: crc16_ccitt_false`), **When** the generator runs, **Then** the value is
   still represented in both outputs in a form that preserves its meaning (today it is
   emitted as a placeholder comment, which loses the value — this must not persist).
5. **Given** the `tlv` section marks a record `required`, `repeated`, or gives it a
   `max_len`, **When** the generator runs, **Then** those attributes are available to code
   in both languages, not only the type number — the descriptor validator (Story 3) reads
   them from generated output rather than duplicating them by hand.

---

### User Story 2 - Encode and decode every v1 message identically in two languages (Priority: P1)

A developer building the host-core, the simulator, or a compliance test constructs an L3
message (header plus a typed payload for one of the v1 opcodes), encodes it to bytes, and
decodes bytes back into typed fields. The host-core implementation and the independent
reference implementation produce the same bytes for the same message, and decode the same
bytes to the same field values. A golden byte vector exists for every message type so a
future regression cannot silently change the wire format.

**Why this priority**: the wire format is the contract between host and modules. Two
implementations that agree on generated corpora (constitution Principle III) is the only
evidence available before hardware exists that the format is unambiguous.

**Independent Test**: for each opcode, build a message from known field values in both
implementations, assert both produce the golden bytes; decode the golden bytes in both,
assert the fields match the original; run the differential corpus and assert zero
mismatches.

**Acceptance Scenarios**:

1. **Given** field values for the common header (opcode, node_id, seq, flags, payload_len)
   and a payload, **When** encoded, **Then** the output is exactly the 5-byte header of
   §3 followed by `payload_len` payload bytes, in that order, with no padding.
2. **Given** a message for each opcode in the definition file (PING, IDENTIFY, READ_DESC,
   SELECT_CHANNEL, SET_BYPASS, SET_PARAM, GET_PARAM, GET_STATUS, GET_EVENT, BP_SLOT_MAP,
   BP_POWER, BP_ROUTE, ERROR) plus the status block of §3.3, **When** encoded by either
   implementation, **Then** the bytes equal that message's golden vector.
3. **Given** a golden vector, **When** decoded by either implementation, **Then** every
   field equals the value it was encoded from, and the two implementations' decoded
   results are equal field-for-field.
4. **Given** a SET_PARAM message, **When** encoded, **Then** the value is a little-endian
   16-bit quantity and the encoder refuses any value above `limits.param_value_max`
   (4095), because SET operations carry absolute values (§3, constitution Principle V)
   and an out-of-range absolute value is a caller error, not something to clamp.
5. **Given** a randomly generated corpus of at least 10,000 valid messages spanning every
   opcode, **When** each is encoded by both implementations and each encoding is decoded
   by both, **Then** encodings are byte-identical and decodings are field-identical, with
   zero exceptions, and the corpus is reproducible from a fixed seed so a failure can be
   replayed.
6. **Given** an ERROR response (opcode 0x7F, flags.error set), **When** decoded, **Then**
   the error code is exposed as a typed value and any optional detail bytes are preserved
   verbatim.

---

### User Story 3 - Parse and build descriptors that survive the future (Priority: P2)

A developer parses a module descriptor (the TLV blob served via READ_DESC) into typed
records and builds one from typed records. The parser enforces every rule in §4: required
records must be present, unknown record types are skipped by their length rather than
rejected, strings respect their declared maximum lengths, and the whole blob never exceeds
2048 bytes. A complete sample descriptor — one exercising every v1 record type including
optional and repeated ones — round-trips byte-identically in both implementations.

**Why this priority**: descriptors are how modules describe themselves to a host that was
written before the module existed. Skip-by-length is named in the protocol document as
*the* forward-compatibility mechanism; getting it right (and provably so) is what lets minor
protocol versions be purely additive.

**Independent Test**: parse the sample descriptor in both implementations and assert
identical typed output; serialise that output and assert the original bytes; then mutate
the sample (drop a required record, insert an unknown type, over-length a string, pad to
2049 bytes) and assert each mutation is rejected or skipped exactly as §4 requires.

**Acceptance Scenarios**:

1. **Given** the sample descriptor bytes, **When** parsed by either implementation,
   **Then** every record in §4.1 present in the sample is returned with its typed fields,
   and both implementations return equal results.
2. **Given** the typed records from scenario 1, **When** serialised by either
   implementation, **Then** the output equals the original sample bytes.
3. **Given** a descriptor containing a record of a type not listed in §4.1 (e.g. 0x55,
   length 7), **When** parsed, **Then** the record is skipped by its length, parsing
   continues, and every subsequent record is still returned — the unknown record is
   neither an error nor silently swallowed without trace (the parser reports how many
   unknown records it skipped).
4. **Given** a descriptor missing any record the definition file marks `required`
   (PROTOCOL, MODULE_TYPE, NAME, MANUFACTURER, MODEL_ID, CHANNEL ≥1, SWITCHING, PARAM ≥1,
   AUDIO, POWER_LV), **When** parsed, **Then** parsing fails with an error that names the
   missing record type.
5. **Given** a NAME or MANUFACTURER string of 25 bytes, or a SERIAL of 17 bytes, **When**
   parsed or serialised, **Then** the operation is rejected with an error naming the record
   and the limit; a string of exactly the limit is accepted.
6. **Given** a descriptor of exactly 2048 bytes, **When** parsed, **Then** it is accepted;
   **Given** one of 2049 bytes, **Then** it is rejected before any record is interpreted.
7. **Given** a record whose declared length runs past the end of the blob, **When**
   parsed, **Then** parsing fails with a truncation error and no byte beyond the blob is
   read.
8. **Given** a non-repeatable record (e.g. PROTOCOL) appearing twice, **When** parsed,
   **Then** parsing fails with a duplicate-record error.

---

### User Story 4 - Hostile bytes are rejected, never survived by accident (Priority: P2)

A maintainer runs the fuzzing harness against every decoder (message header, each opcode
payload, the descriptor parser) with arbitrary input. Every input either decodes cleanly or
is rejected with an error result; no input crashes, hangs, reads outside its buffer, or
trips a sanitizer. The mutation harness can be scoped to only the code a pull request
changed, so the pre-merge deep-verify job runs in bounded time and fails when a change
introduces a bug the tests would not catch.

**Why this priority**: constitution Principle III requires fuzzed parsers, and the CI
`deep-verify` job for T2/T3 pull requests currently invokes two stubs that exit 0
unconditionally — the gate exists but cannot fail. This story makes it bite. It is P2
rather than P1 only because it needs Stories 2–3 to have decoders to attack.

**Independent Test**: run the fuzz harness for its CI budget and assert zero findings; then
plant a known bug (e.g. remove one bounds check) and assert the harness finds it within the
budget; run the mutation harness scoped to a diff and assert it reports killed/survived
counts and exits non-zero when a planted surviving mutant carries no triage label
(ruling 2026-08-29: the gate is per-survivor triage, not a percentage).

**Acceptance Scenarios**:

1. **Given** the fuzz harness and the full CI fuzz budget (600 s as configured in
   `deep-verify`), **When** run against every decoder target, **Then** it reports zero
   crashes, zero timeouts, and zero sanitizer findings, and exits non-zero if any occur.
2. **Given** a decoder with one bounds check deliberately removed, **When** the fuzz
   harness runs, **Then** it reports a finding within the CI budget (evidence that the
   harness reaches the decoder, not merely that it runs).
3. **Given** the mutation harness invoked with `--diff <base>`, **When** run, **Then** it
   mutates only source lines changed relative to `<base>`, reports killed/survived counts,
   and exits non-zero when any survivor on those lines is neither killed nor labelled
   `// mutant-ok(equivalent|accepted): <why>` on its source line.
4. **Given** a pull request that changes no decoder source, **When** deep-verify runs,
   **Then** the mutation stage reports "nothing in scope" and passes in under one minute
   rather than mutating the whole tree.
5. **Given** any decoder and any input, **When** decoding fails, **Then** the failure is an
   error *value* (never an exception, abort, or assertion in embedded-path code) that
   distinguishes at least: truncated input, length field inconsistent with input,
   field value out of range, unknown opcode, missing required record, duplicate record,
   string over limit, blob over cap.

---

### Edge Cases

- `payload_len` larger than the bytes actually supplied → decode fails (truncation),
  never reads past the supplied buffer.
- `payload_len` larger than `limits.max_l3_payload` (64) → decode fails; encode refuses.
- `payload_len` smaller than the opcode's fixed payload size (e.g. SET_PARAM with 3 bytes)
  → payload decode fails with a length-mismatch error.
- Opcode not in the definition file and not in a reserved range → header decodes (the
  header is opcode-agnostic), payload decode reports unknown opcode; the bytes are still
  available verbatim so a caller can forward them.
- Opcode in the reserved firmware-update range 0x60–0x6F → treated as unknown; nothing is
  allocated there by this feature.
- Header flags with reserved bits (2–7) set → decoder preserves them and does not fail
  (forward compatibility); encoder never sets them.
- `node_id` in the reserved range 0x80–0xFF → encoder refuses for requests (spec §2:
  "MUST NOT be used in v1"); decoder accepts and reports the value.
- SET_PARAM `value` above 4095 → encoder refuses; decoder reports out-of-range.
- Descriptor of length 0 → rejected (required records missing), not a crash.
- TLV record with `len` 0 for a type that requires fields (e.g. PROTOCOL) → rejected as a
  malformed record, not skipped.
- TLV record with `len` 255 → accepted if within the blob and the cap.
- Unknown TLV type with `len` extending past the blob → truncation error (skip-by-length
  still respects the boundary).
- Descriptor exactly at the 2048-byte cap → accepted; one byte over → rejected before
  parsing any record.
- Strings containing bytes that are not valid UTF-8 → rejected on parse and on serialise
  (§4: "Strings UTF-8").
- CHANNEL, PARAM, PARAM_ENUM, VENDOR appearing many times → all returned in order; the
  parser does not impose a count limit beyond what the 2048-byte cap implies.
- Definition file with a duplicate opcode number or duplicate TLV type → generator fails
  loudly rather than emitting conflicting constants.
- Generator run on a definition file with keys in a different order → identical output
  (ordering of output is derived from a stable rule, not from source order).

## Requirements *(mandatory)*

### Functional Requirements

**Code generation**

- **FR-001**: The generator MUST read `protocol/omgp-protocol.yaml` and emit one constants
  header for the host-core implementation and one constants module for the reference
  implementation, each containing every value in every section of the definition file.
- **FR-002**: Generator output MUST be deterministic: the same definition file MUST produce
  byte-identical output on every run, on every machine, independent of source key order.
- **FR-003**: The generator MUST preserve non-numeric definition values (names such as
  `crc16_ccitt_false`, `smbus`; attributes such as `idempotent`, `target`, `required`,
  `repeated`, `max_len`, `semantics`, `status`) in both outputs in a usable form; it MUST
  NOT reduce them to placeholder comments.
- **FR-004**: The generator MUST fail with a non-zero exit and a message naming the conflict
  if the definition file contains duplicate opcode numbers, duplicate TLV types, duplicate
  error codes, or a value outside the width its use implies (e.g. an opcode above 0xFF).
- **FR-005**: Protocol values MUST appear in codec code only via the generated outputs; a
  mechanical check MUST fail if a codec source file contains a literal opcode, error code,
  TLV type, flag, limit, or timing value that the definition file also defines
  (constitution Principle I; CLAUDE.md rule 1 and rule 4).

**L3 message codecs**

- **FR-006**: Both implementations MUST encode and decode the common header of §3 (opcode,
  node_id, seq, flags, payload_len; five bytes, in that order).
- **FR-007**: Both implementations MUST provide a payload encoder and decoder for every
  opcode in the definition file: typed (field-level) for every opcode whose layout §3
  defines or FR-008 rules, opaque (FR-009) for the three backplane opcodes; plus the
  status block returned by GET_STATUS (§3.3) and the ERROR payload (error code plus
  optional detail).
- **FR-008**: The response payload layouts for IDENTIFY, READ_DESC, GET_PARAM and
  GET_EVENT are described in §3.1 in prose only. **Ruling (human, 2026-08-28; recorded in
  `docs/OPEN-QUESTIONS.md`)**: adopt the following concrete layouts as provisional, and
  add them to `protocol/omgp-protocol.yaml` and the §3.1 table of `docs/protocol-l3.md`
  in the same change (a T3 slice under GOVERNANCE.md §3, CODEOWNERS-reviewed):
  - IDENTIFY response: u8 major, u8 minor, u8 module_type, u16 desc_len, u16 desc_crc
    (computed per FR-034)
  - READ_DESC response: u16 offset (echo), u8 len, u8[len] bytes
  - GET_PARAM response: u8 param_id, u8 scope, u16 value
  - GET_EVENT response: u8 event_type, u8 remaining_count, u8[] detail
  All multi-byte fields little-endian (§4 convention). Both implementations MUST provide
  typed codecs and golden vectors for these four responses.
- **FR-009**: The request payloads for BP_SLOT_MAP and BP_POWER are described only in
  prose, and BP_ROUTE's format is stated as "TBD". **Ruling (human, 2026-08-28; recorded
  in `docs/OPEN-QUESTIONS.md`)**: opaque passthrough — both implementations MUST encode
  and decode these three opcodes as header plus verbatim payload bytes with no
  field-level validation, so the opcode dispatch path is exercised for every v1 opcode
  and a future field-level codec is a purely additive change. Golden vectors for them
  cover the header plus a sample opaque payload.
- **FR-010**: Encoders MUST refuse to produce a message that violates the protocol: payload
  longer than `limits.max_l3_payload`; SET_PARAM value above `limits.param_value_max`;
  request `node_id` in the reserved range; reserved header flag bits set.
- **FR-011**: Decoders MUST reject, with a typed error value, any input that is truncated,
  whose `payload_len` disagrees with the bytes supplied, whose fixed-layout payload has the
  wrong length, or whose field values are out of the range the protocol defines. Decoders
  MUST NOT read beyond the input they are given.
- **FR-012**: Decoders MUST tolerate what forward compatibility requires: reserved header
  flag bits are preserved not rejected; an unknown opcode yields an "unknown opcode" result
  with the raw payload still available.

**Descriptor codec**

- **FR-013**: Both implementations MUST parse a descriptor blob into typed records and
  serialise typed records into a blob, for every record type in §4.1 (PROTOCOL,
  MODULE_TYPE, NAME, MANUFACTURER, MODEL_ID, SERIAL, CHANNEL, SWITCHING, PARAM,
  PARAM_ENUM, AUDIO, POWER_LV, POWER_TUBE, VENDOR), with multi-byte integers
  little-endian and strings length-delimited UTF-8 without terminator.
- **FR-014**: The parser MUST skip records of unknown type by their declared length,
  continue parsing, and report the count of skipped records; the serialiser MUST be able to
  emit an unknown-type record verbatim so a descriptor with vendor or future records
  round-trips unchanged.
- **FR-015**: The parser MUST reject a descriptor missing any record the definition file
  marks `required`, naming the missing type; for `repeated` required records (CHANNEL,
  PARAM) at least one MUST be present.
- **FR-016**: The parser and serialiser MUST reject a non-`repeated` record that appears
  more than once, naming the type.
- **FR-017**: The parser and serialiser MUST enforce the `max_len` the definition file
  gives NAME (24), MANUFACTURER (24) and SERIAL (16), and MUST enforce that any string
  fits within its record's length byte; a string exactly at its limit is accepted.
- **FR-018**: The parser MUST reject any blob longer than `limits.max_descriptor_bytes`
  (2048) before interpreting any record; the serialiser MUST refuse to produce one.
- **FR-019**: The parser MUST reject a record whose declared length extends past the end of
  the blob, and MUST NOT read past the blob in doing so.
- **FR-020**: Descriptor validation MUST derive `required`, `repeated` and `max_len` from
  the generated outputs (FR-003), not from a second hand-maintained table.
- **FR-034**: Both implementations MUST expose a `descriptor_crc()` that computes
  CRC-16/CCITT-FALSE (the variant the trunk already uses, `link_trunk.crc`) over the
  entire descriptor blob exactly as served by READ_DESC; the variant MUST be recorded in
  the definition file under the descriptor limits (T3 slice with FR-008) rather than
  assumed; the IDENTIFY golden vector's `desc_crc` MUST equal `descriptor_crc()` of the
  sample descriptor, and the existing CRC differential coverage MUST extend to it.

**Reference implementation and differential testing**

- **FR-021**: The reference implementation MUST be written independently of the host-core
  implementation — from the protocol documents, not by transliterating the host-core code —
  so that agreement between them is evidence about the documents, not about one codebase.
- **FR-022**: A differential test MUST generate a reproducible corpus (fixed seed, at least
  10,000 messages spanning every opcode and at least 1,000 descriptors spanning every record
  type including unknown-type records) and assert that both implementations produce
  byte-identical encodings and field-identical decodings for every case; on mismatch it
  MUST print the case bytes and both results so the failure is replayable.
- **FR-023**: The differential test MUST also cover rejection: for a corpus of invalid
  inputs, both implementations MUST reject the same inputs with the same error category.

**Golden vectors**

- **FR-024**: A golden byte vector MUST exist under `tests/vectors/` for every opcode
  (request and, where defined, response), for the status block, for the ERROR payload, and
  for the complete sample descriptor. Each vector is one JSON file containing the field
  values it encodes, the expected bytes as a hex string, and the protocol-document section
  it exercises, so it is human-checkable against the document in a review diff. The
  reference implementation MUST read the JSON files directly; host-core tests MUST consume
  a header generated deterministically from the same files, so no JSON parser enters the
  host-core side (constitution Principle VIII).
- **FR-025**: Golden vectors MUST be generated by the reference implementation and consumed
  by both implementations' tests; they are immutable evidence (CLAUDE.md rule 9) and a test
  MUST NOT be made to pass by editing one.

**Fuzzing and mutation**

- **FR-026**: `tools/fuzz-smoke.sh <seconds>` MUST run a coverage-guided fuzzer against
  every host-core decoder (header, each opcode payload, descriptor) for the given budget,
  under sanitizers, and exit non-zero on any crash, hang, or sanitizer finding.
- **FR-027**: `tools/mutate.sh --diff <base>` MUST restrict mutation to source lines
  changed relative to `<base>`, report killed and surviving mutants, and exit non-zero when
  any survivor on those lines lacks a triage label (`// mutant-ok(equivalent|accepted):
  <one-line justification>`; the allowed count and categories are T3 constants in the
  repository, never relaxed to get green); without `--diff` it MUST report the whole-tree
  kill rate as a trend and MUST NOT gate on it; with no changed lines in scope it MUST
  report that and pass quickly.
- **FR-028**: The CI `deep-verify` job MUST, after this feature, be able to fail: a planted
  decoder bug MUST be caught by fuzzing within the CI budget, and a planted surviving
  mutant MUST be caught by mutation scoping.

**Test tooling (approved dependencies)**

- **FR-032**: This feature MAY introduce one C++ unit-test framework and one mutation-testing
  tool, each open-source under a licence compatible with Apache-2.0 and pinned to an
  exact version. The test framework MUST be vendored so the build works offline
  (CLAUDE.md notes "Catch2 via FetchContent (or vendored for offline builds)"); the
  mutation tool is a CI-only analysis tool installed as a pinned package, and its
  absence locally MUST be disclosed by the mutation harness rather than silently passed
  (FR-027). Neither may become a dependency of host-core code. Their introduction is a T2 "new dependency" change
  (GOVERNANCE.md §3) and MUST land as its own reviewable slice.
- **FR-033**: Adopting a test framework MUST keep the pipeline's unit-test execution floor
  working: the unit stage MUST still obtain a count of executed checks/assertions (the
  `EXECUTED: <n>` contract or an equivalent the framework reports) on both the ctest and
  bootstrap paths, and `UNIT_TEST_FLOOR` MUST be raised accordingly (SC-008).

**Constraints (constitution Principles IV, V)**

- **FR-029**: Host-core codec code MUST be portable C++17 with no dynamic allocation after
  initialisation, no exceptions, no RTTI, no OS or wall-clock dependency; it MUST compile
  in both the native sanitizer build and the ESP32-S3 firmware build.
- **FR-030**: Every SET-class payload MUST carry absolute values (constitution Principle V);
  no codec in this feature may introduce a delta or relative operation.
- **FR-031**: Every codec source file MUST cite the protocol document section it implements
  (constitution Principle IX).

### Key Entities

- **Protocol Definition**: the single machine-readable description of the protocol
  (`protocol/omgp-protocol.yaml`) — sections for limits, addressing, flags, opcodes with
  attributes, error codes, node states, events, parameter kinds, TLV record types with
  required/repeated/max_len attributes, module types, and link/bus timing.
- **Generated Constants**: the two derived artefacts (host-core header, reference module)
  that are the only sanctioned carriers of protocol values into code.
- **L3 Message**: a common 5-byte header plus a payload of up to 64 bytes; requests flow
  host→node, responses node→host with the response flag set and `seq` echoed.
- **Opcode Payload**: the typed content of a message for one opcode (e.g. SET_PARAM's
  param_id/scope/value); has a fixed layout for most opcodes and an opaque form for those
  the protocol document has not yet defined.
- **Status Block**: the 7-byte structure returned by GET_STATUS (§3.3).
- **Descriptor**: a read-only TLV blob of at most 2048 bytes describing a module;
  composed of Records; identified for caching by (MODEL_ID, CRC-16/CCITT-FALSE of the
  whole blob) per §4.1 and FR-034.
- **TLV Record**: `type`, `len`, `value[len]`; typed per §4.1 when the type is known,
  opaque and skippable when it is not.
- **Golden Vector**: an immutable JSON file pairing field values with the exact bytes
  (hex) they encode to and the spec section exercised; one per message type and one for
  the sample descriptor.
- **Differential Corpus**: a seeded, reproducible set of valid and invalid inputs on which
  both implementations must agree.
- **Fuzz Target**: one decoder entry point exposed to arbitrary-byte input under
  sanitizers.
- **Mutation Report**: killed/surviving counts for mutants within a diff scope, each
  survivor with its triage label (or none — which is what decides pass/fail).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% of opcodes in the protocol definition (13 in v1), the status block, the
  ERROR payload and the sample descriptor have a golden vector; a mechanical check lists
  any opcode without one and fails.
- **SC-002**: Two consecutive generator runs on the same definition file produce outputs
  whose hashes are identical, and the CI drift guard fails within one run when a committed
  generated artefact disagrees with the definition file.
- **SC-003**: The differential corpus (≥10,000 messages, ≥1,000 descriptors, plus an
  invalid-input corpus) completes with zero mismatches in under 2 minutes on the CI native
  runner, so it can stay in the per-commit pipeline rather than nightly.
- **SC-004**: A 600-second fuzz run across all decoder targets reports zero crashes, hangs
  or sanitizer findings; a planted missing bounds check is found within that budget.
- **SC-005**: A diff-scoped mutation run on this feature's own pull request reports its
  kill rate, and a planted surviving mutant causes a non-zero exit; a pull request with no
  decoder changes completes the mutation stage in under 60 seconds.
- **SC-006**: The complete sample descriptor (exercising all 14 record types, at least one
  unknown-type record, and every optional/repeated case) round-trips byte-identically in
  both implementations; a 2048-byte descriptor is accepted and a 2049-byte one rejected.
- **SC-007**: A static scan of host-core codec sources finds zero dynamic-allocation,
  exception or RTTI constructs and zero protocol literals that the definition file also
  defines (FR-005, FR-029).
- **SC-008**: The pipeline's unit-test execution floor (`UNIT_TEST_FLOOR` in
  `pipeline.sh`) is raised to reflect the tests this feature adds, so the gate cannot pass
  on a silently filtered-out suite.
- **SC-009**: A developer with no conversation history can, from the repository alone,
  add a new opcode end-to-end (definition file → generated constants → codec in both
  implementations → golden vector → differential coverage) in one pull request, following
  written instructions that this feature leaves behind.

## Assumptions

- **Scope boundary**: this feature produces file-in/file-out libraries and tools only. No
  transport (no `OMGPTransport`, no VirtualTransport), no scheduler, no superframe timing,
  no node health, no CRC framing (L2 is a separate feature; the existing CRC-16 helper is
  untouched). "Any I/O beyond files" means the tools read/write files and stdio; nothing
  opens a socket, serial port, or device.
- **Required repeated records**: the definition file marks PARAM `required: true,
  repeated: true`; this is read as "at least one PARAM record", matching CHANNEL's
  explicit "✔ (≥1)". A module class with genuinely zero parameters would need a definition
  change; recorded in `docs/OPEN-QUESTIONS.md` as a recommendation, not decided here.
- **Unbounded strings**: CHANNEL name, PARAM name, PARAM_ENUM label and VENDOR opaque data
  have no `max_len` in the definition file; they are bounded only by the record length
  byte (≤255 minus fixed fields) and the 2048-byte cap. If the protocol later assigns
  limits, adding `max_len` to the definition file is sufficient (FR-020).
- **UTF-8 strictness**: strings are validated as well-formed UTF-8 on parse and serialise
  because §4 says "Strings UTF-8"; a module emitting Latin-1 would be rejected. This is
  the conservative reading and is cheap on the embedded path.
- **Referential checks are out of scope**: the descriptor parser validates structure and
  presence, not cross-record semantics (e.g. a PARAM_ENUM naming a param_id with no PARAM
  record, a CHANNEL index gap). Those belong to the host-core's descriptor *acceptance*
  logic in a later feature.
- **Semantically identical decoding** means: for valid input, every decoded field is equal
  across implementations; for invalid input, both reject and the error *category* is the
  same (truncated / length mismatch / out of range / unknown opcode / missing required /
  duplicate / string over limit / blob over cap). Error message text may differ.
- **Fuzzer**: a coverage-guided, sanitizer-integrated fuzzer of the libFuzzer family is
  assumed available in the CI image (the `deep-verify` job already installs `clang`,
  whose compiler-rt ships libFuzzer); it is not a new package.
- **Test framework and mutation tool**: approved by human ruling (Clarifications,
  2026-08-28) subject to FR-032; the specific tools are chosen in planning against the
  criteria there. Neither becomes a runtime dependency of the host-core.
- **Protocol version stays 1.0**: the FR-008/FR-034 definition-file changes complete the
  draft rather than extend a released protocol (human ruling, Clarifications
  2026-08-28). Golden vectors therefore encode `PROTOCOL_MAJOR = 1, PROTOCOL_MINOR = 0`.
  Minor-version discipline (§4.2: additive changes bump minor) begins at the first tagged
  protocol release.
- **Existing artefacts are extended, not replaced**: `tools/codegen.py`,
  `tools/diffcheck.py`, `tools/fuzz-smoke.sh`, `tools/mutate.sh`, `tools/refimpl/` and
  the existing `crc_helper` differential all continue to work, and pipeline stage names
  are preserved because `pipeline.sh` and CI depend on them. Two exit-code contracts
  change deliberately: `tools/fuzz-smoke.sh` exits non-zero when no fuzzer toolchain is
  present (it is only invoked where fuzzing is required), and `tools/mutate.sh
  --require` exits non-zero when the mutation tool is missing.
- **Existing generated outputs are regenerated, not hand-edited**: `build/gen/` is a
  build product; any change to its contents comes from the definition file or the
  generator.
- **Risk tier**: this feature touches `core/`/`link/`-class code, `tools/codegen.py`,
  `pipeline.sh` and `tests/vectors/` — expected T2 for most stories and T3 for the
  golden-vector and definition-file changes, which need CODEOWNERS review. Stories should
  be decomposed so T3 slices are small and human-reviewed, per Definition of Ready.
- **Dependencies**: none on other features. Depends on the constitution (v1.0.0) and the
  three protocol documents now being tracked in the repository (PR #12).
