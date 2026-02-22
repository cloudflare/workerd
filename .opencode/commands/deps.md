---
description: Show the dependency graph for a Bazel target
subtask: true
---

Show dependencies for: $ARGUMENTS

Steps:

1. **Resolve the target.** If the argument is a file path, find its Bazel target first by checking the `BUILD.bazel` in the same directory. If it's already a Bazel label (e.g., `//src/workerd/api:http`), use it directly.

2. **Direct dependencies.** Run:

   ```
   bazel query 'deps(<target>, 1)' --output label 2>/dev/null
   ```

   This shows what the target directly depends on.

3. **Reverse dependencies** (what depends on this target). Run:

   ```
   bazel query 'rdeps(//src/..., <target>, 1)' --output label 2>/dev/null
   ```

   This shows what directly depends on this target within `src/`.

4. **If the user asks for the full transitive graph**, use depth 2-3 instead of 1, but warn that deep queries can be slow.

5. **Output:**
   - **Target**: the resolved Bazel label
   - **Direct dependencies**: grouped by type (internal `//src/...` vs external `@...`)
   - **Reverse dependencies**: what depends on this target (within `src/`)
   - **Notable observations**: circular dependency risks, unusually large dependency sets, or external deps that may be surprising

   Keep the output concise. If there are more than 20 deps in a category, summarize the count and list the most important ones.
