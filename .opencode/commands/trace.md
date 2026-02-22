---
description: Trace callers and callees of a function or method across the codebase
subtask: true
---

Trace call graph for: $ARGUMENTS

Steps:

1. **Find the definition.** Search for the function/method definition. If the argument is ambiguous (e.g., a common name like `get`), ask for clarification or use the class-qualified form (e.g., `IoContext::current`).

2. **Read the implementation.** Read the function body to identify:
   - **Direct callees**: functions/methods called within the body
   - Filter out trivial calls (logging, assertions, simple getters) unless they're relevant to understanding the flow

3. **Find callers.** Search for call sites across the codebase:

   ```
   grep -rn '<function_name>' src/ --include='*.h' --include='*.c++'
   ```

   Filter to actual calls (not declarations, comments, or string literals). For methods, search for both `->methodName(` and `.methodName(` patterns.

4. **Build the call graph.** Organize into:
   - **Callers** (who calls this function) — grouped by directory/component
   - **Callees** (what this function calls) — the significant ones from the implementation

5. **Trace one level deeper if needed.** If the argument includes "deep" or the function is a thin wrapper, trace one additional level in each direction to find the meaningful boundaries.

6. **Output:**
   - **Function**: full qualified name, file:line of definition
   - **Callers** (N call sites):
     - `file:line` — brief context of why it's called
   - **Callees** (N significant calls):
     - `function_name` (`file:line`) — brief description
   - **Call flow summary**: A brief narrative of how this function fits into the broader execution flow (e.g., "Called during request setup to initialize the I/O context, which then creates the worker lock...")
   - Limit to ~20 most important callers. If there are more, note the total count and list the most representative ones grouped by component.
