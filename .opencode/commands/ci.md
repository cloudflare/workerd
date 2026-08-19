---
description: CI status report for a workerd PR
subtask: true
---

Load the `ci-report` skill and produce a full CI report.

**Arguments:** $ARGUMENTS

If a PR number (e.g., `7010`) or URL (e.g.,
`https://github.com/cloudflare/workerd/pull/7010`) is provided, use that PR.
Otherwise, detect the PR associated with the current local branch. If there
is no associated PR, say so and stop — do not prompt the user for a number.

Follow the skill workflow:

1. Use the `ci-report` custom tool (or replicate its logic) to gather all
   GitHub Actions data in a single call. If no argument was provided, call
   it without a `pr` parameter so it auto-detects the current branch.
2. Use a single `portal_codemode_execute` call to gather all internal GitLab
   pipeline data (if accessible).
3. Triage every failure as material, pre-existing, or flaky by correlating
   errors against the PR's changed files.
4. Present the combined report with verdict.
5. Offer to retry flaky jobs (never material failures).
