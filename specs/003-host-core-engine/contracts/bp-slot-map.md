# Contract: `BP_SLOT_MAP` response payload (protocol addition, R-01)

`protocol/omgp-protocol.yaml` and `docs/protocol-l3.md` §3.1 change together in the same
commit (CLAUDE.md golden rule 1); `python tools/codegen.py` regenerates the header;
`docs/OPEN-QUESTIONS.md` gets the recommended-default entry per the working agreement —
ruling pending, proceeding on this default since discovery cannot be built without one.

## Wire format

```
u8    slot_count
bytes occupied [ceil(slot_count/8)]
bytes changed  [ceil(slot_count/8)]
```

Bit *i* of `occupied`/`changed` (LSB of byte `i/8` = slot `i`, matching the existing
`link/`-side bit convention in `BusState::probe_fallback` etc.) corresponds to slot index
*i*, `0 <= i < slot_count`. `changed` bit *i* set means slot *i*'s occupancy differs from the
backplane's own last-reported state (as of the last `BP_SLOT_MAP` this backplane answered,
regardless of who polled it — the backplane's own memory, not the host's). `BP_SLOT_MAP`
stays `idempotent: true`: re-reading it is always safe, since it is current-state-plus-delta,
never a queue that drains on read.

Bits at index `>= slot_count` within the last byte of either bitmap (up to 7 per bitmap) are
reserved: a backplane SHOULD leave them clear, and a host MUST ignore their value on receive
rather than reject it (golden rule 7's forward-compatibility stance; red team, PR #773 round 1,
finding 4 — this line was previously unstated).

## Bound

`LIMIT_bp_slot_map_max_slots` is the largest `slot_count` whose payload
(`1 + 2 × ceil(slot_count/8)` bytes) still fits `LIMIT_max_l3_payload`: **232**, against the
current `LIMIT_max_l3_payload = 59` (`1 + 2×29 = 59`; `slot_count = 233` needs `2×30 = 60`
bytes of bitmap alone, `1 + 60 = 61 > 59`). A backplane advertising `slot_count > 232` is a
protocol violation — refused the same way an oversized descriptor is
(`l3::Status::OutOfRange`), not silently truncated.

*(Originally recommended 248, against the payload limit before it was 64 — F10,
`docs/OPEN-QUESTIONS.md` "#110 rulings", 2026-09-06/2026-09-21 — split into `max_l3_message`
(64, unchanged) and a smaller `max_l3_payload` (59). Recomputed and adopted at 232 in
#676/T005; see `docs/OPEN-QUESTIONS.md` 2026-09-21 "BP_SLOT_MAP wire format (R-01)".)*

## L3 types and codec (`l3_types.hpp`, `l3_payload.hpp`)

```cpp
struct BpSlotMapResp {
    uint8_t slot_count;
    Bytes occupied;  // len == ceil(slot_count/8)
    Bytes changed;   // len == ceil(slot_count/8)
};

Status encode_bp_slot_map_resp(const BpSlotMapResp&, uint8_t* out, size_t cap, size_t& written);
Status decode_bp_slot_map_resp(const uint8_t* in, size_t len, BpSlotMapResp& out);
```

`decode_bp_slot_map_resp`: `Truncated` if fewer than `1 + 2 × ceil(slot_count/8)` bytes;
`LengthMismatch` if more; `OutOfRange` if `slot_count > LIMIT_bp_slot_map_max_slots`. Mirrors
every other payload decoder's rules (`l3_payload.hpp`'s own file header: "BufferTooSmall with
no partial write... no decoder reads past `len`").

## Golden vector

`msg_bp_slot_map_resp_full_occupancy`: `slot_count = 232`, every `occupied` and `changed` bit
set — the largest legal payload, pinning the size boundary the way `frame_worst_stuffing` pins
`kMaxWire` for framing (`specs/002-trunk-link-layer/contracts/frame-vectors.md`).

## Differential coverage

`tools/refimpl/omgp_l3.py`'s reference gets the matching encode/decode pair; `tools/diffcheck.py`
carries it through the existing L3 payload differential path (no `--frames`-style special case
needed — this is an ordinary opcode payload, not a frame).
