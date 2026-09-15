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
- `tests/` and `kj-rs/tests/` — Rust and C++ bridge integration tests
- `tools/bazel/` — Bazel bridge-generation macro used by this component's tests

## Async bridge semantics

- Marking a fn `async` in `extern "Rust"` yields a `kj::Promise<T>` in C++; `async` in
  `extern "C++"` yields an `impl Future` in Rust.
- Bridged `kj::Promise<T>`s are **eager by default**: the Rust future is polled to its first
  suspension point at the call (KJ code assumes hot promises), so callers never need
  `.eagerlyEvaluate(nullptr)`. `RustFuture::lazily()` (kj-rs/future.h) is the C++-side
  escape hatch for the rare cold case.
- The waker bridge honors `Waker: Send + Sync` for real: a Rust `.await` of a KJ promise links
  to the `FuturePollEvent` via an intrusive weak link (`RustPromiseAwaiter::link` /
  `FuturePollEvent::leaves`), and a cloned waker is an atomically-refcounted `FutureWakerCell`
  whose wake checks the owning executor — on the owning loop's thread it arms the
  `FuturePollEvent` directly (the hot path), from any other thread it enqueues itself on the
  loop's single `CrossThreadWakeSink` (a `kj::EventLoopLocal`), whose drain replays the wake on
  the owning thread.

## Conventions

- Follow the parent `src/rust/AGENTS.md` and repository `AGENTS.md`.
- Prefer KJ C++ types over STL types unless required by the cxx ABI.
- Preserve cancellation when converting between KJ promises and Rust futures.
- Every unsafe Rust block needs a `// Safety:` explanation.
- Run formatting and the full component tests after changing generated ABI behavior.
