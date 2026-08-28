# Contract: Python reference implementation (`tools/refimpl/`)

Independent implementation written from `docs/protocol-l3.md` and the YAML — **not**
from the C++ source (spec FR-021). Pure standard library + generated
`build/gen/omgp_protocol.py` (imported by path). Python ≥3.10-compatible syntax.

## `omgp_l3.py`

```python
class L3Error(Exception):
    status: str        # exact C++ Status name: "Truncated", "OutOfRange", ...
    detail: str

@dataclass(frozen=True)
class Header: opcode: int; node_id: int; seq: int; flags: int; payload_len: int

def encode_header(h: Header) -> bytes                     # raises L3Error(ReservedViolation|OutOfRange)
def decode_header(b: bytes) -> Header                     # raises L3Error(Truncated|OutOfRange)
def decode_message(b: bytes) -> tuple[Header, bytes]      # + LengthMismatch

# one frozen dataclass + encode_/decode_ pair per payload in data-model §2.2:
IdentifyResp, ReadDescReq, ReadDescResp, SelectChannelReq, SetBypassReq, SetParamReq,
GetParamReq, GetParamResp, StatusBlock, GetEventResp, OpaquePayload, ErrorResp
def encode_set_param(p: SetParamReq) -> bytes
def decode_set_param(b: bytes) -> SetParamReq
...
def payload_bounds(opcode: int, direction: str) -> tuple[int, int, bool]   # (min, max, opaque); UnknownOpcode
```

Semantics identical to the C++ contract (same `Status` names, same rules, never clamps).

## `omgp_descriptor.py`

```python
@dataclass(frozen=True) class Record: type: int; value: bytes     # raw
@dataclass(frozen=True) class ProtocolRec / ModuleTypeRec / NameRec / ... / VendorRec / UnknownRec

def iter_records(blob: bytes) -> Iterator[Record]               # raises Truncated
def parse_descriptor(blob: bytes) -> list[TypedRecord]          # BlobTooLarge first; typed decode; UnknownRec kept
def validate_descriptor(blob: bytes) -> Report                  # same Report fields as C++; raises L3Error
def build_descriptor(records: Sequence[TypedRecord]) -> bytes   # enforces cap, max_len, duplicates, utf8
def descriptor_crc(blob: bytes) -> int                          # omgp_crc.crc16_ccitt_false(blob)
```

`parse_descriptor(build_descriptor(recs)) == recs` for any valid `recs` (including
`UnknownRec`) — this is the round-trip property pytest asserts.

## `canonical.py`

```python
def message_to_canonical(h: Header, payload_obj) -> str
def canonical_to_message(line: str) -> tuple[Header, payload_obj]
def descriptor_to_canonical(records: list[TypedRecord]) -> str      # one line per record, joined by " | "
def canonical_to_descriptor(text: str) -> list[TypedRecord]
def error_to_canonical(e: L3Error) -> str                           # "ERR <Status>"
```

Format defined in `canonical-text.md`; this module is the reference renderer that the
generated `omgp_vectors.h` embeds.

## `genvectors.py`

```
python3 tools/refimpl/genvectors.py            # writes tests/vectors/*.json from the curated list
python3 tools/refimpl/genvectors.py --check    # exit 1 if any existing file would change (pipeline)
```

Output is deterministic (`json.dumps(..., indent=2, sort_keys=True)` + trailing newline).

## Tests (`test_*.py`, pytest)

- `test_l3.py`: every payload against its golden vector (bytes and fields), every
  rejection rule (one test per `Status` per codec), header reserved-bit and node-id
  rules.
- `test_descriptor.py`: sample descriptor round-trip, each §4.1 rule (missing required,
  duplicate, max_len, cap 2048/2049, unknown skip + count, truncation, UTF-8), crc.
- `test_canonical.py`: canonical ⇄ typed is a bijection for every vector.
- `test_codegen.py`: determinism (two runs same sha256; shuffled-key YAML same sha256),
  FR-004 conflict detection, attribute tables present, `--check` exit codes.

Pipeline `refimpl` stage becomes `python3 -m pytest -q tools/refimpl` and prints the
collected/passed count (feeds a second floor if desired later).
