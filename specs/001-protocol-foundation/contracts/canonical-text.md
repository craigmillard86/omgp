# Contract: canonical text format

One line per message or descriptor; ASCII; tokens separated by single spaces; no
trailing space. Used by `tools/l3_helper` (stdin/stdout), `tools/diffcheck.py`,
`tools/refimpl/canonical.py`, `tests/vectors/*.json` (`canonical` field) and the
generated `omgp_vectors.h`. Both implementations must render identical strings for
identical typed values — string equality **is** the semantic-identity test.

## Scalars

- Named constants render as their YAML name: `op=SET_PARAM`, `err=ERR_BUSY`,
  `evt=CHANNEL_SETTLED`, `mt=TUBE_PREAMP`, `state=READY`, `kind=ENUM`. Unknown values
  render as hex: `op=0x63`.
- Byte-sized ids/flags render as `0x%02X`: `node=0x10 flags=0x00 scope=0xFF`.
- Counters/quantities render as decimal: `seq=3 value=4095 settle_ms=120 uptime_s=77`.
- Byte tails render as contiguous lowercase hex, empty allowed: `detail=` / `detail=0a0b`.
- Strings render as `key="..."` with `"` and `\` escaped by backslash and any byte
  outside 0x20–0x7E rendered as `\xNN` (so non-ASCII UTF-8 is unambiguous and the line
  stays ASCII).

## Messages

```
op=<OPCODE> node=0x.. seq=<n> flags=0x.. [<payload fields in layout order>]
```

Examples:

```
op=PING node=0x01 seq=0 flags=0x00
op=IDENTIFY node=0x10 seq=5 flags=0x01 major=1 minor=0 mt=TUBE_PREAMP desc_len=612 desc_crc=0x4A3F
op=SET_PARAM node=0x10 seq=3 flags=0x00 param_id=1 scope=0xFF value=4095
op=GET_EVENT node=0x10 seq=9 flags=0x01 evt=NONE remaining=0 detail=
op=BP_POWER node=0x02 seq=1 flags=0x00 opaque=01ff00
op=ERROR node=0x10 seq=3 flags=0x03 err=ERR_BUSY detail=
```

`payload_len` is not rendered (derivable). Direction is `flags` bit0.

## Status block

```
state=READY channel=2 bypass=0 fault=0x00 uptime_s=77 pending=1
```

## Descriptors

Records joined by ` | `, in blob order:

```
PROTOCOL major=1 minor=0 | MODULE_TYPE mt=TUBE_PREAMP | NAME s="British Preamp" | MANUFACTURER s="OMGP" | MODEL_ID model=0x0101 hw=0x0002 fw=0x0103 | SERIAL s="BP-0001" | CHANNEL idx=0 s="Clean" | CHANNEL idx=1 s="Crunch" | SWITCHING flags=0x01 settle_ms=120 | PARAM id=1 scope=0xFF kind=CONTINUOUS default=2048 s="Gain" | PARAM_ENUM id=3 idx=0 s="Bright" | AUDIO io=0x03 input_mode=1 in_max=500 out_max=1200 | POWER_LV p15=40 n15=40 p9=0 p5=20 | POWER_TUBE class=2 tubes=2 sections=4 heater_nom=600 heater_max=700 bplus_v=250 bplus_exp=12 bplus_max=20 | VENDOR vendor=0x1234 data=deadbeef | UNKNOWN type=0x55 data=01020304050607
```

## Errors

Exactly `ERR <Status>` (C++ enum spelling), e.g. `ERR Truncated`. Nothing else on the
line, so a mismatch in *category* is a plain string mismatch.

## `tools/l3_helper` protocol

```
stdin  : one request per line
  ENC <canonical message>        → stdout: hex bytes           | ERR <Status>
  DEC <hex>                      → stdout: canonical message   | ERR <Status>
  DENC <canonical descriptor>    → stdout: hex                 | ERR <Status>
  DDEC <hex>                     → stdout: canonical descriptor| ERR <Status>
  DVAL <hex>                     → stdout: OK skipped=<n> channels=<n> params=<n> | ERR <Status> type=0x.. offset=<n>
  CRC <hex>                      → stdout: 0x%04X
  QUIT                           → exit 0
```

One output line per input line, flushed per line; unknown verb → `ERR BadRequest`
(helper-level, never produced by the codec). Hex input is case-insensitive, spaces
allowed.
