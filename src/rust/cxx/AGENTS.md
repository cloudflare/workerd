# src/rust/cxx/

## Overview

This directory contains workerd's in-tree fork of cxx-rs. It was imported from the former
`cloudflare/workerd-cxx` repository. Changes to the fork and its workerd consumers should use the
in-tree Bazel labels and land atomically.

The fork adds KJ exceptions, smart pointers, data types, and bidirectional async interop. Do not
assume stock cxx-rs behavior when changing bridge generation or runtime code.

## Build and test

Run commands from the workerd repository root:

```sh
bazel build //src/rust/cxx/...
bazel test //src/rust/cxx/...
```

Important targets:

- `//src/rust/cxx:cxx` — Rust runtime crate
- `//src/rust/cxx:core` — C++ runtime and public header
- `//src/rust/cxx:codegen` — C++ bridge generator
- `//src/rust/cxx/kj-rs` — KJ integration crate and C++ support library

Dependencies come from workerd's `@crates_vendor` repository and `@capnp-cpp`; do not add a nested
Bazel module, Cargo workspace, toolchain configuration, or external `workerd-cxx` repository.

## Architecture

- `src/` and `include/` — cxx Rust and C++ runtimes
- `syntax/`, `gen/`, and `macro/` — bridge parser and code generators
- `kj-rs/` — KJ promises/futures, exceptions, ownership, refcounting, dates, and `Maybe`
- `kj-rs-tokio/` — `TokioEventPort`: a `kj::EventPort` backed by a per-thread tokio
  `current_thread` runtime, plus `setupTokioAsyncIo()` (no I/O providers) and
  `kj_rs_tokio::spawn()`
- `kj-rs-io/` — KJ async I/O interfaces over tokio (`kj::AsyncIoStream`, listeners,
  `kj::Network`, providers, signals, file watching), `kj_rs_io::setupTokioAsyncIo()` as the
  drop-in `kj::setupAsyncIo()` replacement, the stream unwrap fast path, and
  `serve_kj_stream()` for Rust servers consuming KJ streams
- `tests/` and `kj-rs/tests/` — Rust and C++ bridge integration tests
- `tools/bazel/` — Bazel bridge-generation macro used by this component's tests

## Conventions

- Follow the parent `src/rust/AGENTS.md` and repository `AGENTS.md`.
- Prefer KJ C++ types over STL types unless required by the cxx ABI.
- Preserve cancellation when converting between KJ promises and Rust futures.
- Every unsafe Rust block needs a `// Safety:` explanation.
- Run formatting and the full component tests after changing generated ABI behavior.
