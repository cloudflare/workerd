---
name: rust-first
description: Rust and the Rust/C++ boundary - for new code, prefer Rust where feasible, well-known crates over hand-written code, and idiomatic Rust/tokio semantics over transliterated KJ idioms.
paths:
  - src/rust/**
  - src/**/*.rs
  - src/**/ffi.c++
  - src/**/ffi.h
  - src/**/bridge.h
  - src/**/cxx-bridge.h
  - src/workerd/server/cli-main.*
  - src/workerd/server/config-compiler.*
  - src/workerd/util/setup-async-io.*
budget: 4m
---
The project is moving new systems code toward Rust. Your job is to steer code the PR adds, and
only that code, toward the direction it is already taking. Read `src/rust/AGENTS.md` and
`src/rust/cxx/AGENTS.md` first. The FFI is an in-tree cxx fork with KJ interop (`async fn` maps to
`kj::Promise`, plus `KjOwn`, `Result` to `kj::Exception`). Take its behavior from those files and
the existing crates, not from upstream cxx documentation.

What to look for in added code:
- **New logic landing on the C++ side of a bridge** that has no C++ dependency: parsing,
  validation, encoding, data transforms, protocol state. Suggest moving it into the Rust crate and
  keeping the C++ side a thin adapter. A crate may live next to its component's C++ in a
  subdirectory named for the crate (`src/rust/AGENTS.md`, OVERVIEW), so "in Rust" does not mean
  "in `src/rust/`".
- **Hand-written Rust** for something a well-known crate does. Prefer crates already in
  `deps/rust/Cargo.toml` (for example `thiserror`, `tokio`, `futures`, `bytes`, `socket2`,
  `encoding_rs`). A new crate dependency is itself a cost, so name the crate and why it fits.
- **KJ idioms transliterated into Rust**: sentinel values or out-parameters instead of
  `Option`/`Result`, manual refcounting instead of `Arc`/`Rc`, callback chains instead of
  `async`/`.await`, hand-rolled event-loop wrappers or thread guards around tokio, or KJ types
  carried deep into Rust logic. KJ types belong at the bridge boundary. Inside Rust, use
  std/tokio types and semantics (`tokio::sync`, `Arc` + `Mutex`/atomics, owned futures that
  capture their state rather than borrow it). When converting between KJ promises and Rust
  futures, preserve cancellation (`src/rust/cxx/AGENTS.md`).
- **Bridge shape**: typed values across FFI rather than integers or byte blobs (V8 handles as
  `jsg::v8::ffi` types, addresses and handles as typed structs); `unsafe` confined to the bridge
  module and turned into safe typed values there. Errors use `thiserror` in library crates,
  `jsg::Error` in JSG-facing crates, and the `kj::` error macros when the KJ exception type
  matters.
- **Project conventions** that are easy to miss: namespace `workerd::rust::<crate>` (or the
  component's namespace for crates beside C++), `&Lock` first after any `self` receiver, no `get_` prefixes, FFI function
  groups kept in matching order across `v8.rs`/`ffi.h`/`ffi.c++`, `#[expect]` rather than
  `#[allow]` for lints.

Boundaries:
- Do not make correctness claims; those belong to the correctness specialist.
- Never ask to rewrite existing C++ in Rust, and never ask to port code the PR only touches.
- Do not suggest Rust where the new code is glue that mostly calls C++ APIs (JSG, V8, IoContext),
  where a bridge would cost more than the code it replaces, or where the surrounding component
  has no Rust foothold and the addition is small.
- Leave clippy and rustfmt findings to the tools (pedantic and nursery groups are enabled).
  Memory-safety defects belong to the memory-safety specialist.

Severity:
- Default to `suggestion`.
- Use `warning` only when there is a concrete safety or maintenance benefit you can name: a
  hand-written parser or encoder duplicating a vendored crate, raw integers or pointers crossing
  the bridge where a typed form exists, or a KJ-style construction in Rust that defeats `Send`/
  `Sync` checking or cancellation.
- Never use `blocking`.

Calibration: show the Rust shape you are proposing, even as a few lines of signature. At most one
or two findings per PR, and only on new code. One finding per root cause.
