#!/usr/bin/env python3
"""Create one GitHub issue per task in a Spec Kit tasks.md.
Spec Kit ships an optional taskstoissues command — prefer that if installed;
this is the dependency-free fallback. Requires gh CLI, authenticated.

Usage: tools/tasks-to-issues.py specs/<NNN-feature>/tasks.md feature:f2-link
Idempotent: skips tasks whose title already exists as an open issue.
"""
import re, subprocess, sys, json, pathlib

path, feature = sys.argv[1], sys.argv[2]
text = pathlib.Path(path).read_text()
# Spec Kit task lines: "- [ ] T001 [P] Description ..." (checkbox optional across versions)
tasks = re.findall(r'^\s*-\s*(?:\[[ xX]\]\s*)?(T\d+)\s*(\[P\])?\s*(.+)$', text, re.M)
if not tasks:
    sys.exit(f"no tasks matched in {path} — check the tasks.md format")
existing = json.loads(subprocess.run(
    ["gh", "issue", "list", "--label", "task", "--state", "all",
     "--json", "title", "--limit", "500"],
    capture_output=True, text=True, check=True).stdout)
have = {i["title"] for i in existing}
for tid, par, desc in tasks:
    title = f"{tid}: {desc.strip()}"[:120]
    if title in have:
        print(f"skip (exists): {title}"); continue
    body = f"""### Intent
Spec Kit task **{tid}** from `{path}`{' — parallelisable [P]' if par else ''}: {desc.strip()}

### Spec references
See the feature's spec.md/plan.md under `{pathlib.Path(path).parent}/` — REPLACE with the concrete doc sections before releasing.

### Acceptance criteria
- [ ] {desc.strip()}
- [ ] REPLACE: add falsifiable assertions per DEFINITION-OF-READY.md

### Evidence required
Full local pipeline green (OPERATING-POLICY §5); name any new scenarios here.

### Out of scope
REPLACE: name adjacent work this task must not include.

### Dependencies
Preceding tasks in this feature's tasks.md order, unless [P].

### Expected risk tier
REPLACE: T0/T1/T2 per GOVERNANCE.md §3.
"""
    subprocess.run(["gh", "issue", "create", "--title", title, "--body", body,
                    "--label", "task", "--label", "enrich", "--label", feature], check=True)
    print(f"created: {title}")
