# l3/ — OMGP L3 message and descriptor codecs

The wire-format layer of the protocol (`docs/protocol-l3.md` §3–§4): encode and decode the
5-byte common header, every v1 opcode payload, the §3.3 status block, and the §4 TLV
descriptor. Shared by the host-core, the simulator and the module SDK; it knows nothing
about transports, scheduling or time.

API: `specs/001-protocol-foundation/contracts/l3-codec-cpp.md`. Independent Python
reference: `tools/refimpl/` (the golden vectors under `tests/vectors/` are generated from
it and are immutable).

## Portable-subset constraints (CLAUDE.md rule 5) and how each is enforced

| Constraint | Enforced by |
|---|---|
| No exceptions, no RTTI | `-fno-exceptions -fno-rtti` on the `omgp_l3` target and the ESP-IDF component — use is a compile error |
| No heap after init | `tools/check_embedded.py` (quality stage) forbids `new`/`malloc`/allocating STL; every Catch2 test links a counting `__wrap_malloc` and asserts zero calls inside `HEAP_FREE_SCOPE` |
| No OS, no wall clock | `check_embedded.py` forbids `<chrono>`, `<thread>`, `sleep`, `system_clock` |
| No protocol literals | `check_embedded.py` flags any integer literal the YAML also defines (≥ 0x10); values come from `build/gen/omgp_protocol.h` |
| Spec traceability | `check_embedded.py` requires a `protocol-l3 §…` citation in every file here |
| Caller-provided buffers, `Status` returns | by construction — see `l3_types.hpp`; no function allocates or throws |
| Both builds green | native ASan/UBSan (`./pipeline.sh`) and ESP32-S3 (`./pipeline.sh esp32`, IDF component `esp32-host/components/omgp_l3`) |

## Files

- `l3_types.hpp` — `Status`, `Header`, `Bytes`/`Str` views, payload PODs, `DescriptorReport`
- `l3_header.{hpp,cpp}` — §3 header; `decode_message` delimits the payload
- `l3_payload.{hpp,cpp}` — §3.1 payloads, §3.3 status block, `payload_bounds` dispatch
- `l3_descriptor.{hpp,cpp}` — §4 `RecordCursor`, `validate_descriptor`, typed decoders,
  `DescriptorWriter`, `descriptor_crc`
- `l3_utf8.hpp` — RFC 3629 well-formedness for descriptor strings

Adding an opcode: `docs/ADDING-AN-OPCODE.md`.
