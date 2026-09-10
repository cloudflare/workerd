# In-tree CXX fork

This directory contains Cloudflare's fork of [cxx](https://cxx.rs/), the C++/Rust
interop layer used by workerd. It was imported from the former
[`cloudflare/workerd-cxx`](https://github.com/cloudflare/workerd-cxx) repository and is now
built directly as part of workerd. The fork remains based on cxx, but adds the KJ integration
required by workerd.

The source is intentionally kept in tree so bridge changes can be developed, reviewed, and tested
atomically with their workerd consumers. Rust dependencies are resolved by
[`deps/rust/Cargo.toml`](../../../deps/rust/Cargo.toml), and the Bazel targets in this directory use
workerd's toolchains and external dependencies.

## Build targets

The main targets are:

- `//src/rust/cxx:cxx` — Rust runtime crate.
- `//src/rust/cxx:core` — C++ runtime and `<rust/cxx.h>`.
- `//src/rust/cxx:codegen` — bridge code generator used by `wd_rust_crate`.
- `//src/rust/cxx/kj-rs` — KJ smart-pointer, data-type, exception, and async integration.

Build or test the component from the workerd repository root:

```sh
bazel build //src/rust/cxx/...
bazel test //src/rust/cxx/...
```

Workerd bridge rules depend on these targets through in-tree labels; no external
`workerd-cxx` repository is involved.

## Fork-specific behavior

### `kj::Exception` and panic handling

Generated bridges use KJ's exception model:

- C++ exceptions cross into Rust as errors for `Result` signatures and as panics for infallible
  signatures.
- Rust `Result<T, E>` errors cross into C++ as `kj::Exception` values.
- `cxx::KjError` allows Rust code to control the KJ exception type and description.
- `kj::CanceledException` is preserved in both directions.
- Rust panics cross into C++ as `kj::Exception` rather than aborting the process.

### KJ types

The `kj-rs` bridge supports:

- `kj::Own<T>`, `kj::Rc<T>`, and `kj::Arc<T>`.
- `kj::Maybe<T>` and `kj::Date`.
- Conversions provided by [`kj-rs/convert.h`](kj-rs/convert.h).

### Async functions

Bridge declarations support async calls in both directions:

- A C++ `kj::Promise<T>` is exposed to Rust as a `Future<Output = T>`.
- A Rust future is exposed to C++ as a `kj::Promise<T>`.

These bridge futures and promises are driven by the KJ event loop. Rust-native futures that do not
cross the bridge can still use executors such as Tokio.

### Shared Rust type aliases

The fork includes support for reusing Rust type aliases across bridge modules, based on
[cxx pull request 1181](https://github.com/dtolnay/cxx/pull/1181).

See [`PATCHES.md`](PATCHES.md) for a concise inventory of the fork's major differences from
upstream cxx.
