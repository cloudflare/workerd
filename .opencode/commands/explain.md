---
description: Explain what a file, class, or symbol does and how it fits into the architecture
subtask: true
---

Explain: $ARGUMENTS

Steps:

1. **Locate the target.** If the argument is a file path, read it. If it's a symbol name (class, function, macro), search for its definition using grep/glob.

2. **Read the definition.** For classes, read the header file first. For functions, read the declaration and implementation. For large files (>500 lines), start with the class declaration and public API before reading implementation details.

3. **Check for local documentation.** Look for:
   - An `AGENTS.md` in the same directory or nearest parent
   - A `README.md` in the same directory
   - Doc comments on the symbol itself

4. **Trace relationships.** Identify:
   - What this code depends on (base classes, key types used, includes)
   - What depends on this code (grep for callers, subclasses, or includes of the header)
   - Limit to the most important 5-10 relationships, not an exhaustive list

5. **Identify the architectural layer.** Place it in context:
   - Is this API layer (`api/`), I/O layer (`io/`), JSG bindings (`jsg/`), server infrastructure (`server/`), or utility (`util/`)?
   - What is its role in the request lifecycle or worker lifecycle?

6. **Output a concise summary** with:
   - **What it is**: One-sentence description
   - **Where it lives**: File path(s) and architectural layer
   - **Key responsibilities**: Bullet list of what it does
   - **Key relationships**: What it depends on and what depends on it
   - **Notable patterns or gotchas**: Anti-patterns, compat flag gating, thread safety considerations, or other things a developer should know
