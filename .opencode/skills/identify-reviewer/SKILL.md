---
name: identify-reviewer
description: Identifies the local user's GitHub account and git identity before performing code reviews. Load this skill at the start of any PR review, code review, or commit log analysis so findings can be framed relative to the user's own prior comments, commits, and approval status.
---

## Identify Reviewer

Load this skill at the start of any code review workflow — before analyzing diffs, commit logs, or prior comments.

---

### Steps

1. **Detect identities.** Run these commands in parallel:

   ```
   gh auth status
   git config user.name
   git config user.email
   ```

   - From `gh auth status`, parse `Logged in to github.com account <USERNAME>` to get the GitHub handle.
   - From `git config`, capture the local user's commit name and email.

   These may not match exactly (e.g., GitHub handle `octocat`, git name `Octo Cat`, git email `octocat@example.com`). All three identify the same person.

   If `gh` is not installed or the user is not authenticated, fall back to git config alone. If neither is available, skip identification and proceed without it — do not block the review.

2. **Store the identity for the session.** Use the discovered identity when:
   - **Attributing prior review comments.** If the user previously commented on the PR, refer to those comments in second person ("your earlier comment about X") rather than third person ("octocat flagged X").
   - **Attributing commits.** When analyzing `git log` output, match the author name/email against the git config values. Refer to the user's own commits in second person ("your commit `abc1234` introduced...") and other authors' commits in third person.
   - **Filtering approval status.** Note whether the user has already approved, requested changes, or not yet reviewed. Frame the review summary accordingly (e.g., "You previously approved this PR; here are new findings since your approval").
   - **Distinguishing roles.** If the user is also the PR author, flag this clearly and adjust tone (self-review). Match by both GitHub handle and git author email since either may appear in PR metadata.

3. **Apply throughout the review.** Every finding that references a prior comment or commit by the user should use second person. Prior comments and commits by _other_ people remain in third person with their handle or name.

### Matching Rules

- GitHub handle from `gh auth status` matches PR review comment authors and PR author login.
- Git name from `git config user.name` matches `git log --format='%an'` author names.
- Git email from `git config user.email` matches `git log --format='%ae'` author emails.
- A person is the local user if **any** of these identifiers match.

### Examples

Without this skill:

> octocat flagged the performance regression in a prior review. The author disagreed.

With this skill (when the local user is octocat):

> You flagged the performance regression in a prior review. The author disagreed but did not provide benchmarks.

Without this skill:

> Commit `abc1234` by Octo Cat refactored the decoder interface.

With this skill (when git user.name is "Octo Cat"):

> Your commit `abc1234` refactored the decoder interface.
