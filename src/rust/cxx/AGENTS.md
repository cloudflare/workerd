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
- `kj-rs-io/` — tokio-backed `kj::AsyncIoStream` / `kj::Network` / `kj::LowLevelAsyncIoProvider`
  (the I/O providers for the tokio loop, `kj_rs_io::setupTokioAsyncIo()`), the `--watch` file
  watcher (Rust over `notify`), signals, and the native-serve entry points kj-hyper consumes
  (`serve_kj_stream` / `take_kj_socket`). C++ there is interface adaptation only; the one policy
  object that stays C++ is `PeerFilter`, a wrapper over KJ's own `kj::_::NetworkFilter`, which
  Rust consults through a bridged `should_allow`
- `tests/` and `kj-rs/tests/` — Rust and C++ bridge integration tests
- `tools/bazel/` — Bazel bridge-generation macro used by this component's tests

## Conventions

- Follow the parent `src/rust/AGENTS.md` and repository `AGENTS.md`.
- Prefer KJ C++ types over STL types unless required by the cxx ABI.
- Preserve cancellation when converting between KJ promises and Rust futures.
- Every unsafe Rust block needs a `// Safety:` explanation.
- Run formatting and the full component tests after changing generated ABI behavior.

## kj-rs-io ownership and reactor rules

- **Raw handles become owners at the bridge.** `ffi.rs` is the crate's only module allowed to
  write `unsafe`. An fd/SOCKET arrives from C++ as an integer only because cxx has no owning
  handle type; the bridge entry point converts it into `socket2::Socket` / `OwnedFd` in one
  `unsafe` block that names the C++ contract (`prepareFd` in async-io.c++: the handle is open,
  owned, non-blocking). The conversion helpers are `unsafe fn`s -- never make them safe, and never
  pass integers past `ffi.rs`. The same goes for read buffers: KJ callers may pass uninitialized
  storage, so a `tryRead` buffer crosses as pointer + length and becomes `&mut [MaybeUninit<u8>]`,
  never `&mut [u8]`.
- **Create tokio resources on the loop thread, poll them anywhere the driver allows.** Sockets,
  listeners, `AsyncFd`s and signal streams register with the runtime current at creation
  (`runtime::require_loop_runtime`, checked at creation only). A resource moved to another thread
  (a `ServeIo` native socket) keeps that registration and only progresses while the originating
  KJ loop turns tokio's driver; it is independent of the *KJ stream* it came from, not of the
  loop. Say so in docs rather than calling anything "KJ-independent".
- **KJ contracts are the boundary, not the type system.** Buffer validity until a promise settles,
  fd ownership under `TAKE_OWNERSHIP`, "no I/O in flight when handing over a stream" for foreign
  streams: these stay KJ's documented interface contracts. Where the crate can check one (a
  kj-rs-io stream's in-flight operations, via the `RefCell` guard in stream.rs) it does; where it
  cannot, name the contract at the `// Safety:` comment instead of claiming the compiler proves it.
- **`--config=asan` instruments C++ only.** Rust is compiled without `-Zsanitizer=address` under
  that config (only the `tsan` configs instrument Rust), so a passing `--config=asan` run says
  nothing about Rust memory safety; add `--@rules_rust//:extra_rustc_flag=-Zsanitizer=address`
  to instrument it.
- **Mirror KJ's policy, do not re-implement it.** Peer filtering is KJ's own
  `kj::_::NetworkFilter` behind a refcounted chain (`PeerFilter`), consulted from the Rust
  connect/accept/parse loops; fd-flag handling, accept-retry sets, socket options, backlog, and
  the operation-start policy (every returned I/O promise is started inside the call, like KJ's
  native streams) are cited to the kj source they mirror.

