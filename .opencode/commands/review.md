---
description: Review local branch changes or a GitHub PR
agent: architect
subtask: true
---

Review code changes. Determine what to review based on the arguments below, then follow the architect agent's review workflow.

**Arguments:** $ARGUMENTS

**How to determine what to review:**

1. If arguments contain a PR number (e.g., `1234`) or URL (e.g., `https://github.com/.../pull/1234`), review that PR:
   - Use `gh pr view <number>` for the description and metadata
   - Use `gh pr diff <number>` for the changes
   - Use `gh pr checks <number>` for CI status
   - Check for prior review comments via `gh api`

2. If no PR is specified, review the current branch vs origin/main:
   - Use `git diff origin/main...HEAD` for the changes
   - Use `git log --oneline origin/main..HEAD` for commit context

Perform a balanced review covering all analysis areas. Focus on CRITICAL and HIGH issues first. If there are no significant issues, say so briefly rather than inventing low-value findings.

Any additional instructions from the arguments above (e.g., "focus on memory safety") should narrow the review scope accordingly.
