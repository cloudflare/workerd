---
name: pr-review-guide
description: Guidelines for posting pull request review comments via GitHub CLI, including suggested edits format, handling unresolved comments, etiquette, and report/issue tracking. Load this skill when reviewing a PR via GitHub and posting inline comments.
---

Load this skill when posting review comments on a GitHub pull request.

### Line Number Tracking

When analyzing a PR diff, **Always** record exact file paths and line numbers for every finding as
you go. Each finding must include the precise `path` and `line` (and `start_line` for multi-line
ranges) in the _new_ file (right side of the diff) needed to post a review comment. **Do not** defer
line number resolution to a later step.

When the review is performed by a sub-agent, the agent's returned findings must include these fields
per finding so the caller can post comments immediately:

- `path`: file path relative to repo root
- `line`: line number in the new file (end line for multi-line)
- `start_line` (optional): start line for multi-line comments
- `body`: the comment text, ready to post

### Posting Review Comments

When asked to review a pull request, you may use the GitHub CLI tool to post inline comments on the
PR with specific feedback for each issue you identify. You can suggest specific code changes in your
comments. **Always** reference specific lines of code in your comments for clarity.

When providing feedback on a pull request:

- **Always** focus on actionable insights that can help improve the code
- **Always** be clear and concise in your comments; provide specific examples or references
  to the code to support your feedback. Avoid vague statements and instead provide concrete
  suggestions for improvement.
- **Always** post comments on specific lines of code and never as a single monolithic comment

### Suggested Edits

When the fix for an issue is obvious and localized (e.g., a typo, a missing annotation, a wrong
type, a simple rename), include a GitHub suggested edit block in your review comment so the author
can apply it with one click. Use this format:

````
```suggestion
corrected line(s) of code here
```
````

Guidelines for suggested edits:

- **Do** use them for: typos, missing `override`/`[[nodiscard]]`/`constexpr`, wrong types, simple renames, small bug fixes where the correct code is unambiguous.
- **Do not** use them for: large refactors, design changes, cases where multiple valid fixes exist, or anything requiring context the author should decide on.
- Keep suggestions minimal — change only the lines that need fixing. Do not reformat surrounding code.
- When a suggestion spans multiple lines, include all affected lines in the block.

### Unresolved Review Comments

When reviewing a PR, **always** check prior review comments (from any reviewer) that have been marked
as resolved. If the current code still exhibits the issue described in a resolved comment, flag it as
a finding with a reference to the original comment. Use this format:

- **[HIGH]** Previously flagged issue not addressed: _{original comment summary}_
  - **Location**: File and line references
  - **Problem**: Review comment by {author} was marked resolved but the underlying issue remains in
    the current code.
  - **Evidence**: Link to or quote the original comment, and show the current code that still has
    the issue.
  - **Recommendation**: Address the original feedback before merging.

**Do not** flag resolved comments where the concern has been legitimately addressed, even if addressed differently than the reviewer suggested.

### Tone

- **Do not editorialize.** No praise, no compliments on the approach, no filler like "nice fix!" or "solid solution." The review body and inline comments should contain only findings, questions, and actionable feedback. Let the findings speak for themselves.
- The review body should be a concise summary of findings (a bulleted list is fine) plus the AI-generated disclaimer. Nothing else.

### Etiquette

- Do not spam the pull request with excessive comments. Focus on the most important issues and
  provide clear guidance on how to address them. If there are minor style issues, you can mention
  them but prioritize more significant architectural, performance, security, or correctness issues.
- Do not modify existing comments or feedback from other reviewers. When issues are addressed and
  resolved, you can acknowledge the changes with a new comment but avoid editing or deleting
  previous comments to maintain a clear history of the review process.
- Always be respectful and constructive. Always acknowledge that the code review comments are written
  by an AI assistant and may not be perfect.

### Tools

For interaction with GitHub, use the GitHub CLI (`gh`) tool or `git` as appropriate.
