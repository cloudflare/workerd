# src/rust/

## OVERVIEW

11 Rust library crates + 1 binary crate linked into workerd via CXX FFI. No Cargo workspace — entirely Bazel-driven (`wd_rust_crate.bzl`). Clippy pedantic+nursery enabled; `allow-unwrap-in-tests`.

## CRATES

| Crate                | Purpose                                                                                                |
| -------------------- | ------------------------------------------------------------------------------------------------------ |
| `jsg/`               | Rust JSG bindings: `Lock`, `Rc<T>`, `Resource`, `Struct`, `Type`, `Realm`, `FeatureFlags`, module registration |
| `jsg-macros/`        | Proc macros: `#[jsg_struct]`, `#[jsg_method]`, `#[jsg_resource]`, `#[jsg_oneof]`, `#[jsg_static_constant]` |
| `jsg-test/`          | Test harness (`Harness`) for JSG Rust bindings                                                         |
| `api/`               | Rust-implemented Node.js APIs; registers modules via `register_nodejs_modules()`                       |
| `dns/`               | DNS record parsing (CAA, NAPTR) via CXX bridge; legacy duplicate of `api/dns.rs`, pending removal      |
| `net/`               | Single function: `canonicalize_ip()`                                                                   |
| `kj/`                | Rust bindings for KJ library (`http`, `io`, `own` submodules); `Result<T>` = `Result<T, cxx::KjError>` |
| `cxx-integration/`   | Tokio runtime init; called from C++ `main()` before anything else                                      |
| `transpiler/`        | TS type stripping via SWC (`ts_strip()`, `StripOnly` mode)                                             |
| `python-parser/`     | Python import extraction via `ruff_python_parser`; **namespace: `edgeworker::rust::`**                 |
| `gen-compile-cache/` | Binary crate — V8 bytecode cache generator; calls C++ `compile()` via CXX                              |

## CONVENTIONS

- **CXX bridge**: `#[cxx::bridge(namespace = "workerd::rust::<crate>")]` with companion `ffi.c++`/`ffi.h` files
- **Namespace**: always `workerd::rust::*` except `python-parser` → `edgeworker::rust::python_parser`
- **Errors**: `thiserror` for library crates; `jsg::Error` with `ExceptionType` for JSG-facing crates
- **JSG resources**: `#[jsg_resource]` on struct + impl block; `#[jsg_method]` auto-converts `snake_case` → `camelCase`; methods with `&self`/`&mut self` become instance methods, methods without a receiver become static methods; `#[jsg_static_constant]` on `const` items exposes read-only numeric constants on both constructor and prototype (name kept as-is, no camelCase); resources integrate with GC via the `GarbageCollected` trait (auto-derived for `Rc<T>`, `WeakRc<T>`, `Option<Rc<T>>`, and `Nullable<Rc<T>>` fields)
- **Formatting**: `rustfmt.toml` — `group_imports = "StdExternalCrate"`, `imports_granularity = "Item"` (one `use` per import)
- **Linting**: `just clippy <crate>` — pedantic+nursery; `allow-unwrap-in-tests`
- **Tests**: inline `#[cfg(test)]` modules; JSG tests use `jsg_test::Harness::run_in_context()`
- **FFI pointers**: functions receiving raw pointers must be `unsafe fn` (see `jsg/README.md`)
- **Parameter ordering**: `&Lock` / `&mut Lock` must always be the first parameter in any function that takes a lock (matching the C++ convention where `jsg::Lock&` is always first). This applies to free functions, trait methods, and associated functions (excluding `&self`/`&mut self` receivers which come before `lock`).
- **Feature flags**: `Lock::feature_flags()` returns a capnp `compatibility_flags::Reader` for the current worker. Use `lock.feature_flags().get_node_js_compat()`. Flags are parsed once and stored in the `Realm` at construction; C++ passes canonical capnp bytes to `realm_create()`. Schema: `src/workerd/io/compatibility-date.capnp`, generated Rust bindings: `compatibility_date_capnp` crate.
