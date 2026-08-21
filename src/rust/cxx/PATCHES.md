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
- **`c_char16`.** An additional builtin type, spelled `char16_t` in C++, for UTF-16 data. `u16`
  means `uint16_t`, which C++ mangles and overloads separately from `char16_t`, so a bridge that
  must name `char16_t` cannot use `u16`. The Rust side is an alias for `u16`, following `c_char`,
  so no cast is needed at call sites; see the doc comment on `cxx::c_char16` for the limits that
  follow from being an alias.
- **Unqualified emission of C++ fundamental types.** A `Pair` whose name is a fundamental type and
  whose namespace is empty is written bare rather than with a leading `::`, because those names are
  keywords: `::char16_t` is ill-formed where `::uint16_t` is fine. This lets an `ExternType` alias
  bind a fundamental type that has no builtin spelling in a bridge.
- **Workerd build integration.** Bazel targets use workerd's in-tree Rust toolchain, crate vendor
  repository, Cap'n Proto/KJ dependency, formatting, and lint configuration.
