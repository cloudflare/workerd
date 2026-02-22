---
description: Summarize current branch changes for a PR description or release note
subtask: true
---

Generate a changelog for the current branch. $ARGUMENTS

Steps:

1. **Determine the branch and base.** Run:

   ```
   git log --oneline origin/main..HEAD
   ```

   If there are no commits, try `main..HEAD`. If the user specified a different base in the arguments, use that.

2. **Read the full diff** to understand the actual changes:

   ```
   git diff origin/main...HEAD --stat
   ```

3. **Read commit messages** for context:

   ```
   git log origin/main..HEAD --format='%h %s%n%n%b'
   ```

4. **Categorize changes** by area. Use these categories based on the files changed:
   - **API** — `src/workerd/api/` changes
   - **Node.js compat** — `src/workerd/api/node/` or `src/node/` changes
   - **I/O** — `src/workerd/io/` changes
   - **JSG** — `src/workerd/jsg/` changes
   - **Server** — `src/workerd/server/` changes
   - **Build** — `build/`, `MODULE.bazel`, `BUILD.bazel` changes
   - **Types** — `types/` changes
   - **Tests** — test-only changes
   - **Docs** — documentation-only changes
   - **Other** — anything that doesn't fit above

5. **Draft the summary.** For each category with changes:
   - One bullet per logical change (not per commit — squash related commits into one bullet)
   - Start each bullet with a verb: Add, Fix, Update, Remove, Refactor
   - Include the most important file references
   - Note any breaking changes, new compat flags, or new autogates prominently

6. **Output format:**

   ```
   ## Summary

   <1-2 sentence overview of the branch's purpose>

   ## Changes

   ### <Category>
   - <change description>

   ## Breaking Changes
   <if any, otherwise omit section>

   ## Testing
   <brief note on what tests were added or modified>
   ```

   If the user's arguments request a specific format (e.g., "for release notes", "for PR"), adjust the tone accordingly. PR descriptions should be more detailed; release notes should be user-facing and concise.
