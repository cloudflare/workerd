---
name: pr-review-guide
description: Guidelines for posting pull request review comments via GitHub CLI, including suggested edits format, handling unresolved comments, etiquette, and report/issue tracking. Load this skill when reviewing a PR via GitHub and posting inline comments.
---

## Pull Request Review Guide

Load this skill when posting review comments on a GitHub pull request.

---

### Posting Review Comments

When asked to review a pull request, you may use the GitHub CLI tool to post inline comments on the PR with specific feedback for each issue you identify. Do not make code changes yourself, but you can suggest specific code changes in your comments. Be sure to reference specific lines of code in your comments for clarity.

When providing feedback on a pull request, focus on actionable insights that can help improve the code. Be clear and concise in your comments, and provide specific examples or references to the code to support your feedback. Avoid vague statements and instead provide concrete suggestions for improvement.

Review comments should be posted on individual lines of code in the pull request, never as a single monolithic comment. This allows for clearer communication and easier tracking of specific issues.

### Suggested Edits

When the fix for an issue is obvious and localized (e.g., a typo, a missing annotation, a wrong type, a simple rename), include a GitHub suggested edit block in your review comment so the author can apply it with one click. Use this format:

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

When reviewing a PR, check prior review comments (from any reviewer) that have been marked as resolved. If the current code still exhibits the issue described in a resolved comment, flag it as a finding with a reference to the original comment. Use this format:

- **[HIGH]** Previously flagged issue not addressed: _{original comment summary}_
  - **Location**: File and line references
  - **Problem**: Review comment by {author} was marked resolved but the underlying issue remains in the current code.
  - **Evidence**: Link to or quote the original comment, and show the current code that still has the issue.
  - **Recommendation**: Address the original feedback before merging.

Do not flag resolved comments where the concern has been legitimately addressed, even if addressed differently than the reviewer suggested.

### Etiquette

- Do not spam the pull request with excessive comments. Focus on the most important issues and provide clear guidance on how to address them. If there are minor style issues, you can mention them but prioritize more significant architectural, performance, security, or correctness issues.
- Do not modify existing comments or feedback from other reviewers. When issues are addressed and resolved, you can acknowledge the changes with a new comment but avoid editing or deleting previous comments to maintain a clear history of the review process.
- Always be respectful and constructive. Always acknowledge that the code review comments are written by an AI assistant and may not be perfect.

---

### Reporting

When asked, you may prepare a detailed report and status tracking document when refactoring is planned. The report should be in markdown format, would be placed in the docs/planning directory, and must be kept up to date as work progresses. It should contain suitable information and context to help resume work after interruptions. The agent has permission to write and edit such documents without additional approval but must not make any other code or documentation changes itself.

### Issue Tracking

When appropriate, you may be asked to create and maintain Jira tickets or GitHub issues to track work items. You have permission to create and edit such tickets and issues without additional approval but must not make any other code or documentation changes itself. When creating such tickets or issues, ensure they are well-formed, with clear titles, descriptions, acceptance criteria, and any relevant links or context. Also make sure it's clear that the issues are being created/maintained by an AI agent.

Avoid creating duplicate tickets or issues for the same work item. Before creating a new ticket or issue, search existing ones to see if it has already been created. If it has, update the existing ticket or issue instead of creating a new one.

Be concise and clear in ticket and issue descriptions, focusing on actionable information. Do not be overly verbose or include unnecessary details. Do not leak internal implementation details or sensitive information in ticket or issue descriptions. When in doubt, err on the side of caution and omit potentially sensitive information or ask for specific permission and guidance.

For interaction with GitHub, use the GitHub CLI (gh) tool or git as appropriate.
