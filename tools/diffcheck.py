#!/usr/bin/env python3
"""Differential test: C++ implementation vs Python reference over a random corpus."""
import pathlib, random, subprocess, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent / "refimpl"))
from omgp_crc import crc16_ccitt_false
ROOT = pathlib.Path(__file__).resolve().parents[1]
helper = ROOT / "build" / "native" / "crc_helper"
if not helper.exists():
    sys.exit("diffcheck: build the native preset first (crc_helper missing)")
rng = random.Random(0xB0071E)  # fixed seed: reproducible corpus
for i in range(200):
    data = bytes(rng.randrange(256) for _ in range(rng.randrange(0, 80)))
    cpp = int(subprocess.run([str(helper)], input=data, capture_output=True).stdout.strip(), 16)
    ref = crc16_ccitt_false(data)
    if cpp != ref:
        sys.exit(f"diffcheck: MISMATCH on case {i}: cpp={cpp:#06x} ref={ref:#06x} data={data.hex()}")
print("diffcheck: 200 cases, C++ and Python agree")
