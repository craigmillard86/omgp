# Definition of Ready — stories for AI development

A story may be released to agent development (`ready` or `queued` label)
only when it would let a competent agent with no conversation history
implement it correctly from the issue plus the repo alone. The `ready-gate`
workflow enforces the mechanical half; the human applying the release
label owns the judgement half.

## The PRD scheme (story.yml template sections)

| Section | Ready means | Why agents need it |
|---|---|---|
| Intent | One paragraph of what/why, no how | Anchors judgement calls during implementation |
| Spec references | Named sections of the authoritative docs | Prevents reinvention; comments must cite these |
| Acceptance criteria | Checkbox assertions, each mappable to a test or scenario | These BECOME the failing tests written first |
| Evidence required | Pipeline stages / named scenarios beyond the §5 baseline | Defines green for this story specifically |
| Out of scope | Explicit exclusions and adjacent temptations | The main defence against helpful overreach |
| Dependencies | One `- #N` list item per issue that must be **closed** before this one starts (see the Dependencies rule below) | Enforced: open deps block `ready`; `queued` waits for them and auto-promotes |
| Expected risk tier | T0–T2 for dispatchable work | T3 work is human-ruling work; it is never dispatched |

## Two release paths

Both are the human's release judgement; both run the same content
validation. They differ only in when dispatch may pull the story:

| Label | Meaning | Dependencies | Dispatch |
|---|---|---|---|
| `ready` | dispatch now | must all be closed (open ones are a gap) | eligible immediately |
| `queued` | batch release | may still be open; `promote-queued` swaps `queued` → `ready` when the last one closes | eligible after promotion |

`queued` exists so a human can release a whole ordered slice (a Spec Kit
phase) in one sitting without babysitting each dependency; the promotion
is mechanical and the final `ready` still passes through `ready-gate`.
Agents pull only `ready`, never `queued`.

## Mechanical gate (ready-gate workflow)

Applying `ready` or `queued` triggers validation: all sections present and
non-empty, acceptance criteria in checkbox form, no `REPLACE:` placeholders,
expected tier not T3 (the declared tier is the first T0–T3 token of the
section). For `ready` the dependency check is blocking: any open dependency
is a gap. For `queued` open dependencies are reported, not rejected; if all
are already closed the gate swaps `queued` → `ready` on the spot. Failures
remove whichever label was applied, with a gap list; a release label only
sticks on a conforming story. Re-apply after fixing.

## Dependencies rule (what the gate and the promoter count)

- A blocking dependency is an issue ref on a **list item** of the Dependencies
  section (`- #19 (T001) …`; an item's indented continuation lines belong to
  it). `#19–#24` ranges expand.
- A list item that says the relation is **not blocking** — "not blocking",
  "not blocked by", "not dependent on", "non-blocking", "no ordering
  dependency", "sibling", "same PR", "expected to remain open" — is ignored,
  as is any ref in prose paragraphs when the section also has list items.
  Use those forms for `[P]` siblings and test-first pairs delivered in one
  PR; the enricher is told to.
- A section with **no** list items counts every ref in it (prose style).
- Why: two stories that name each other as dependencies can never be
  released or promoted. The enricher had correctly written "not a blocking
  dependency: #27" and "#53 … expected to remain open" — the old every-ref
  parser made cycles of them (#26↔#27 and seven more, 2026-08-30).

## Promotion (promote-queued workflow)

Whenever an issue closes (and on manual dispatch), every open `queued`
story whose Dependencies section refers only to closed issues — or
declares none — is promoted: `queued` removed, `ready` added, a
"promoted" comment posted, and `ready-gate` asked to run its final
validation. Stories carrying `blocked`, `needs-human` or `in-progress`
are never promoted. Because a label applied by a workflow does not start
`labeled` runs on its own, the promoter requests that validation
explicitly (`repository_dispatch`); the dependency check it just performed
is the same one `ready-gate` repeats.

## Judgement half (the human's checklist before labelling)

- Could I hand this to a contractor who can't ask questions? If not, what's
  missing goes in the issue, not in your head.
- Is each acceptance criterion falsifiable? "Works correctly" is not ready;
  "returns ERR_BUSY while settling" is.
- Does out-of-scope name the thing an eager implementer would bolt on?
- Is the story small enough for one PR and one review sitting? (The WIP cap
  makes oversized stories block the whole flow.)

## Enrichment and auto-release

Labelling a task `enrich` invokes the story-enrichment agent, which
populates all sections from the feature's `specs/<NNN-feature>/` artefacts and predicts
the risk tier. Stories predicted at or below `auto_ready_max_tier`
(.github/agent-config.yml) are auto-released to dispatch; higher tiers
queue for human review of the story. The prediction is checked against
reality by the PR-time risk score; merge approval is human in all cases.

## Sources of stories

- Spec Kit `tasks.md` via `tools/tasks-to-issues.py` — emitted bodies
  conform to the scheme, with acceptance criteria drawn from the task text;
  enrich thin ones before release rather than releasing and hoping.
- Manual stories via the "Story / task" issue template.
- Nightly/audit findings — triage-agent work items are exempt from the
  scheme (their spec is the failure itself) but not from the risk rules.

## Relationship to Spec Kit

The feature-level spec (spec.md/plan.md) answers "is the FEATURE fully
specified"; this scheme answers "is each STORY independently executable".
Both are needed: speckit's clarify gate catches feature ambiguity,
ready-gate catches under-specified decomposition. /speckit.checklist can
generate feature-specific readiness checklists to supplement this baseline.
