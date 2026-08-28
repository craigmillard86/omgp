# Open Questions

Append-only log of spec ambiguities encountered during implementation, and
their resolutions. Entries are dated and never edited or deleted. A
decision is changed by appending a new, superseding entry that references
the entry it supersedes — never by editing history.

## Entry format

```
## YYYY-MM-DD — <short title>

**Context:** what was ambiguous and where (spec section, file).
**Recommendation:** the safe default proposed, if any.
**Ruling:** human decision, or "pending" until one is made.
**Supersedes:** (optional) link/reference to the prior entry this replaces.
```

---

## 2026-08-28 — Software licence: Apache-2.0

**Context:** `docs/omgp-spec-v0.7.md` §50 "Licensing Direction" says
licensing "remains to be formally selected" (Apache-2.0 or MIT plausible
for software), while `docs/GOVERNANCE.md` §4b and constitution Principle
VIII state Apache-2.0. The committed `LICENSE` file contained no licence
text at all — only a sandbox network-egress error message captured when
an earlier fetch of the Apache-2.0 text failed — so no licence was
actually granted for this public repository.
**Recommendation:** Apache-2.0, as already stated by the constitution and
GOVERNANCE.md; replace `LICENSE` with the canonical text.
**Ruling:** Apache-2.0 — human instruction 2026-08-28. `LICENSE` replaced
with the canonical Apache License 2.0 text (sourced from the system
licence store, not the network). Spec §50 should be updated to record the
decision; the spec is a human-ruling artefact, so that edit is left to a
human.
**Supersedes:** none.
