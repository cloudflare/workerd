# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Instructions for Claude Code

- Look for high-level overview in `docs/` directory
- Check README.md files for package/directory level information
- Check source file comments for more detailed info
- Suggest updates to CLAUDE.md when you find new high-level information

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

## Testing

### Test Types & Commands
- **Unit Tests**: `.wd-test` files use Cap'n Proto config format
- **C++ Tests**: Traditional C++ unit tests
- **Node.js Compatibility**: `just node-test <test_name>`
- **Web Platform Tests**: `just wpt-test <test_name>`
- **Benchmarks**: `just bench <path>` (e.g., `just bench mimetype`)


## Architecture

- **Cap'n Proto source code** available in `external/capnp-cpp` - contains KJ C++ base library and
capnproto RPC library. Consult it for all questions about `kj/` and `capnproto/` includes and
`kj::` and `capnp::` namespaces.

### Core Directory Structure (`src/workerd/`)
- **`api/`** - Runtime APIs (HTTP, crypto, streams, WebSocket, etc.)
  - Contains both C++ implementations
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

## Backward Compatibility
- Strong backwards compatibility commitment - features cannot be removed or changed once deployed
- We use compatibility-date.capnp to introduce feature flags when we need to change the behavior

## Risky Changes
- We use autogates (defined in `src/workerd/util/autogate.*`) flags to make risky changes conditional for testing/slow rollout.

## Development Workflow

### Code Style & Formatting
- Automatic formatting via clang-format (enforced in CI)
- Run `just format` before committing

### Contributing
- High bar for non-standard APIs; prefer implementing web standards
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

## Visual Studio Code Setup

See [docs/vscode.md](docs/vscode.md) for instructions on setting up Visual Studio Code for workerd development, including build tasks, debugging configurations, and clangd integration for code completion and navigation.

## Other Documentation

See the markdown files in the `docs/` directory for additional information on specific topics:
- [development.md](docs/development.md) - Development environment setup and tools
- [api-updates.md](docs/api-updates.md) - Guidelines for adding new JavaScript APIs
- [pyodide.md](docs/pyodide.md) - Pyodide package management and updates

Some source directories also contain README.md files with more specific information about that component. Proactively look for these when working in unfamiliar areas of the codebase. Proactively suggest updates to the documentation when it is missing or out of date, but do not make edits without confirming accuracy.

