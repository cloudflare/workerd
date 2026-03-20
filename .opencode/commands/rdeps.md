---
description: Find what depends on a given external dependency or internal target
subtask: true
---

Find reverse dependencies for: $ARGUMENTS

**Use the `bazel-deps` tool** with `direction: "rdeps"` to perform this lookup. It handles label resolution, Bazel queries, and grouping automatically. Pass the dependency name from the arguments as the `target`.

The target supports ecosystem qualifiers to disambiguate when a name matches both C++ and Rust targets:

- `rust:base64` or `crate:base64` — resolve as a Rust crate only
- `cpp:base64` or `cc:base64` — resolve as a C++ target only
- Unqualified names (e.g. `base64`) use the default resolution order (C++ first, then Rust)

Pass the full argument including any qualifier as the `target` (e.g. `target: "rust:base64"`).

If the argument includes "deep" or "transitive", pass `depth: 2` (or higher) to the tool. Otherwise use the default depth of 1 (direct dependents only). Warn the user that deeper queries are slower.

After receiving the tool output, add any useful observations:

- Is the dependency narrowly or broadly used?
- Are there surprising consumers?
- For C++ deps, are source files using the dependency via `deps` (public) or `implementation_deps` (hidden)?
  You can check this by reading the relevant `BUILD.bazel` files.
- For dependencies that appear unused, note whether they might be pulled in transitively.
