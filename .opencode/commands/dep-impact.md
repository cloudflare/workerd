---
description: Analyze dependency changes in a PR or diff and identify impacted code
subtask: true
---

Analyze dependency changes and their impact on the codebase.

**Arguments:** $ARGUMENTS

## How to determine the diff

1. If arguments contain a PR number or URL, use `gh pr diff <number>` to get the changes.
2. If no PR is specified, use `git diff origin/main...HEAD` for local branch changes.
3. If a specific commit range is given, use `git diff <range>`.

## Step 1: Identify dependency changes

Scan the diff for changes to files that indicate dependency updates:

| File pattern                                     | Dependency type                                     |
| ------------------------------------------------ | --------------------------------------------------- |
| `MODULE.bazel`                                   | Bazel module dependencies (version bumps, new deps) |
| `build/deps/**`                                  | Dependency configuration and BUILD overlays         |
| `deps/rust/crates/BUILD.*.bazel`                 | Rust crate additions, removals, or version bumps    |
| `deps/rust/Cargo.lock`, `deps/rust/Cargo.toml`   | Rust dependency tree changes                        |
| `patches/**`                                     | Patches applied to vendored dependencies            |
| `package.json`, `pnpm-lock.yaml`                 | JavaScript/TypeScript dependencies                  |
| `BUILD.bazel` (deps/implementation_deps changes) | Internal dependency wiring changes                  |

For each changed dependency, extract:

- **Name** of the dependency
- **Change type**: version bump, added, removed, or patch modified
- **Old → New version** (if applicable)

If no dependency changes are found in the diff, say so and exit early.

## Step 2: Map impact using bazel-deps

For each changed dependency, use the `bazel-deps` tool with `direction: "rdeps"` to find what workerd code depends on it.

- For C++ dependencies: pass the dependency name as-is (e.g., `target: "ada-url"`)
- For Rust crates: use the `rust:` qualifier (e.g., `target: "rust:base64"`)
- For dependencies with patches changed: the dependency name is typically the directory name under `patches/` (e.g., `patches/v8/` → `target: "v8"`)

Run these lookups in parallel when there are multiple dependency changes.

## Step 3: Assess risk and summarize

For each dependency change, assess:

- **Blast radius**: How many components are affected? (narrow = 1-2, moderate = 3-5, broad = 6+)
- **Risk level**:
  - **HIGH**: Major version bump, patch modifications, security-sensitive dep (ssl, crypto), broad blast radius, or a dependency that touches V8/memory management
  - **MEDIUM**: Minor version bump with moderate blast radius, new dependency added, or changes to build configuration
  - **LOW**: Patch version bump, narrow blast radius, leaf dependency with no transitive impact
- **Review focus**: What specific areas of the codebase should reviewers pay extra attention to?

### Output format

```
## Dependency Impact Analysis

### Summary
- N dependency changes detected
- Overall risk: HIGH/MEDIUM/LOW
- Components affected: list

### Changes

#### 1. dep-name: old-version → new-version [RISK]
- **Change**: description of what changed
- **Blast radius**: N targets across M components
- **Impacted components**:
  - component-a/ (N targets) — brief description of what uses this dep
  - component-b/ (N targets)
- **Review focus**: What to look for in these components
- **Patch changes** (if applicable): Summary of what changed in patches

### Recommendations
- Prioritized list of review actions
- Specific test targets to run
- Any compatibility concerns
```

If the change includes patch file modifications (under `patches/`), read the patch diff carefully and summarize what changed — these are often the highest-risk part of a dependency update since they represent custom modifications to upstream code that must be maintained across versions.
