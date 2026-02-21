# src/rust/

## OVERVIEW

11 Rust library crates + 1 binary crate linked into workerd via CXX FFI. No Cargo workspace — entirely Bazel-driven (`wd_rust_crate.bzl`). Clippy pedantic+nursery enabled; `allow-unwrap-in-tests`.

## CRATES

| Crate                | Purpose                                                                                                |
| -------------------- | ------------------------------------------------------------------------------------------------------ |
| `jsg/`               | Rust JSG bindings: `Lock`, `Ref<T>`, `Resource`, `Struct`, `Type`, `Realm`, module registration        |
| `jsg-macros/`        | Proc macros: `#[jsg_struct]`, `#[jsg_method]`, `#[jsg_resource]`, `#[jsg_oneof]`                       |
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
- **JSG resources**: must include `_state: jsg::ResourceState` field; `#[jsg_method]` auto-converts `snake_case` → `camelCase`
- **Formatting**: `rustfmt.toml` — `group_imports = "StdExternalCrate"`, `imports_granularity = "Item"` (one `use` per import)
- **Linting**: `just clippy <crate>` — pedantic+nursery; `allow-unwrap-in-tests`
- **Tests**: inline `#[cfg(test)]` modules; JSG tests use `jsg_test::Harness::run_in_context()`
- **FFI pointers**: functions receiving raw pointers must be `unsafe fn` (see `jsg/README.md`)
