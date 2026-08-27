#!/usr/bin/env python3
"""Until F4's scenario_runner exists: validate scenario YAML shape."""
import sys, pathlib, yaml
ok = True
for f in sorted(pathlib.Path("tests/scenarios").glob("*.yaml")):
    s = yaml.safe_load(f.read_text())
    for key in ("name", "requirement", "rig", "script"):
        if key not in s:
            print(f"scenario-lint: {f.name}: missing '{key}'"); ok = False
    if not isinstance(s.get("script"), list) or not s["script"]:
        print(f"scenario-lint: {f.name}: script must be a non-empty list"); ok = False
print("scenario-lint:", "all scenarios well-formed" if ok else "FAILED")
sys.exit(0 if ok else 1)
