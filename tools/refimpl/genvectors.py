#!/usr/bin/env python3
"""Generate the golden byte vectors under tests/vectors/ from the reference implementation.

    python3 tools/refimpl/genvectors.py            # (re)write tests/vectors/*.json
    python3 tools/refimpl/genvectors.py --check    # exit 1 if any existing file would change

Vectors are immutable evidence (CLAUDE.md rule 9): this script is run by a human when a
vector is created or a protocol ruling changes one, and the commit carries the reason.
The pipeline only runs --check. Schema: contracts/golden-vector.schema.json.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from _gen import P  # noqa: E402
import canonical as C  # noqa: E402
import omgp_l3 as l3  # noqa: E402
import omgp_link as link  # noqa: E402

G = P()
ROOT = pathlib.Path(__file__).resolve().parents[2]
VECTORS = ROOT / "tests" / "vectors"
RESP = G.FLAG_response
ERRF = G.FLAG_response | G.FLAG_error


def msg(name, spec_ref, opcode, node, seq, flags, obj=None, note=None):
    return {"name": name, "kind": "message", "spec_ref": spec_ref,
            "header": (opcode, node, seq, flags), "obj": obj, "note": note}


TAIL61 = bytes(range(61))

# Every opcode in both directions where a layout exists (IDENTIFY response is created in
# T047 once the sample descriptor makes desc_crc computable), plus boundary values.
MESSAGES = [
    msg("msg_ping_req", "protocol-l3 §3.1 PING", G.OP_PING, 0x01, 0, 0),
    msg("msg_ping_resp", "protocol-l3 §3.1 PING response (seq echoed)", G.OP_PING, 0x01, 0, RESP),
    msg("msg_ping_req_max_node", "protocol-l3 §2 node id 0x7F (last module id)", G.OP_PING,
        G.ADDR_module_max, 255, 0, note="boundary: node_id and seq at max"),
    msg("msg_identify_req", "protocol-l3 §3.1 IDENTIFY", G.OP_IDENTIFY, 0x10, 5, 0),
    msg("msg_read_desc_req", "protocol-l3 §3.1 READ_DESC (module-bus chunk)", G.OP_READ_DESC, 0x10, 2, 0,
        l3.ReadDescReq(0, G.LIMIT_module_bus_chunk)),
    msg("msg_read_desc_resp_max", "protocol-l3 §3.1 READ_DESC response, 61-byte tail", G.OP_READ_DESC,
        0x10, 2, RESP, l3.ReadDescResp(1987, TAIL61), note="boundary: payload_len 64"),
    msg("msg_read_desc_resp_short", "protocol-l3 §3.1 READ_DESC response, last chunk", G.OP_READ_DESC,
        0x10, 3, RESP, l3.ReadDescResp(2040, bytes(range(1, 9)))),
    msg("msg_select_channel_req", "protocol-l3 §3.1/§3.2 SELECT_CHANNEL", G.OP_SELECT_CHANNEL, 0x10, 7, 0,
        l3.SelectChannelReq(1)),
    msg("msg_select_channel_resp", "protocol-l3 §3.2 accepted (empty)", G.OP_SELECT_CHANNEL, 0x10, 7, RESP),
    msg("msg_set_bypass_req", "protocol-l3 §3.1 SET_BYPASS", G.OP_SET_BYPASS, 0x10, 8, 0, l3.SetBypassReq(1)),
    msg("msg_set_bypass_resp", "protocol-l3 §3.1 SET_BYPASS accepted", G.OP_SET_BYPASS, 0x10, 8, RESP),
    msg("msg_set_param_req", "protocol-l3 §3.1 SET_PARAM (absolute value, max)", G.OP_SET_PARAM, 0x10, 3, 0,
        l3.SetParamReq(1, 0xFF, G.LIMIT_param_value_max), note="boundary: value 4095, scope module"),
    msg("msg_set_param_resp", "protocol-l3 §3.1 SET_PARAM accepted", G.OP_SET_PARAM, 0x10, 3, RESP),
    msg("msg_get_param_req", "protocol-l3 §3.1 GET_PARAM", G.OP_GET_PARAM, 0x10, 4, 0, l3.GetParamReq(2, 0)),
    msg("msg_get_param_resp", "protocol-l3 §3.1 GET_PARAM response", G.OP_GET_PARAM, 0x10, 4, RESP,
        l3.GetParamResp(2, 0, 2048)),
    msg("msg_get_status_req", "protocol-l3 §3.1 GET_STATUS", G.OP_GET_STATUS, 0x10, 4, 0),
    msg("msg_get_status_resp", "protocol-l3 §3.3 status block", G.OP_GET_STATUS, 0x10, 4, RESP,
        l3.StatusBlock(G.STATE_READY, 2, 0, 0, 77, 1)),
    msg("msg_get_event_req", "protocol-l3 §3.1 GET_EVENT", G.OP_GET_EVENT, 0x10, 9, 0),
    msg("msg_get_event_resp_none", "protocol-l3 §3.4 empty queue -> NONE", G.OP_GET_EVENT, 0x10, 9, RESP,
        l3.GetEventResp(G.EVT_NONE, 0, b"")),
    msg("msg_get_event_resp_settled", "protocol-l3 §3.2/§3.4 CHANNEL_SETTLED", G.OP_GET_EVENT, 0x10, 10, RESP,
        l3.GetEventResp(G.EVT_CHANNEL_SETTLED, 2, bytes([1]))),
    msg("msg_get_event_resp_user", "protocol-l3 §3.4 USER_DEFINED 0xF0", G.OP_GET_EVENT, 0x10, 11, RESP,
        l3.GetEventResp(G.EVT_USER_DEFINED_MIN, 0, bytes.fromhex("deadbeef"))),
    msg("msg_bp_slot_map_req", "protocol-l3 §3.1 BP_SLOT_MAP (opaque)", G.OP_BP_SLOT_MAP, 0x02, 1, 0,
        l3.OpaquePayload(bytes.fromhex("0102"))),
    msg("msg_bp_slot_map_resp", "protocol-l3 §3.1 BP_SLOT_MAP response (opaque)", G.OP_BP_SLOT_MAP, 0x02, 1, RESP,
        l3.OpaquePayload(bytes.fromhex("03"))),
    msg("msg_bp_power_req", "protocol-l3 §3.1 BP_POWER (opaque)", G.OP_BP_POWER, 0x02, 1, 0,
        l3.OpaquePayload(bytes.fromhex("01ff00"))),
    msg("msg_bp_power_resp", "protocol-l3 §3.1 BP_POWER response (opaque readback)", G.OP_BP_POWER, 0x02, 1, RESP,
        l3.OpaquePayload(bytes.fromhex("01ff00c8000100"))),
    msg("msg_bp_route_req", "protocol-l3 §3.1 BP_ROUTE (opaque, format TBD)", G.OP_BP_ROUTE, 0x02, 2, 0,
        l3.OpaquePayload(bytes.fromhex("00"))),
    msg("msg_bp_route_resp", "protocol-l3 §3.1 BP_ROUTE response (opaque, empty)", G.OP_BP_ROUTE, 0x02, 2, RESP,
        l3.OpaquePayload(b""), note="boundary: opaque payload of length 0"),
    msg("msg_error_resp_busy", "protocol-l3 §3.1 ERROR busy/settling, no detail", G.OP_ERROR, 0x10, 3, ERRF,
        l3.ErrorResp(G.ERR_BUSY, b"")),
    msg("msg_error_resp_detail", "protocol-l3 §3.1 ERROR bad payload with detail", G.OP_ERROR, 0x10, 3, ERRF,
        l3.ErrorResp(G.ERR_BAD_PAYLOAD, bytes.fromhex("0405"))),
]

STATUS = [
    {"name": "status_block", "kind": "status", "spec_ref": "protocol-l3 §3.3",
     "obj": l3.StatusBlock(G.STATE_READY, 2, 0, 0, 77, 1), "note": None},
]


def _ping_req_bytes() -> bytes:
    return C.encode_message(l3.Header(G.OP_PING, 0x01, 0, 0, 0), None)


def _ping_resp_bytes() -> bytes:
    return C.encode_message(l3.Header(G.OP_PING, 0x01, 0, RESP, 0), None)


# contracts/frame-vectors.md "frame_worst_stuffing": every 0x7E/0x7D byte in this 64-byte
# payload is chosen so that, combined with dst=src=0x7D and the pinned ctrl/len, the CRC
# also lands on 0x7D7E — every escapable byte in the frame escapes (SC-008, ruling
# 2026-08-31: 140 wire bytes is the achieved structural ceiling).
_WORST_STUFFING_PAYLOAD = bytes.fromhex(
    "7E7E7E7D7D7E7E7D7E7E7E7D7E7E7E7E7D7D7E7D7E7E7E7D7D7D7D7E7E7E7E7D"
    "7E7D7D7E7D7E7D7E7E7D7D7D7E7E7D7D7D7D7E7E7D7E7D7E7E7E7D7D7D7E7E7D"
)

# contracts/frame-vectors.md "Vectors" table (T020 agent-dispatchable slice, issue #38):
# the five frame_* golden vectors, built from the T018 codec (omgp_link.py) and cross
# checked against the T019 canonical renderer (canonical.py). Generation only — writing
# these under tests/vectors/ is a separate, human-triggered commit (CLAUDE.md rule 9,
# OPERATING-POLICY §2).
FRAMES = [
    ("frame_ping_req", "trunk §4 minimal frame; payload is the L3 PING request (msg_ping_req)",
     link.Frame(dst=0x01, src=0x00, response=False, retry=False, seq=0, payload=_ping_req_bytes()), None),
    ("frame_response", "trunk §4 response bit set, non-zero seq; payload is an L3 PING response",
     link.Frame(dst=0x00, src=0x01, response=True, retry=False, seq=5, payload=_ping_resp_bytes()), None),
    ("frame_retry", "trunk §4 retry bit set; seq at the 4-bit maximum",
     link.Frame(dst=0x01, src=0x00, response=False, retry=True, seq=15, payload=_ping_req_bytes()), None),
    ("frame_max_payload", "trunk §4 payload at LIMIT_max_l3_payload",
     link.Frame(dst=0x01, src=0x00, response=False, retry=False, seq=0, payload=bytes(range(64))),
     "72 wire bytes; CRC 0xE3F2 escapes neither byte (ruling 2026-08-31)"),
    ("frame_worst_stuffing", "trunk §4 worst-case stuffing: dst, src, payload and CRC all escape",
     link.Frame(dst=0x7D, src=0x7D, response=True, retry=True, seq=11, payload=_WORST_STUFFING_PAYLOAD),
     "140 wire bytes, the structural ceiling, achieved (SC-008, ruling 2026-08-31)"),
]


def _fields(obj) -> dict:
    if obj is None:
        return {}
    d = dataclasses.asdict(obj)
    return {k: (v.hex() if isinstance(v, bytes) else v) for k, v in d.items()}


def build() -> list[dict]:
    out = []
    for m in MESSAGES:
        opcode, node, seq, flags = m["header"]
        h0 = l3.Header(opcode, node, seq, flags, 0)
        raw = C.encode_message(h0, m["obj"])
        h = l3.decode_header(raw)
        entry = {"name": m["name"], "kind": "message", "spec_ref": m["spec_ref"],
                 "fields": {"opcode": G.OPCODE_NAMES[opcode], "node_id": node, "seq": seq, "flags": flags,
                            **_fields(m["obj"])},
                 "canonical": C.message_to_canonical(h, m["obj"]),
                 "bytes": " ".join(f"{b:02X}" for b in raw)}
        if m["note"]:
            entry["note"] = m["note"]
        out.append(entry)
    for s in STATUS:
        raw = l3.encode_status_block(s["obj"])
        out.append({"name": s["name"], "kind": "status", "spec_ref": s["spec_ref"],
                    "fields": _fields(s["obj"]), "canonical": C.status_to_canonical(s["obj"]),
                    "bytes": " ".join(f"{b:02X}" for b in raw)})
    for name, spec_ref, f, note in FRAMES:
        raw = link.encode_frame(f)
        entry = {"name": name, "kind": "frame", "spec_ref": spec_ref,
                 "fields": {"dst": f.dst, "src": f.src, "response": f.response, "retry": f.retry,
                            "seq": f.seq, "payload": f.payload.hex()},
                 "canonical": C.frame_to_canonical(f),
                 "bytes": " ".join(f"{b:02X}" for b in raw)}
        if note:
            entry["note"] = note
        out.append(entry)
    try:  # descriptor vectors arrive with US3 (omgp_descriptor.py)
        import descriptor_vectors  # type: ignore
        out.extend(descriptor_vectors.build())
    except ImportError:
        pass  # before US3 lands there are no descriptor vectors to add — by design, not an error
    return sorted(out, key=lambda e: e["name"])


def render(entry: dict) -> str:
    return json.dumps(entry, indent=2, sort_keys=True) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if any existing vector would change")
    ap.add_argument("--dir", default=str(VECTORS))
    args = ap.parse_args(argv)
    d = pathlib.Path(args.dir)
    entries = build()
    if args.check:
        drift = 0
        for e in entries:
            f = d / f"{e['name']}.json"
            if not f.exists():
                print(f"genvectors --check: {f.name} does not exist yet (run without --check, commit with justification)")
                drift += 1
            elif f.read_text() != render(e):
                print(f"genvectors --check: {f.name} would change — vectors are immutable; regenerate only with a "
                      f"documented ruling (CLAUDE.md rule 9)")
                drift += 1
        print(f"genvectors --check: {len(entries)} vectors, {drift} drift")
        return 1 if drift else 0
    d.mkdir(parents=True, exist_ok=True)
    for e in entries:
        (d / f"{e['name']}.json").write_text(render(e))
    print(f"genvectors: wrote {len(entries)} vectors to {d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
