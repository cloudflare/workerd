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
  - Contains both C++ implementations and TypeScript definitions
  - Node.js compatibility layer in `api/node/`
  - Tests in `api/tests/` and `api/node/tests/`
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

- TypeScript definitions generated in `types/` directory

### Configuration System
- Uses **Cap'n Proto** for configuration files (`.capnp` format)
- Main schema: `src/workerd/server/workerd.capnp`
- Sample configurations in `samples/` directory
- Configuration uses capability-based security model

## Development Workflow

### Code Style & Formatting
- Automatic formatting via clang-format (enforced in CI)
- Run `just format` before committing
- Uses KJ C++ style guide (see Cap'n Proto project)

### Contributing
- Strong backwards compatibility commitment - features cannot be removed once deployed
- High bar for non-standard APIs; prefer implementing web standards
- Run tests with `just test` before submitting PRs

### Rust Development
- `just update-rust <package>` - Update Rust dependencies (equivalent to `cargo update`)
- `just clippy <package>` - Run clippy linting on Rust code

## NPM Package Management

- Uses **pnpm** for TypeScript/JavaScript dependencies
- Root package.json contains development dependencies
