# WORKERD KNOWLEDGE BASE

**Generated:** 2026-02-21 **Commit:** d791daca6 **Branch:** jasnell/agent-refinements

## OVERVIEW

Cloudflare Workers JS/Wasm runtime. C++23/Bazel monorepo with TS, Rust, Python layers. Single binary (`workerd`) with subcommand dispatch (`serve`, `compile`, `test`). See also `CLAUDE.md` for build commands and contributing guidelines.

## STRUCTURE

```
src/workerd/           # Core C++ runtime (~375K lines)
  api/                 # JS APIs: HTTP, crypto, streams, WebSocket, Node.js compat (C++)
  io/                  # I/O lifecycle: IoContext, workers, actors, gates, compat flags
  jsg/                 # JavaScript Glue: V8 bindings, macros, type system
  server/              # Binary entry point, config parsing, server orchestration
  util/                # Shared: SQLite, state machine, weak refs, autogates
  tests/               # Benchmarks (bench-*.c++), test fixtures
  tools/               # Build-time: C++ AST param extractor
src/node/              # Node.js compat (TypeScript): public API + internal/
src/cloudflare/        # Cloudflare APIs (TypeScript): AI, D1, R2, Vectorize, etc.
src/pyodide/           # Python Workers via Pyodide
src/rust/              # 11 Rust crates: JSG bindings, transpiler, DNS, KJ bridge
build/                 # Custom Bazel rules (wd_cc_*, wd_ts_*, wd_test, etc.)
types/                 # TypeScript type generation from C++ RTTI
patches/               # Upstream patches: V8 (33), SQLite (4), BoringSSL (1), etc.
npm/                   # npm distribution: workerd + 5 platform packages
samples/               # Example configurations
```

## WHERE TO LOOK

| Task                   | Location                                                      | Notes                                                          |
| ---------------------- | ------------------------------------------------------------- | -------------------------------------------------------------- |
| Add/modify JS API      | `src/workerd/api/`                                            | C++ with JSG macros; see `jsg/jsg.h` for binding system        |
| Add Node.js compat     | `src/workerd/api/node/` (C++) + `src/node/` (TS)              | Dual-layer; register in `api/node/node.h` NODEJS_MODULES macro |
| Add Cloudflare API     | `src/cloudflare/`                                             | TypeScript; mock in `internal/test/<product>/`                 |
| Modify compat flags    | `src/workerd/io/compatibility-date.capnp`                     | ~1400 lines; annotations define flag names + enable dates      |
| Add autogate           | `src/workerd/util/autogate.h` + `.c++`                        | Enum + string map; both must stay in sync                      |
| Config schema          | `src/workerd/server/workerd.capnp`                            | Cap'n Proto; capability-based security                         |
| Worker lifecycle       | `src/workerd/io/worker.{h,c++}`                               | Isolate, Script, Worker, Actor classes                         |
| Request lifecycle      | `src/workerd/io/io-context.{h,c++}`                           | IoContext: the per-request god object                          |
| Durable Object storage | `src/workerd/io/actor-cache.{h,c++}` + `actor-sqlite.{h,c++}` | LRU cache over RPC / SQLite-backed                             |
| Streams implementation | `src/workerd/api/streams/`                                    | Has 842-line README; dual internal/standard impl               |
| Bazel build rules      | `build/`                                                      | Custom `wd_*` macros; `wd_test.bzl` generates 3 test variants  |
| TypeScript types       | `types/`                                                      | Extracted from C++ RTTI + hand-written `defines/*.d.ts`        |
| V8 patches             | `patches/v8/`                                                 | 33 patches; see `docs/v8-updates.md`                           |

## CONVENTIONS

- **C++ file ext**: `.c++` / `.h` (not `.cpp`); test suffix `-test` (hyphenated)
- **C++23**: `-std=c++23`; KJ style guide (from Cap'n Proto)
- **JSG binding pattern**: `JSG_RESOURCE_TYPE` for reference types, `JSG_STRUCT` for value types; `js.alloc<T>()` for allocation
- **Error handling**: `KJ_IF_SOME` (1400+ uses) for Maybe; `JSG_REQUIRE`/`JSG_FAIL_REQUIRE` for JS-facing errors with DOM exception types
- **Feature gating**: Compat flags (per-worker, date-driven, permanent) vs Autogates (per-process, config-driven, temporary)
- **Test format**: `.wd-test` = Cap'n Proto config; JS tests use named exports with `test()` methods + `node:assert`; each test auto-generates 3 variants (`@`, `@all-compat-flags`, `@all-autogates`)
- **Formatting**: `just format` runs clang-format + prettier + ruff + buildifier + rustfmt
- **Pre-commit hook**: Blocks `KJ_DBG` in staged code; runs format check
- **Commit discipline**: Split PRs into small commits; each must compile + pass tests; no fixup commits
- **TypeScript**: Strict mode, `exactOptionalPropertyTypes`, private `#` syntax enforced, explicit return types

## ANTI-PATTERNS (THIS PROJECT)

- **NEVER** put `v8::Local`/`v8::Global`/`JsValue` in `JSG_STRUCT` fields (use `jsg::V8Ref`/`jsg::JsRef`)
- **NEVER** put `v8::Global<T>` or `v8::Local<T>` in `kj::Promise` (compile-time deleted)
- **NEVER** pass `jsg::Lock` into KJ promise coroutines
- **NEVER** hold JS heap refs to KJ I/O objects without `IoOwn`; enforced by `DISALLOW_KJ_IO_DESTRUCTORS_SCOPE`
- **NEVER** use `JSG_INSTANCE_PROPERTY` without good reason (breaks GC optimization); prefer `JSG_PROTOTYPE_PROPERTY`
- **NEVER** call `recursivelyFreeze()` on user-provided content (unsafe for cyclic values)
- **NEVER** add new `Fetcher` methods without compat flag (conflicts with JS RPC wildcard)
- **NEVER** change `Headers::Guard` enum values (serialized)
- **NEVER** use `getWaitUntilTasks()` (use `addWaitUntil()`)
- `Ref<T>` stored in C++ objects visible from JS heap **MUST** implement `visitForGc()`; C++ reference cycles are **NEVER** collected
- SQLite `SQLITE_MISUSE` errors always throw (never suppressed); transactions disallowed in DO SQLite
- Module evaluation **MUST NOT** be in an IoContext; async I/O is **FORBIDDEN** in global scope

## COMMANDS

```bash
just build              # Build workerd binary
just test               # Run all tests
just format             # Format all code (required before commit)
just node-test <name>   # Run Node.js compat test (e.g., zlib)
just wpt-test <name>    # Run Web Platform Test (e.g., urlpattern)
just bench <name>       # Run benchmark (e.g., mimetype)
just clippy <pkg>       # Rust linter (e.g., jsg-macros)
just clang-tidy <tgt>   # C++ linter
just build-asan         # Build with AddressSanitizer
just test-asan          # Test with AddressSanitizer
```

## NOTES

- `build/` is build system definitions, NOT build output (output is `bazel-bin/`)
- Cap'n Proto source (`external/capnp-cpp`) provides KJ library (`kj::` namespace) — consult for all `kj::` questions
- Uses tcmalloc; memory analysis must account for its behavior
- `empty/` directory is a Bazel placeholder (single empty file)
- Can be a submodule of larger internal Cloudflare repo; check `../../CLAUDE.md` if so
- V8 updates require human judgment for merge conflicts; never auto-resolve
