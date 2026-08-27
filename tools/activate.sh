#!/usr/bin/env bash
# RUNBOOK steps 2-6, scripted. Run from repo root on YOUR machine after
# `git push`. Requires: gh CLI authed to the repo's owner account.
# Usage: OWNER=<github-username> REPO=<repo-name> tools/activate.sh
set -euo pipefail
OWNER="${OWNER:-craigmillard83}"; REPO="${REPO:-omgp}"
R="$OWNER/$REPO"

echo "== step 2: CODEOWNERS already set (@craigmillard83)"

echo "== step 3: branch protection on main"
gh api -X PUT "repos/$R/branches/main/protection" \
  -F "required_status_checks[strict]=true" \
  -F "required_status_checks[contexts][]=ci-gate" \
  -F "enforce_admins=true" \
  -F "required_pull_request_reviews[required_approving_review_count]=1" \
  -F "required_pull_request_reviews[require_code_owner_reviews]=true" \
  -F "restrictions=null" -F "allow_force_pushes=false" -F "allow_deletions=false" \
  -H "Accept: application/vnd.github+json" --input /dev/null 2>/dev/null || \
  gh api -X PUT "repos/$R/branches/main/protection" --input - << JSON
{"required_status_checks":{"strict":true,"contexts":["ci-gate"]},
 "enforce_admins":true,
 "required_pull_request_reviews":{"required_approving_review_count":1,"require_code_owner_reviews":true},
 "restrictions":null,"allow_force_pushes":false,"allow_deletions":false}
JSON

echo "== step 4: secret scanning + push protection + dependabot alerts"
gh api -X PATCH "repos/$R" --input - << 'JSON'
{"security_and_analysis":{"secret_scanning":{"status":"enabled"},
 "secret_scanning_push_protection":{"status":"enabled"}}}
JSON
gh api -X PUT "repos/$R/vulnerability-alerts"

echo "== step 5: OAuth token secret"
echo "Run 'claude setup-token' in another terminal, then paste the token:"
gh secret set CLAUDE_CODE_OAUTH_TOKEN -R "$R"
echo "MANUAL: install the Claude GitHub App on $R:"
echo "  https://github.com/apps/claude"

echo "== step 6: labels + project board"
OWNER="$OWNER" tools/gh-setup.sh
echo "MANUAL: in the OMGP Delivery project UI, enable built-in workflows:"
echo "  auto-add on label 'task'; item closed -> Done."

echo "== activation: done. Verify with a trivial PR (RUNBOOK step 8)."
