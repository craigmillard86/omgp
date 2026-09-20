# Contract: `MockL3Node` scripted L3 responder (`tests/support/`, R-08)

This feature's own message-level test double — not F2's `MockWire` (byte-level; stays in
`link/`'s own tests), and named distinctly from the roadmap's aspirational "`MockTransport`
from F2" so the gap (R-08) is not read back as F2's own artifact by a later reader. Host-only
code (may use the full language); drives `CoreEngine` through its real `link::Master` and
`link::HealthTracker` (R-02) — this double sits one level below `Master`, standing in for a
`link::ByteWire` plus a real node's answers, over a `FakeClock` (F2's own `tests/support/
fake_clock.hpp`, reused).

## Step table

```cpp
struct IdentifyStep    { l3::IdentifyResp resp; };
struct DescChunkStep   { const uint8_t* blob; uint16_t blob_len; };  // answers every READ_DESC
                                                                       // against this blob,
                                                                       // honouring the request's
                                                                       // own offset/max_len
struct StatusStep      { l3::StatusBlock status; };
struct EventStep       { l3::GetEventResp resp; };  // one queued event; NONE once exhausted
struct ChannelStep     { bool settles; uint64_t settle_delay_us; };  // accepts immediately;
                                                                       // CHANNEL_SETTLED after
                                                                       // settle_delay_us, or
                                                                       // never if !settles
struct ParamStep       { bool ok; uint16_t value; uint8_t error_code; };
struct SlotMapStep     { uint8_t slot_count; uint32_t occupied_bits; uint32_t changed_bits; };
struct SilenceStep     {};   // node/backplane does not answer at all (timeout)
struct ErrorStep       { uint8_t code; };  // an explicit ERROR response
```

One script per simulated trunk address (backplane or, via its owning backplane's bridging,
one of its slots); steps consumed in order per opcode class, mirroring `MockWire`'s own
per-node array-in-order convention. An exhausted script answers the next request with
whatever the last step of its kind specified (steady-state, not silence-by-default — a
scripted rig is expected to stay in its last configured state, not go quiet, once a test has
finished driving its interesting transitions).

## Scheduling

Backed by a trivial in-memory `link::ByteWire` (loopback-style: `transmit()` records the
frame and schedules the scripted answer's bytes at the computed start instants exactly as
`MockWire` does, `receive()` returns whatever is due) — this file does not reimplement byte
stuffing or CRC; it reuses `link::encode_frame`/`Deframer` the same way `MockWire` does,
so a corrupted-on-purpose scripted answer is still expressed as real wire bytes, never as a
shortcut that skips framing. Time is explicit (`advance_to(t)`, no sleeping), matching
`research.md` R-11 and CLAUDE.md rule 3.

## What this double is for, and what it is not

Sufficient for `CoreEngine`'s own unit tests: discovery of one backplane with a handful of
slots, descriptor caching (same `ModelIdRec`+CRC scripted twice), a channel switch that settles
or times out, event draining, parameter set/get success and failure, and — the plan input's
"first end-to-end tests may stub a minimal in-memory responder" — one small scripted rig
exercised superframe-by-superframe end to end. **Not** a substitute for F4's virtual rig:
no fault injection beyond `SilenceStep`/`ErrorStep`, no real backplane bridging model, no
multi-node scale test, no scenario YAML. Full rig scenarios stay F4's, per the plan input and
`speckit-prompts.md`'s own feature boundary.
