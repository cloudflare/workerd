# AGENTS.md

This file provides guidance to Claude Code (claude.ai/code) or Opencode (opencode.ai) when working with code in this repository.

Subdirectory `AGENTS.md` files provide component-specific context (key classes, where-to-look tables, local conventions and anti-patterns).

## Instructions for AI Code Assistants

- Suggest updates to AGENTS.md when you find new high-level information
- You should always determine if the current repository was checked out standalone or as a submodule
  of the larger workers project.
- If checked out as a submodule, be aware that there is additional documentation and context in the
  root of that repository that is not present here. Look for the `../../README.md`, `../../AGENTS.md`,
  and other markdown files in the root of the parent repository.

## Project Overview

**workerd** is Cloudflare's JavaScript/WebAssembly server runtime that powers Cloudflare Workers. It's an open-source implementation of the same technology used in production at Cloudflare, designed for self-hosting applications, local development, and programmable HTTP proxy functionality.

## Build System & Commands

### Primary Build System: Bazel

- Main build command: `bazel build //src/workerd/server:workerd`
- Binary output: `bazel-bin/src/workerd/server/workerd`

### Just Commands (recommended for development)

- `just build` or `just b` - Build the project
- `just test` or `just t` - Run all tests
- `just format` or `just f` - Format code (uses clang-format + Python formatter)
- `just clippy <package>` - Run Rust clippy linter (e.g., `just clippy jsg-macros`)
- `just clang-tidy <target>` - Run clang-tidy on C++ code (e.g., `just clang-tidy //src/rust/jsg:ffi`)
- `just stream-test <target>` - Stream test output for debugging
- `just node-test <name>` - Run specific Node.js compatibility tests (e.g., `just node-test zlib`)
- `just wpt-test <name>` - Run Web Platform Tests (e.g., `just wpt-test urlpattern`)
- `just generate-types` - Generate TypeScript definitions
- `just compile-commands` - Generate compile_commands.json for clangd support
- `just build-asan` - Build with AddressSanitizer
- `just test-asan` - Run tests with AddressSanitizer
- `just new-test <target>` - Scaffold a new test (e.g., `just new-test //src/workerd/api/tests:my-test`)
- `just new-wpt-test <name>` - Scaffold a new WPT test
- `just lint` or `just eslint` - Run ESLint on TypeScript sources
- `just coverage <path>` - Generate code coverage report (Linux only, defaults to `//...`)
- `just watch <args>` - Watch `src/` and `build/` dirs, re-run a just command on changes

## Testing

### Test Types

- **`.wd-test` tests**: Cap'n Proto config files that define a `Workerd.Config` with embedded JS/TS modules. Bazel macro: `wd_test()`. See format details below.
- **C++ tests**: KJ-based unit tests (`.c++` files). Bazel macro: `kj_test()`.
- **Node.js compatibility tests**: `just node-test <test_name>`
- **Web Platform Tests**: `just wpt-test <test_name>`
- **Benchmarks**: `just bench <path>` (e.g., `just bench mimetype`)

### Running a Single Test

Both `just test` and `just build` accept specific Bazel targets (they default to `//...`):

```
just test //src/workerd/api/tests:encoding-test@
just test //src/workerd/io:io-gate-test@
just stream-test //src/workerd/api/tests:encoding-test@    # streams output for debugging
```

Or use Bazel directly:

```
bazel test //src/workerd/api/tests:encoding-test@
```

### Test Variants

Every test automatically generates multiple variants via the build macros:

- **`name@`** — default variant (oldest compat date, 2000-01-01)
- **`name@all-compat-flags`** — newest compat date (2999-12-31), tests with all flags enabled
- **`name@all-autogates`** — all autogates enabled + oldest compat date

The `@` suffix is required in target names. For example: `//src/workerd/io:io-gate-test@`, not `//src/workerd/io:io-gate-test`.

To find the right target name for a file, check the `BUILD.bazel` file in the same directory for `wd_test()` or `kj_test()` rules. You can also use Bazel query:

```
bazel query //src/workerd/api/tests:all    # list all targets in a package
```

### `.wd-test` File Format

`.wd-test` files are Cap'n Proto configs that define test workers:

```capnp
using Workerd = import "/workerd/workerd.capnp";

const unitTests :Workerd.Config = (
  services = [(
    name = "my-test",
    worker = (
      modules = [(name = "worker", esModule = embed "my-test.js")],
      compatibilityDate = "2024-01-01",
      compatibilityFlags = ["nodejs_compat"],
    ),
  )],
);
```

Key elements: `modules` (embed JS/TS files), `compatibilityFlags`, `bindings` (service bindings, JSON, KV, etc.), `durableObjectNamespaces`.

## Architecture

### Dependencies

- **Cap'n Proto source code** available in `external/capnp-cpp` - contains KJ C++ base library and
  capnproto RPC library. Consult it for all questions about `kj/` and `capnproto/` includes and
  `kj::` and `capnp::` namespaces.

The other core runtime dependencies include:

