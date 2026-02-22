---
description: Find who owns or is most active in a file or directory
subtask: true
---

Find code owners for: $ARGUMENTS

Steps:

1. **Resolve the path.** If the argument is a symbol name, find its file first. If it's a directory, analyze the directory as a whole.

2. **Recent commit activity.** Run:

   ```
   git log --format='%aN <%aE>' --since='6 months ago' -- <path> | sort | uniq -c | sort -rn | head -10
   ```

   This shows who has been most active recently.

3. **Blame analysis.** For individual files, run:

   ```
   git blame --line-porcelain <file> | grep '^author ' | sort | uniq -c | sort -rn | head -10
   ```

   This shows who wrote the most lines currently in the file.

4. **Check for CODEOWNERS.** Look for a `CODEOWNERS` or `.github/CODEOWNERS` file that may define ownership rules.

5. **Output:**
   - **Top recent contributors** (last 6 months): ranked list with commit counts
   - **Top authors by current lines**: ranked list with line counts
   - **Suggested reviewers**: 2-3 people who are most likely to be good reviewers, preferring people who are both recent contributors and significant authors
   - If the results are sparse (few commits, single author), note that and suggest broadening the search to the parent directory
