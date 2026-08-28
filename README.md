# OMGP — Open Modular Guitar Platform

An open standard for modular guitar signal hardware: analogue and tube
modules with digital control, discovery, presets and MIDI — think
"500 series with a control plane," for rack and studio use.

**Status: pre-hardware.** The protocol, host software and simulator are
being built software-first; the specification drives everything.

- `docs/` — the specification and protocol drafts (authoritative)
- `protocol/omgp-protocol.yaml` — machine-readable protocol ground truth
- `pipeline.sh` — build + full test suite (see CLAUDE.md)
- `docs/GOVERNANCE.md` — how this repo is run (AI-assisted delivery with
  human gates; every merge is human-approved)

This repository is developed with heavy AI agent involvement under the
governance model in `docs/`. Issues and PRs from humans are very welcome —
agent-authored changes are labelled as such and gated identically.

Software: Apache-2.0. Hardware licence to be finalised before any design
files land.