| Dependency                          | Description                                                         |
| ----------------------------------- | ------------------------------------------------------------------- |
| V8                                  | JavaScript engine                                                   |
| Cap'n Proto (capnp-cpp)             | Serialization/RPC framework and KJ base library                     |
| BoringSSL                           | TLS/crypto (Google's OpenSSL fork, patched for ncrypto/libdecrepit) |
| SQLite3                             | Embedded database                                                   |
| ICU (com_googlesource_chromium_icu) | Internationalization (Chromium fork)                                |
| zlib                                | Compression (Chromium fork, patched)                                |
| zstd                                | Zstandard compression                                               |
| brotli                              | Brotli compression                                                  |
| tcmalloc                            | Memory allocator                                                    |
| ada-url                             | URL parser                                                          |
| simdutf                             | Unicode transcoding (SIMD-accelerated)                              |
| nbytes                              | Node.js byte utilities                                              |
| ncrypto                             | Node.js crypto utilities                                            |
| perfetto                            | Tracing/profiling framework (patched)                               |
| fast_float                          | Fast float parsing                                                  |
| fp16                                | Half-precision float support                                        |
| highway                             | SIMD abstraction library                                            |
| dragonbox                           | Float-to-string conversion                                          |

These dependencies are vendored via Bazel into the `external/` directory. See `MODULE.bazel` and the `build/deps/` directory for how they are integrated into the build system. (The project uses bzlmod; the legacy `WORKSPACE` file may still exist but is no longer the primary mechanism.)

For several of these dependencies (notably V8, boringssl, sqlite, perfetto, and zlib), we maintain sets of patches that are applied on top of the upstream code. These patches are stored in the `patches/` directory and are applied during the build process. When updating these dependencies, it's important to review and update the corresponding patches as needed. The patches may introduce workerd-specific customizations and new APIs.

Be aware that workerd uses tcmalloc for memory allocation in the typical case. When analyzing memory usage or debugging memory issues, be aware that tcmalloc's behavior may differ from the standard allocator. Any memory usage analysis that you perform should take this into account.

### Core Directory Structure (`src/workerd/`)

- **`api/`** - Runtime APIs (HTTP, crypto, streams, WebSocket, etc.)
  - Contains C++ implementations of the core APIs exposed to JavaScript, as well as the Node.js compatibility layer
  - C++ portions of the Node.js compatibility layer are in `api/node/`, while the JavaScript and TypeScript implementations live in `src/node/`
  - Tests in `api/tests/` and `api/node/tests/`
  - TypeScript definitions are derived from C++ (which can have some annotations). This generation is handled by code in `types/` directory.

- **`io/`** - I/O subsystem, actor storage, threading, worker lifecycle
  - Actor storage and caching (`actor-cache.c++`, `actor-sqlite.c++`)
  - Request tracking and limits (`request-tracker.c++`, `limit-enforcer.h`)
- **`jsg/`** - JavaScript Glue layer for V8 integration
  - Core JavaScript engine bindings and type wrappers
  - Promise handling, memory management, module system
- **`server/`** - Main server implementation and configuration
  - Main binary entry point and Cap'n Proto config handling
- **`util/`** - Utility libraries (SQLite, UUID, threading, etc.)

### Multi-Language Support

- **`src/cloudflare/`** - Cloudflare-specific APIs (TypeScript)
- **`src/node/`** - Node.js compatibility layer (TypeScript)
- **`src/pyodide/`** - Python runtime support via Pyodide
- **`src/rust/`** - Rust integration components

### Configuration System

- Uses **Cap'n Proto** for configuration files (`.capnp` format)
- Main schema: `src/workerd/server/workerd.capnp`
- Sample configurations in `samples/` directory
- Configuration uses capability-based security model

### Where to Look

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

## Coding Conventions

This project generally follows the [KJ Style Guide](https://github.com/capnproto/capnproto/blob/v2/kjdoc/style-guide.md) and [KJ Tour](https://github.com/capnproto/capnproto/blob/v2/kjdoc/tour.md), with one exception: comment style follows the more common idiomatic C++ patterns (e.g., `//` line comments) rather than KJ's comment conventions.

- **C++ standard**: C++23 (`-std=c++23`)
- **C++ file extensions**: `.c++` / `.h` (not `.cpp`); test suffix `-test` (hyphenated)
- **Formatting**: `just format` runs clang-format + prettier + ruff + buildifier + rustfmt
- **Pre-commit hook**: Blocks `KJ_DBG` in staged code; runs format check
- **Commit discipline**: Split PRs into small commits; each must compile + pass tests; no fixup commits
- **TypeScript**: Strict mode, `exactOptionalPropertyTypes`, private `#` syntax enforced, explicit return types

### Use KJ types, not STL

This project uses the KJ library instead of the C++ standard library for most types:

| Instead of              | Use                                                 |
| ----------------------- | --------------------------------------------------- |
| `std::string`           | `kj::String` (owned) / `kj::StringPtr` (view)       |
| `std::vector`           | `kj::Array<T>` (fixed) / `kj::Vector<T>` (growable) |
| `std::unique_ptr`       | `kj::Own<T>`                                        |
| `std::shared_ptr`       | `kj::Rc<T>` / `kj::Arc<T>` (thread-safe)            |
| `std::optional`         | `kj::Maybe<T>`                                      |
| `std::function`         | `kj::Function<T>`                                   |
| `std::variant`          | `kj::OneOf<T...>`                                   |
| `std::span` / array ref | `kj::ArrayPtr<T>`                                   |
| `std::exception`        | `kj::Exception`                                     |
| `std::promise`/`future` | `kj::Promise<T>` / `kj::ForkedPromise<T>`           |

### Error Handling

- `KJ_IF_SOME` for unwrapping `kj::Maybe` (1400+ uses across the codebase)
- `JSG_REQUIRE` / `JSG_FAIL_REQUIRE` for JS-facing errors with DOM exception types
- `KJ_ASSERT` / `KJ_REQUIRE` / `KJ_FAIL_ASSERT` for C++ assertions and preconditions

### JSG (JavaScript Glue)

C++ classes are exposed to JavaScript via JSG macros in `src/workerd/jsg/`. See the comprehensive guide at `src/workerd/jsg/README.md` for details. When adding or modifying JavaScript APIs, find a similar existing API and follow its pattern.

- `JSG_RESOURCE_TYPE` for reference types, `JSG_STRUCT` for value types
- `js.alloc<T>()` for resource allocation

### Feature Management

- **Compatibility flags** (`src/workerd/io/compatibility-date.capnp`) — per-worker, date-driven, permanent. Flags MUST be documented before their enable date.
- **Autogates** (`src/workerd/util/autogate.*`) — per-process, config-driven, temporary. For risky rollouts with conditional activation.

## Anti-Patterns

- **NEVER** put `v8::Local`/`v8::Global`/`JsValue` in `JSG_STRUCT` fields (use `jsg::V8Ref`/`jsg::JsRef`)
- **NEVER** put `v8::Global<T>` or `v8::Local<T>` in `kj::Promise` (compile-time deleted)
- **NEVER** pass `jsg::Lock` into KJ promise coroutines
- **NEVER** hold JS heap refs to KJ I/O objects without `IoOwn`; enforced by `DISALLOW_KJ_IO_DESTRUCTORS_SCOPE`
- **NEVER** use `JSG_INSTANCE_PROPERTY` without good reason (breaks GC optimization); prefer `JSG_PROTOTYPE_PROPERTY`
- **NEVER** call `recursivelyFreeze()` on user-provided content (unsafe for cyclic values)
- **NEVER** add new `Fetcher` methods without compat flag (conflicts with JS RPC wildcard)
- **NEVER** change `Headers::Guard` enum values (serialized)
- **NEVER** use `getWaitUntilTasks()` (use `addWaitUntil()`)
- **NEVER** use boolean arguments; prefer `WD_STRONG_BOOL`
- `Ref<T>` stored in C++ objects visible from JS heap **MUST** implement `visitForGc()`; C++ reference cycles are **NEVER** collected
- SQLite `SQLITE_MISUSE` errors always throw (never suppressed); transactions disallowed in DO SQLite
- Module evaluation **MUST NOT** be in an IoContext; async I/O is **FORBIDDEN** in global scope

## Backward Compatibility

- Strong backwards compatibility commitment - features cannot be removed or changed once deployed
- We use compatibility-date.capnp to introduce feature flags when we need to change the behavior

## Development Workflow

### Contributing

- High bar for non-standard APIs; prefer implementing web standards
- Run formatting with `just format` before submitting PRs
- Run tests with `just test` before submitting PRs
- See `CONTRIBUTING.md` for more details

### Rust Development

- `just update-rust <package>` - Update Rust dependencies (equivalent to `cargo update`)
- `just clippy <package>` - Run clippy linting on Rust code

## NPM Package Management

- Uses **pnpm** for TypeScript/JavaScript dependencies
- Root package.json contains development dependencies

## V8 Updates

See [docs/v8-updates.md](docs/v8-updates.md) for instructions on updating the V8 engine version used by workerd. These steps include syncing the V8 source, applying workerd patches, rebasing onto the new version, regenerating patches, and updating dependency versions in Bazel files.

When updating V8, ensure that all tests pass. Look for new deprecations when building and flag those for users if necessary.

If asked to help with a V8 update, ask for the specific target V8 version to update to and ask clarifying questions about any specific patches or customizations that need to be preserved before proceeding. Merge conflicts are common during V8 updates, so be prepared to resolve those carefully. These almost always require human judgment to ensure that workerd-specific changes are preserved while still applying the upstream V8 changes correctly. Do not attempt to resolve merge conflicts automatically without human review.

## Other Documentation

See the markdown files in the `docs/` directory for additional information on specific topics:

- [development.md](docs/development.md) - Development environment setup and tools
- [api-updates.md](docs/api-updates.md) - Guidelines for adding new JavaScript APIs
- [pyodide.md](docs/pyodide.md) - Pyodide package management and updates

Some source directories also contain README.md files with more specific information about that component. Proactively look for these when working in unfamiliar areas of the codebase. Proactively suggest updates to the documentation when it is missing or out of date, but do not make edits without confirming accuracy.
