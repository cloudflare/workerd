---
description: Guided onboarding to the workerd codebase, or a specific area
subtask: true
---

Onboard: $ARGUMENTS

**If no argument is provided (empty or blank), present the project basics and available areas:**

1. **What is workerd**: Cloudflare Workers JavaScript/WebAssembly runtime. C++23 monorepo built with Bazel, using V8 for JS execution.

2. **How to build and test**:
   - Build: `just build`
   - Run all tests: `just test`
   - Run a single test with live output: `just stream-test <bazel target>`
   - Run a sample: `bazel run //src/workerd/server:workerd -- serve $(pwd)/samples/<name>/config.capnp`

3. **AI tools available to help you**: Read the `.opencode/` directory and list:
   - **Agents** (from `.opencode/agent/`): list each with a one-line description
   - **Commands** (from `.opencode/commands/`): list each with a one-line description (e.g., `/explain`, `/find-owner`, `/trace`, `/run`, etc.)
   - **Skills** (from `.opencode/skills/`): list each with a one-line description (e.g., `kj-style`, `add-compat-flag`, `update-v8`, etc.)

4. **What do you want to learn about?** Present areas as a menu:
   - `/onboarding vscode` — Editor setup, extensions, debugging, clangd
   - `/onboarding architecture` — Layers, request flow, key abstractions
   - `/onboarding api` — JavaScript APIs (HTTP, crypto, WebSocket, etc.)
   - `/onboarding streams` — ReadableStream/WritableStream/TransformStream
   - `/onboarding node` — Node.js compatibility layer
   - `/onboarding io` — I/O context, ownership, worker lifecycle
   - `/onboarding actors` — Durable Objects, storage, gates
   - `/onboarding jsg` — V8 bindings, JSG macros, type registration
   - `/onboarding rust` — Rust crates, CXX FFI, Rust JSG bindings, proc macros
   - `/onboarding just` — `just` command runner: available recipes and aliases
   - `/onboarding build` — Bazel, dependencies, test formats
   - `/onboarding server` — Config, services, networking
   - `/onboarding ai` — Custom AI commands, skills, agents, and tools available in this project
   - `/onboarding <area>` — Guided walkthrough of a specific area

   Keep this concise — just the menu, don't explain each area in detail.

**If an argument is provided, give a guided walkthrough of that area:**

### Special area: `ai`

If the argument is `ai`, dynamically discover and present all custom AI tooling configured for this project. Do NOT hard-code any names or descriptions — read them from the filesystem so the output stays current as files are added or removed.

**Discovery steps:**

1. **Commands** — List `.opencode/commands/`. For each `.md` file, read the YAML frontmatter and extract the `description` field. Present as a table: command name (derived from filename without `.md`, prefixed with `/`) and description.

2. **Skills** — List `.opencode/skills/`. For each subdirectory, read `SKILL.md` inside it and extract the `name` and `description` from the YAML frontmatter. Present as a table: skill name and description.

3. **Agents** — List `.opencode/agent/`. For each `.md` file, read the YAML frontmatter and extract the `description` and `mode` fields. Present as a table: agent name (derived from filename without `.md`), mode, and description.

