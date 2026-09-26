# Differences from upstream cxx

The in-tree fork contains these major behavioral changes:

- **Exception handling instead of aborts.** Every FFI function reports failure through its return
  value in both directions. An `extern "Rust"` function that panics reports a `kj::Exception` to
  C++, while an `extern "C++"` function that throws reports the exception to Rust. Infallible Rust
  signatures panic when C++ throws. Return values travel through out parameters so no bridge shim
  is treated as truly `noexcept`.
- **Required lifetime annotations on returned references.** This supports the exception-safe bridge
  ABI.
- **KJ interoperability.** Bridge generation and `kj-rs` support KJ ownership, refcounting,
  `kj::Maybe`, `kj::Date`, exceptions, promises, and Rust futures.
- **Async bridge functions.** `async fn` in an `extern "Rust"` block becomes a C++ function
  returning `kj::Promise<T>`; in an `extern "C++"` block it wraps a promise-returning C++ function
  as a Rust future. Borrowed arguments elide their lifetimes and the functions are safe: the
  future is bound to the arguments (a synthesized `'__cxx` lifetime on the Rust side, edition
  2024 `impl Future` capture on the C++ side). The upstream rule that a function exposing
  lifetimes to C++ must be `unsafe` does not apply to async functions.
- **`__WORKERD_CXX__`.** This preprocessor definition identifies the fork even though its crate and
  generated symbol names remain compatible with cxx.
- **Workerd build integration.** Bazel targets use workerd's in-tree Rust toolchain, crate vendor
  repository, Cap'n Proto/KJ dependency, formatting, and lint configuration.
