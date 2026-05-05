---
name: commit-categories
description: Commit categorization rules for changelogs and "what's new" summaries. MUST be loaded before categorizing commits in changelog or whats-new commands. Provides the canonical path-based category table used to group commits by area.
---

# Commit Categories

Categorize commits by the files they touch, using the primary area for commits spanning multiple categories.

| Category            | Path patterns                                         |
| ------------------- | ----------------------------------------------------- |
| **API**             | `src/workerd/api/` (excluding `node/` and `pyodide/`) |
| **Node.js compat**  | `src/workerd/api/node/`, `src/node/`                  |
| **Python**          | `src/workerd/api/pyodide/`, `src/pyodide/`            |
| **Rust**            | `src/rust/`                                           |
| **Cloudflare APIs** | `src/cloudflare/`                                     |
| **I/O**             | `src/workerd/io/`                                     |
| **JSG**             | `src/workerd/jsg/`                                    |
| **Server**          | `src/workerd/server/`                                 |
| **Build**           | `build/`, `MODULE.bazel`, `BUILD.bazel`               |
| **Types**           | `types/`                                              |
| **Docs / Config**   | Documentation, agent/tool configs, `.md` files        |
| **Tests**           | Changes exclusively in test files                     |
| **Other**           | Anything that doesn't fit above                       |

## Cross-cutting callouts

These are **not** primary categories — they are additional callout sections that appear alongside the main categories whenever a commit touches the relevant files. A single commit can appear in both a primary category and one or more callouts.

| Callout                      | Trigger                                                                                             |
| ---------------------------- | --------------------------------------------------------------------------------------------------- |
| **New/Updated Compat Flags** | Changes to `src/workerd/io/compatibility-date.capnp` or new `compatibilityFlags` references in code |
| **New/Updated Autogates**    | Changes to `src/workerd/io/supported-autogates.h` or new autogate registrations                     |

When either callout applies, add a dedicated section **after** the main categories listing each new or modified flag/gate with a brief description. These must never be buried inside a general category bullet — they are high-visibility items that reviewers and release-note readers need to spot immediately.

## How to categorize

1. For each commit, run `git diff-tree --no-commit-id --name-only -r <hash>` to list files changed.
2. Match changed files against the path patterns above.
3. Assign the commit to whichever category covers the majority of its changes.
4. For commits touching multiple areas, list under the **primary** area (the one with the most changed files or the most significant change).
5. Check every commit against the cross-cutting callout triggers. If a commit adds or modifies compat flags or autogates, note it in the corresponding callout section **in addition to** its primary category.
6. Omit empty categories and unused callout sections from output.
