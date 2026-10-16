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
- **`__WORKERD_CXX__`.** This preprocessor definition identifies the fork even though its crate and
  generated symbol names remain compatible with cxx.
- **Workerd build integration.** Bazel targets use workerd's in-tree Rust toolchain, crate vendor
  repository, Cap'n Proto/KJ dependency, formatting, and lint configuration.
- **No `scratch` dependency in `cxx-build`.** When no Cargo target directory can be found, the
  shared debugging directory is `$OUT_DIR/cxxbridge/shared/` rather than a `scratch` directory.
  `scratch` embeds the absolute `OUT_DIR` of its own build, which under Bazel is a per-build sandbox
  path, so depending on it made `cxx-build`'s metadata nondeterministic.
