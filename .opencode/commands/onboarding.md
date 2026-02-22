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
   - `/onboarding build` — Bazel, dependencies, test formats
   - `/onboarding server` — Config, services, networking
   - `/onboarding <area>` — Guided walkthrough of a specific area

   Keep this concise — just the menu, don't explain each area in detail.

**If an argument is provided, give a guided walkthrough of that area:**

### General approach for all areas

1. **Read context**: Read the relevant `AGENTS.md` file for the area (e.g., `src/workerd/api/streams/AGENTS.md` for streams). Also check for a `README.md` in the directory.
2. **Explain the subsystem**: What it does, why it exists, how it fits into the broader architecture. Keep it to 2-3 paragraphs.
3. **Key classes and files**: List the 5-10 most important classes/files with one-line descriptions and file paths.
4. **Concrete example**: Walk through one specific flow end-to-end (e.g., "here's what happens when a fetch() response body is read as a stream").
5. **Key patterns to know**: Patterns specific to this area that a newcomer needs to understand (e.g., `IoOwn` for io, JSG macros for jsg, compat flag gating for api).
6. **Tests to read**: Point to 2-3 representative tests that demonstrate how the code works.
7. **Further reading**: Suggest related `/onboarding <area>` topics, relevant `/explain` targets, or docs.

### Area-specific sources

| Area           | Primary sources                                                                                                                                                                                                                                                                                                               |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `vscode`       | `docs/vscode.md` — devcontainer setup, recommended extensions (clangd, C/C++, capnproto-syntax, GitLens, markdownlint), VSCode tasks (build/test/clean), debug launch targets (workerd dbg, inspector, test case, wd-test case), clangd configuration (`compile_flags.txt` or dependency files), `tools/unix/clangd-check.sh` |
| `architecture` | `CLAUDE.md`, root `AGENTS.md`, all subdirectory `AGENTS.md` files — explain the layers: `server/` (config, networking) → `io/` (IoContext, worker lifecycle) → `api/` (JS APIs) → `jsg/` (V8 bindings), plus `util/` (shared utilities)                                                                                       |
| `api`          | `src/workerd/api/AGENTS.md`, `src/workerd/api/BUILD.bazel` — the JS API surface, `global-scope.h` as the entry point, how APIs are registered                                                                                                                                                                                 |
| `streams`      | `src/workerd/api/streams/AGENTS.md`, `src/workerd/api/streams/README.md` — dual controller architecture (internal vs standard), the key classes                                                                                                                                                                               |
| `node`         | `src/node/AGENTS.md`, `src/workerd/api/node/AGENTS.md` — three-tier module system (C++ → TS internal → TS public), `NODEJS_MODULES` macro, compat flag gating                                                                                                                                                                 |
| `io`           | `src/workerd/io/AGENTS.md` — IoContext, IoOwn, DeleteQueue, InputGate/OutputGate, Worker::Actor                                                                                                                                                                                                                               |
| `actors`       | `src/workerd/io/AGENTS.md`, `src/workerd/api/actor-state.h` — Actor lifecycle, ActorCache vs ActorSqlite, gates, hibernation                                                                                                                                                                                                  |
| `jsg`          | `src/workerd/jsg/AGENTS.md`, `src/workerd/jsg/README.md` — JSG_RESOURCE_TYPE, JSG_METHOD, type mapping, V8 integration                                                                                                                                                                                                        |
| `build`        | `build/AGENTS.md`, `CLAUDE.md` — Bazel targets, `just` commands, `wd_test`/`kj_test` macros, dependency management (`MODULE.bazel`, `build/deps/`)                                                                                                                                                                            |
| `server`       | `src/workerd/server/AGENTS.md` — workerd.capnp config, Server class, service setup, networking                                                                                                                                                                                                                                |
