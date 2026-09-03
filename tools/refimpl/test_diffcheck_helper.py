"""Tests for tools/diffcheck.py's Helper.ask_stream reader (spec 002 US1, T025), written
first per CLAUDE.md rule 8: this file must fail before the ask_stream fix and pass after.

Runs a tiny fake stand-in process instead of the real l3_helper binary, so the reader's
block-termination logic is pinned independent of the native build.
"""
from __future__ import annotations

import pathlib
import sys
import threading

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import diffcheck as D  # noqa: E402

# Simulates tools/l3_helper_dispatch.cpp's FSTREAM case being renamed or dropped: dispatch
# falls through to the single-line "ERR BadRequest" response with no "END " line, and the
# process stays alive afterwards (blocked reading the next request) rather than exiting.
FAKE_HELPER_NO_END = """#!/usr/bin/env python3
import sys
for line in sys.stdin:
    line = line.strip()
    if line == "QUIT":
        break
    if line.startswith("FSTREAM"):
        print("ERR BadRequest", flush=True)
    # else: no output at all -- alive but produces nothing further for this request.
"""


def test_ask_stream_reports_a_live_helper_that_never_sends_end_instead_of_hanging(tmp_path):
    script = tmp_path / "fake_helper.py"
    script.write_text(FAKE_HELPER_NO_END)
    script.chmod(0o755)
    helper = D.Helper(script)
    result: dict = {}
    t = threading.Thread(target=lambda: result.__setitem__("out", helper.ask_stream(["FSTREAM aa"])), daemon=True)
    t.start()
    t.join(timeout=5)
    try:
        assert not t.is_alive(), "ask_stream hung on a live helper that answered without an END line"
        assert result["out"] == [["ERR BadRequest"]]
    finally:
        helper.p.kill()
        helper.p.wait(timeout=5)


# Mirrors tools/canonical.cpp's real fstream_response() bad-hex path: dispatch_line returns
# "ERR BadRequest\nEND 0" as one string, and l3_helper.cpp prints it with a single trailing
# '\n' (tools/l3_helper.cpp:31) -- so both lines land in the pipe as one flushed write, one
# block, for one FSTREAM request. A second request follows so a reader that stops at the
# first non-"OK " line (rather than at "END ") would misattribute "END 0" to it.
FAKE_HELPER_TWO_LINE_ERR = """#!/usr/bin/env python3
import sys
for line in sys.stdin:
    line = line.strip()
    if line == "QUIT":
        break
    if line == "FSTREAM zz":
        print("ERR BadRequest\\nEND 0", flush=True)
    elif line.startswith("FSTREAM"):
        print("OK frame\\nEND 1", flush=True)
"""


def test_ask_stream_keeps_a_two_line_error_response_in_one_block(tmp_path):
    script = tmp_path / "fake_helper.py"
    script.write_text(FAKE_HELPER_TWO_LINE_ERR)
    script.chmod(0o755)
    helper = D.Helper(script)
    try:
        out = helper.ask_stream(["FSTREAM zz", "FSTREAM aa"])
    finally:
        helper.close()
    assert out == [["ERR BadRequest", "END 0"], ["OK frame", "END 1"]]
