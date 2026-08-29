# Definition of Ready — stories for AI development

A story may be released to agent development (`ready` label) only when it
would let a competent agent with no conversation history implement it
correctly from the issue plus the repo alone. The `ready-gate` workflow
enforces the mechanical half; the human applying `ready` owns the
judgement half.

## The PRD scheme (story.yml template sections)

| Section | Ready means | Why agents need it |
|---|---|---|
| Intent | One paragraph of what/why, no how | Anchors judgement calls during implementation |
| Spec references | Named sections of the authoritative docs | Prevents reinvention; comments must cite these |
| Acceptance criteria | Checkbox assertions, each mappable to a test or scenario | These BECOME the failing tests written first |
| Evidence required | Pipeline stages / named scenarios beyond the §5 baseline | Defines green for this story specifically |
| Out of scope | Explicit exclusions and adjacent temptations | The main defence against helpful overreach |
| Dependencies | Issue refs, all closed before release | Enforced: open deps block `ready` |
| Expected risk tier | T0–T2 for dispatchable work | T3 work is human-ruling work; it is never dispatched |

## Mechanical gate (ready-gate workflow)

Applying `ready` triggers validation: all sections present and non-empty,
acceptance criteria in checkbox form, no open dependencies, expected tier
not T3. Failures remove the label with a gap list; the label only sticks
on a conforming story. Re-apply after fixing.

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
