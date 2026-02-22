---
description: Summarize recent commits — by count, date, or time range
subtask: true
---

Show what's new: $ARGUMENTS

**Parse the argument** to determine the commit range:

- A number (e.g., `10`) → the N most recent commits: `git log -<N>`
- `since <date>` (e.g., `since 2026-02-01`) → commits after that date: `git log --since='2026-02-01'`
- `today` → `git log --since='midnight'`
- `this week` → `git log --since='1 week ago'`
- `this month` → `git log --since='1 month ago'`
- `yesterday` → `git log --since='2 days ago' --until='midnight'`
- `last week` → `git log --since='2 weeks ago' --until='1 week ago'`
- No argument → default to `10`

Steps:

1. **Target the main branch at origin.** Always show what's new on the remote `main` branch,
   regardless of which branch is currently checked out. First, fetch the latest:

   ```
   git fetch origin main
   ```

   Then use `origin/main` as the ref for all subsequent git commands (instead of `HEAD`).

2. **Fetch the commits.** Run `git log` with the parsed range against `origin/main`:

   ```
   git log <range> origin/main --format='%h|%aN|%as|%s' --no-merges
   ```

   Also include merge commits separately if relevant:

   ```
   git log <range> origin/main --format='%h|%aN|%as|%s' --merges
   ```

3. **Get the diff stats** for scope:

   ```
   git diff --stat <oldest_commit>^..origin/main
   ```

4. **Categorize each commit** by the files it touches:
   - **API** — `src/workerd/api/` (excluding `node/`)
   - **Node.js compat** — `src/workerd/api/node/` or `src/node/`
   - **I/O** — `src/workerd/io/`
   - **JSG** — `src/workerd/jsg/`
   - **Server** — `src/workerd/server/`
   - **Build** — `build/`, `MODULE.bazel`, `BUILD.bazel`
   - **Types** — `types/`
   - **Cloudflare APIs** — `src/cloudflare/`
   - **Python** — `src/pyodide/`
   - **Rust** — `src/rust/`
   - **Docs / Config** — documentation, agent configs, `.md` files
   - **Tests only** — changes exclusively in test files

   For commits touching multiple areas, list under the primary area.

5. **Highlight notable changes.** Scan commit messages and diffs for:
   - New compatibility flags (additions to `compatibility-date.capnp`)
   - New autogates (additions to `autogate.h`)
   - Breaking changes or behavioral changes
   - New APIs or features
   - Security fixes
   - Dependency updates

6. **Output:**

   ```
   ## What's New (<range description>)

   **<N> commits** by <M> authors | <files changed> files changed, <insertions> insertions, <deletions> deletions

   ### Highlights
   - <notable changes, if any>

   ### API
   - `<hash>` <summary> — <author>

   ### Node.js compat
   - `<hash>` <summary> — <author>

   ...additional categories as needed (omit empty categories)...
   ```

   Keep summaries to one line per commit. Group related commits (e.g., a feature + its fixup) into a single entry where obvious.
