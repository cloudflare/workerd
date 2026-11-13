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
- `KjException` in a bridge signature passes an exception as a value, in either direction: a
  function can take one as an argument or return one, also as the value of a `Result` return
  (`-> Result<KjException>`), and C++ sees a `kj::Exception` by value. On the Rust side it is
  `cxx::KjException` (`cxx::KjException::from(kj_error)` builds one), which owns a heap
  `kj::Exception`; ownership moves with the call, and the receiving side takes over the
  exception before the callee runs. It keeps the type, description, file, line and every detail.
  It is rejected anywhere else: behind a reference or pointer, in a slice, array, `Vec`, `Box`,
  `KjMaybe` or other container, in a struct field or a function pointer, and as the output of an
  `async fn`.
- A Rust function or future whose error is a `cxx::KjException` (`Result<T, cxx::KjException>`
  in its Rust signature) fails in C++ with that exception as it is, stack trace included, rather
  than with a new `FAILED` exception holding only its description.
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
