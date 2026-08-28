# OMGP Protocol — Message Model & Descriptor Format

**Draft 0.1 — companion to OMGP Specification v0.7**
Status: working draft for simulator implementation. Everything here is provisional until exercised by the virtual bus and at least one HIL run.

---

## 1. Layering

| Layer | Scope | Defined by |
|---|---|---|
| L4 Application | Presets, routing, power policy, UI generation | Host-core |
| L3 Messages | Transport-independent operations (this document) | This document |
| L2 Link | Framing, addressing, CRC, media access | Trunk link-layer spec (separate doc); SMBus on module bus |
| L1 Physical | RS-485 trunk; I2C module bus | OMGP Spec §28 |

L3 is the portability boundary: host-core, the simulator, the SDK and the compliance suite all speak L3 and never see L2. `OMGPTransport` (Spec §42) carries L3 messages.

## 2. Addressing

- **Node ID**: 8-bit logical address assigned by the host. `0x00` = host, `0x01–0x0F` = backplane controllers, `0x10–0x7F` = modules, `0x80–0xFF` reserved (multicast/broadcast future).
- Module node IDs are assigned at discovery: the backplane reports occupied slots (from SLOT_PRESENT# and SLOT_ID_0–2); the host maps `(backplane, slot)` → node ID. Modules never self-assign.
- On the module bus, the backplane translates node ID ↔ I2C address (`0x20 + slot_id`, avoiding reserved I2C ranges). Modules require no address configuration.

## 3. Message model

Request/response, host-initiated. Modules never transmit unsolicited; asynchronous events are signalled via IRQ# (SMBALERT-style) and collected by the backplane's poll, which forwards them to the host as EVENT responses.

Common header (all messages):

```
u8  opcode
u8  node_id        (target for requests, source for responses)
u8  seq            (request sequence; echoed in response)
u8  flags          (bit0: response, bit1: error, bits 2–7 reserved)
u8  payload_len
u8[payload_len] payload
```

Integrity is an L2 concern (CRC-16 on trunk frames, SMBus PEC on the module bus); L3 assumes delivered messages are intact. Retries and timeouts are L2 policy with L3 idempotency: all operations below are safe to retry (SET operations are absolute, never relative).

### 3.1 Opcodes — v1 core set

| Opcode | Name | Direction | Purpose |
|---|---|---|---|
| 0x01 | PING | H→N | Liveness; response echoes seq |
| 0x02 | IDENTIFY | H→N | Returns protocol version, module type, descriptor length, descriptor CRC-16. Response: `u8 major, u8 minor, u8 module_type, u16 desc_len, u16 desc_crc` |
| 0x03 | READ_DESC | H→N | Chunked descriptor read: payload = offset (u16), max_len (u8). Response: `u16 offset, u8 len, u8[len] bytes` |
| 0x10 | SELECT_CHANNEL | H→M | payload = channel (u8); response when settled or accepted (see 3.2) |
| 0x11 | SET_BYPASS | H→M | payload = 0/1 |
| 0x12 | SET_PARAM | H→M | payload = param_id (u8), scope (u8: module=0xFF or channel), value (u16 LE, 0–4095) |
| 0x13 | GET_PARAM | H→M | payload = param_id, scope. Response: `u8 param_id, u8 scope, u16 value` |
| 0x14 | GET_STATUS | H→N | Returns status block (see 3.3) |
| 0x15 | GET_EVENT | H→N | Drains one queued event; repeat until empty. Response: `u8 event_type, u8 remaining_count, u8[] detail`; `event_type` 0x00 (NONE) when the queue is empty |
| 0x20 | BP_SLOT_MAP | H→B | Backplane only: occupied slots, presence changes since last poll. Payload format not yet defined — v1 codecs pass it through as opaque bytes |
| 0x21 | BP_POWER | H→B | Backplane only: rail enable/disable per slot, current/PG/fault readback. Payload format not yet defined — opaque in v1 codecs |
| 0x22 | BP_ROUTE | H→B | Backplane only: local routing control (format TBD with routing hardware) — opaque in v1 codecs |
| 0x7F | ERROR | N→H | flags.error set; payload = error code (u8) + optional detail |

Error codes (initial): `0x01` unknown opcode, `0x02` bad payload, `0x03` unknown param/channel, `0x04` busy/settling, `0x05` not permitted (e.g. rail not enabled), `0x06` internal fault.

Responses to SELECT_CHANNEL, SET_BYPASS and SET_PARAM carry an empty payload (acceptance); failures arrive as ERROR. The response layouts above were ruled 2026-08-28 (`docs/OPEN-QUESTIONS.md`) and remain provisional until exercised in the simulator; `protocol/omgp-protocol.yaml` `l3_payloads` is the machine-readable form.

### 3.2 Channel switching semantics

SELECT_CHANNEL responds immediately with `accepted`; the module performs its declared mute/switch/settle sequence (Spec §7). Completion is reported as a CHANNEL_SETTLED event. The host uses the descriptor's declared switching time as its timeout. Rationale: keeps the poll loop non-blocking and makes slow relay-based modules and instant DSP modules uniform.

### 3.3 Status block

```
u8  state          (0=init, 1=ready, 2=settling, 3=fault, 4=disabled)
u8  active_channel
u8  bypass
u8  fault_code     (module-defined, 0 = none)
u16 uptime_s
u8  event_pending  (count of queued events)
```

Tube modules extend this with heater/B+ telemetry via read-only parameters, not a special status format.

### 3.4 Events

Queued on the module, drained by GET_EVENT. v1 event types: NONE (0x00 — returned by GET_EVENT when the queue is empty), CHANNEL_SETTLED (0x01), FAULT_RAISED (0x02), FAULT_CLEARED (0x03), PARAM_CHANGED_LOCALLY (0x04, reserved — modules have no local controls in v1, but the type is allocated so a future hardware-control capability does not break the model), USER_DEFINED (0xF0–0xFF).

## 4. Descriptor format

The descriptor is a read-only TLV blob served by the module, read in chunks via READ_DESC. TLV: `u8 type, u8 len, u8[len] value`. Unknown types MUST be skipped by length — this is the forward-compatibility mechanism. Multi-byte integers little-endian. Strings UTF-8, not NUL-terminated (length-delimited).

Maximum descriptor size v1: 2048 bytes. Chunk size ≤ 28 bytes on the module bus (fits one SMBus block with headers); the trunk may use larger chunks.

### 4.1 Record types

| Type | Name | Value | Req |
|---|---|---|---|
| 0x01 | PROTOCOL | u8 major, u8 minor | ✔ |
| 0x02 | MODULE_TYPE | u8 (enum: Spec §4 classes) | ✔ |
| 0x03 | NAME | string ≤ 24 | ✔ |
| 0x04 | MANUFACTURER | string ≤ 24 | ✔ |
| 0x05 | MODEL_ID | u16 vendor-scoped, u16 hw rev, u16 fw rev | ✔ |
| 0x06 | SERIAL | string ≤ 16 | – |
| 0x10 | CHANNEL | u8 index, string name | ✔ (≥1) |
| 0x11 | SWITCHING | u8 flags (bit0 mute-required, bit1 seamless), u16 settle_ms | ✔ |
| 0x20 | PARAM | u8 param_id, u8 scope (0xFF=module, else channel), u8 kind (0=continuous, 1=toggle, 2=enum, 3=momentary, 4=trigger, 5=readonly), u16 default, string name | ✔ per param |
| 0x21 | PARAM_ENUM | u8 param_id, u8 index, string label | for enum params |
| 0x30 | AUDIO | u8 io_flags (mono/stereo in/out), u8 input_mode (0=buffered, 1=PICKUP_SENSITIVE), u16 in_max_mVrms, u16 out_max_mVrms | ✔ |
| 0x40 | POWER_LV | u16 mA per rail: +15, −15, +9, +5 (3V3_STBY implicit) | ✔ |
| 0x41 | POWER_TUBE | u8 power_class (T1–T4), u8 tubes, u8 sections, u16 heater_nom_mA, u16 heater_max_mA, u16 bplus_nom_V, u8 bplus_exp_mA, u8 bplus_max_mA | tube modules |
| 0x7E | VENDOR | u16 vendor id, opaque | – |

The descriptor CRC-16 in IDENTIFY lets the host cache descriptors: same MODEL_ID + CRC = skip the read. This makes reinsertion and power-up fast on the slow module bus. The CRC is CRC-16/CCITT-FALSE (the trunk's `crc16_ccitt_false`) computed over the entire descriptor blob exactly as served by READ_DESC (ruled 2026-08-28).

### 4.2 Versioning policy

- **Major** mismatch: host refuses normal operation, module stays discoverable (IDENTIFY always works) so the host can report "module requires newer host".
- **Minor** ahead of host: host proceeds; unknown TLVs and event types are skipped. Minor versions MUST be purely additive.
- New required behaviour = new major. Expected to be rare; the TLV skip rule carries most evolution.

## 5. What the SDK hides

`module.begin()` owns: I2C peripheral setup, address from SLOT_ID pins, descriptor assembly from `addChannel`/`addParameter` calls, IDENTIFY/READ_DESC service, PEC, event queue, status block, and the mute/settle handshake around the `onChannelChange` callback. A module author touches only the Spec §31 API.

## 6. Open questions (to be settled in the simulator)

1. Does BP_ROUTE belong at L3 or should routing be a normal parameter set on a Routing module class? (Leaning: normal parameters — fewer special cases.)
2. Bulk parameter set for preset recall (one message, many params) — needed for recall speed on the module bus? Measure in simulation first.
3. Firmware update opcodes — explicitly out of scope for draft 0.1; chunked-write design sketch needed before the opcode space fills up. Reserve 0x60–0x6F now.
4. Should GET_EVENT drain multiple events per call? Decide from poll-loop latency in simulation.
5. PARAM name strings inflate descriptors on parameter-heavy DSP modules — is 2048 bytes enough? Check against a worst-case virtual module.
