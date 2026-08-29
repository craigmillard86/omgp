# OMGP Trunk Link Layer — RS-485

**Draft 0.1 — companion to OMGP Specification v0.7 and Protocol Draft 0.1 (L3)**
Status: working draft for simulator implementation. Timing values are provisional until measured on Backplane Rev A.

---

## 1. Scope

This document defines L2 for the host trunk only: framing, addressing, media access, integrity, timing and error recovery between the host and backplane controllers (plus optional native control surfaces). The module bus (I2C/SMBus) has its own L2, defined by SMBus conventions plus the mappings in Protocol Draft 0.1 §2/§4.

Design goals, in priority order: deterministic latency, simple implementation on a bare-metal UART ISR, robustness to a misbehaving node, and implementability from this document alone.

## 2. Physical layer (restated from Spec §28)

- RS-485 (TIA/EIA-485), half-duplex, single twisted pair + common ground (trunk nodes share chassis ground; the trunk is not isolated in v1).
- Reference bit rate: **1 Mbit/s**, UART 8N1. All nodes must also support 115.2 kbit/s as a fallback/bring-up rate (strap or configuration; the host probes at 1 Mbit/s first).
- 120 Ω termination at both physical ends of the trunk. Failsafe bias (pull A/B apart, ~560 Ω to rails at one point, host end) so the bus idles as a valid mark.
- Transceivers must be ≥ 1 Mbit/s rated, with driver enable under firmware control. Slew-limited transceivers are recommended below 500 kbit/s only.

## 3. Media access: strict master poll

The host is the only initiator. A node transmits only in the response window immediately following a frame addressed to it. There is no multi-master arbitration, no CSMA, no token.

- **Turnaround**: a polled node must assert its driver and begin its response within **T_turn = 20 µs to 100 µs** of the final stop bit of the request. The host releases its driver within 10 µs of its final stop bit.
- **Response timeout**: if the host sees no start bit within **T_resp = 200 µs**, the request has failed (see §7).
- **Inter-frame gap**: the host leaves ≥ **T_gap = 50 µs** of bus idle between transactions.
- A node must never transmit outside its response window. A node that detects itself transmitting erroneously (readback mismatch on its own driver, where hardware allows) must release the bus and raise a local fault.

## 4. Framing

Byte-stuffed delimited frames (HDLC-style), so receivers can resynchronise mid-stream after any error:

```
0x7E  FLAG (start)
u8    dst        L2 address
u8    src        L2 address
u8    ctrl       bit0: response, bit1: retry, bits 2-3: reserved 0, bits 4-7: L2 sequence
u8    len        payload length 0-64
u8[len] payload  one L3 message (opaque to L2)
u16   crc16      CCITT-FALSE, over dst..payload, little-endian
0x7E  FLAG (end)
```

- **Byte stuffing**: within dst..crc16, `0x7E → 0x7D 0x5E` and `0x7D → 0x7D 0x5D`. Applied after CRC computation; CRC is computed over unstuffed bytes.
- **Max payload 64 bytes**: fits every v1 L3 message; keeps worst-case frame time ≤ ~1.4 ms at 1 Mbit/s including stuffing overhead, bounding poll-cycle jitter.
- A frame with bad CRC, bad length, or ≥ 8 consecutive stuffing violations is discarded silently; resynchronisation is on the next FLAG.

## 5. L2 addressing

L2 addresses are the L3 node IDs of trunk-resident nodes only: `0x00` host, `0x01–0x0F` backplanes, `0x08–0x0F` may alternatively be native control surfaces (allocation within the range is a host configuration matter). Module node IDs never appear as L2 trunk addresses — frames for modules are addressed to their backplane, which bridges by L3 node ID.

Backplane L2 address is set by geographic strap pins or rotary switch on the backplane, not auto-assigned — the trunk must work before any protocol runs.

`dst = 0xFF` is reserved for future broadcast and MUST NOT be used in v1.

## 6. Poll schedule

The host runs a fixed-period superframe. Reference period **T_poll = 2 ms**, giving every backplane a service opportunity 500 times per second.

Each superframe, in order:
1. One **status poll** per enrolled backplane (GET_STATUS or BP_SLOT_MAP alternating), which also collects `event_pending` counts.
2. **Demand slots**: pending L3 traffic (parameter sets, descriptor chunks, event drains), scheduled by the host up to the superframe budget. Preset recall bursts may consume several consecutive superframes' demand slots.
3. One **enrolment probe** per superframe to the next unenrolled address in rotation (PING), so a newly attached backplane is discovered within 15 superframes (~30 ms).

Budget rule: a superframe never exceeds T_poll; unsent demand traffic carries to the next superframe. This makes worst-case control latency calculable: an event on a module is host-visible within (backplane module-poll period) + (≤ 2 × T_poll), independent of load.

## 7. Errors, retries, node health

- **Retry**: on response timeout or CRC-failed response, the host retransmits with the same L2 sequence and `ctrl.retry` set, up to **2 retries**. L3 idempotency (Protocol Draft §3) makes this safe. A node receiving a retry of a sequence it already answered re-sends its previous response (single-frame replay buffer per node).
- **Node suspect**: 3 consecutive failed transactions → node marked SUSPECT; polled once per 10 superframes.
- **Node dead**: 1 s in SUSPECT without a valid response → node marked OFFLINE, reported to L4 (which decides audio-safety consequences, e.g. muting that backplane's paths). OFFLINE nodes stay in the enrolment rotation.
- **Bus health**: if all nodes fail simultaneously, the host declares BUS_FAULT, re-probes at the fallback bit rate, and surfaces a system alert. This distinguishes a dead node from a broken trunk.

## 8. Bridging rules (backplane duty)

- Frames whose L3 node ID belongs to one of the backplane's slots are translated to module-bus transactions; the backplane holds the trunk response until the module answers or its module-bus timeout (5 ms) expires, whichever is sooner — but must always respond on the trunk within T_resp. If the module transaction is still in flight, the backplane answers `ERROR: busy`, and the host retries later; the backplane MUST NOT stall the trunk waiting on I2C.
- The backplane maintains the per-slot event queues' summary (`event_pending`) in its own status block so the host learns of module events from the routine status poll without extra trunk traffic.
- Module-bus supervision (clock-timeout recovery, slot power-cycle) is autonomous backplane behaviour, reported via backplane events, never coordinated over the trunk in real time.

## 9. Conformance timing table (provisional)

| Symbol | Value | Meaning |
|---|---|---|
| Bit rate | 1 Mbit/s (115.2 kbit/s fallback) | UART 8N1 |
| T_turn | 20–100 µs | request end → response start |
| T_resp | 200 µs | host timeout awaiting start bit |
| T_gap | ≥ 50 µs | idle between transactions |
| T_poll | 2 ms | superframe period |
| Retries | 2 | per transaction |
| Max payload | 64 B | one L3 message |

## 10. Open questions

1. Should native control surfaces get a guaranteed low-latency slot in every superframe (encoder feel), or is demand scheduling sufficient? Measure knob-to-parameter latency in simulation with a virtual surface.
2. Is a 64-byte payload enough for efficient descriptor transfer over the trunk, or should trunk-side READ_DESC chunks go to 128 B (frame time vs. fewer round trips)? Simulator can measure full-rig discovery time both ways.
3. Isolation: v1 assumes a common-ground single chassis. Multi-chassis rigs may want an isolated trunk segment — decide whether to provision for it mechanically (connector choice) even if unimplemented.
4. 1 Mbit/s over the intended trunk length with stubs needs signal-integrity confirmation on Rev A before the rate is frozen; the fallback rate exists so bring-up never blocks on this.