4. **Tools** — List `.opencode/tools/`. For each `.ts` file (excluding files that don't export a `tool()`), read the file and extract the `description` string from the `tool({description: ...})` call. Present as a table: tool name (derived from filename without `.ts`) and description.

**Output format:**

Present each category with a brief intro explaining what it is and how to use it:

- **Commands**: Invoked with `/command-name [args]` in the chat. These are project-specific slash commands.
- **Skills**: Loaded automatically or on demand to provide domain-specific instructions. Users don't invoke these directly — the AI loads them when relevant.
- **Agents**: Specialized AI modes with different permissions and focuses (e.g., read-only architect vs. code-writing build agent).
- **Tools**: Custom tools the AI can call during tasks. Users don't invoke these directly — the AI uses them as needed.

After presenting all four categories, suggest a few "what next" pointers (e.g., "try `/onboarding architecture` to learn the codebase structure" or "try `/explain <class>` to understand a specific class").

Skip the general approach steps below — this area does not involve a code walkthrough.

### Special area: `just`

If the argument is `just`, read the `justfile` at the project root and present a quick reference of the `just` command runner and all available recipes.

**Output format:**

1. **Brief intro**: Explain that workerd uses [`just`](https://github.com/casey/just) as a command runner (like `make` but simpler, no build graph, just commands). Mention that running `just` with no arguments lists all recipes.

2. **Recipes table**: Read the `justfile` and present all recipes organized by category. For each recipe, show the command (including aliases where defined), parameters with defaults, and a short description (derived from comments or the recipe body). Group into logical categories (e.g., "Core workflows", "Testing shortcuts", "Sanitizers & analysis", "Benchmarks & profiling", "Setup & maintenance").

3. **Tips**: Include a few practical tips:
   - Aliases (e.g., `just b` = `just build`)
   - `just watch <recipe>` to auto-rerun on file changes
   - Default args (e.g., `just build` builds everything, `just build //src/workerd/api/...` builds a subtree)

Skip the general approach steps below — this area does not involve a code walkthrough.

### General approach for all areas

The goal is orientation, not reference. Keep the reader focused on what they need to understand first — not everything that exists. For exhaustive detail (full API surfaces, complete file listings, build targets), defer to `/explain`.

1. **Read context**: Read the relevant `AGENTS.md` file for the area (e.g., `src/workerd/api/streams/AGENTS.md` for streams). Also check for a `README.md` in the directory.
2. **Explain the subsystem**: What it does, why it exists, how it fits into the broader architecture. Keep it to 2-3 paragraphs. Stick to facts — don't editorialize or speculate.
3. **Key classes and files**: List the 3-5 most important classes/files with one-line descriptions and file paths. Focus on what to read first, not everything. For the complete listing, suggest `/explain <area>`.
4. **Concrete example**: Walk through one specific flow end-to-end (e.g., "here's what happens when a fetch() response body is read as a stream"). **This is the most important section** — it's what builds intuition. Make it the longest and most detailed part of the output.
5. **Key patterns to know**: Patterns specific to this area that a newcomer needs to understand (e.g., `IoOwn` for io, JSG macros for jsg, compat flag gating for api).
6. **Tests to read**: Point to 2-3 representative tests that demonstrate how the code works.
7. **Further reading**: Suggest related `/onboarding <area>` topics for adjacent areas. For detailed reference on specific classes or files mentioned above, point to `/explain <target>`.
8. **Never** miss an opportunity for a good dad joke (using the `dad-jokes` skill). Don't overdo it, but don't avoid them either. When summarizing, **always** preserve any jokes from the subagent output, and **always** including the intro prefix ("Here's a dad joke for you:", etc.) so the user knows it's intentional.

### Area-specific sources

Always apply the general approach above, but use these specific sources for each area:

| Area           | Primary sources                                                                                                                                                                                                                                                                                                               |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `vscode`       | `docs/vscode.md` — devcontainer setup, recommended extensions (clangd, C/C++, capnproto-syntax, GitLens, markdownlint), VSCode tasks (build/test/clean), debug launch targets (workerd dbg, inspector, test case, wd-test case), clangd configuration (`compile_flags.txt` or dependency files), `tools/unix/clangd-check.sh` |
| `architecture` | `AGENTS.md`, root `AGENTS.md`, all subdirectory `AGENTS.md` files — explain the layers: `server/` (config, networking) → `io/` (IoContext, worker lifecycle) → `api/` (JS APIs) → `jsg/` (V8 bindings), plus `util/` (shared utilities)                                                                                       |
| `api`          | `src/workerd/api/AGENTS.md`, `src/workerd/api/BUILD.bazel` — the JS API surface, `global-scope.h` as the entry point, how APIs are registered                                                                                                                                                                                 |
| `streams`      | `src/workerd/api/streams/AGENTS.md`, `docs/streams.md` — dual controller architecture (internal vs standard), tutorial walkthrough with data flow diagrams                                                                                                                                                                    |
| `node`         | `src/node/AGENTS.md`, `src/workerd/api/node/AGENTS.md` — three-tier module system (C++ → TS internal → TS public), `NODEJS_MODULES` macro, compat flag gating                                                                                                                                                                 |
| `io`           | `src/workerd/io/AGENTS.md` — IoContext, IoOwn, DeleteQueue, InputGate/OutputGate, Worker::Actor                                                                                                                                                                                                                               |
| `actors`       | `src/workerd/io/AGENTS.md`, `src/workerd/api/actor-state.h` — Actor lifecycle, ActorCache vs ActorSqlite, gates, hibernation                                                                                                                                                                                                  |
| `jsg`          | `src/workerd/jsg/AGENTS.md`, `docs/jsg.md` — JSG_RESOURCE_TYPE, JSG_METHOD, type mapping, V8 integration                                                                                                                                                                                                                      |
| `rust`         | `src/rust/AGENTS.md`, `src/rust/jsg/README.md`, `src/rust/jsg-macros/README.md` — CXX FFI bridges, Rust JSG bindings (`#[jsg_resource]`, `#[jsg_method]`), proc macros, crate organization, `just clippy`, error handling patterns                                                                                            |
| `build`        | `build/AGENTS.md`, `AGENTS.md` — Bazel targets, `just` commands, `wd_test`/`kj_test` macros, dependency management (`MODULE.bazel`, `build/deps/`)                                                                                                                                                                            |
| `server`       | `src/workerd/server/AGENTS.md` — workerd.capnp config, Server class, service setup, networking                                                                                                                                                                                                                                |
