---
description: Show the dependency graph for a Bazel target
subtask: true
---

Show dependencies for: $ARGUMENTS

**Use the `bazel-deps` tool** with `direction: "deps"` to perform this lookup. It handles file path resolution, Bazel queries (forward and reverse in parallel), and grouping automatically. Pass the argument as the `target`.

The target supports ecosystem qualifiers to disambiguate when a name matches both C++ and Rust targets:

- `rust:base64` or `crate:base64` — resolve as a Rust crate only
- `cpp:base64` or `cc:base64` — resolve as a C++ target only
- Unqualified names (e.g. `base64`) use the default resolution order (C++ first, then Rust)

Pass the full argument including any qualifier as the `target` (e.g. `target: "rust:base64"`).

If the user asks for the full transitive graph, pass `depth: 2` or `depth: 3`. Warn that deeper queries can be slow.

After receiving the tool output, add any useful observations:

- Are there circular dependency risks or unusually large dependency sets?
- Are there surprising external dependencies?
- For targets with many reverse dependents, note the impact of changes to this target.
