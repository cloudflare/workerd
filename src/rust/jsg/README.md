# JSG (JavaScript Glue) Rust Bindings

Rust bindings for the JSG (JavaScript Glue) layer, enabling Rust code to integrate with workerd's JavaScript runtime.

## FFI Functions with Raw Pointers

Functions exposed to C++ via FFI that receive raw pointers must be marked as `unsafe fn`. The `unsafe` keyword indicates to callers that the function deals with raw pointers and requires careful handling.

```rust
pub unsafe fn realm_create(
    isolate: *mut v8::ffi::Isolate,
    feature_flags_data: &[u8],
) -> Box<Realm> {
    // implementation
}
```

For more information on unsafe Rust and raw pointers, see the [Rust Book: Unsafe Superpowers](https://doc.rust-lang.org/book/ch20-01-unsafe-rust.html#unsafe-superpowers).

## Union Types

To accept JavaScript values that can be one of several types, define an enum with the `#[jsg_oneof]` macro:

```rust
use jsg_macros::jsg_oneof;

#[jsg_oneof]
#[derive(Debug, Clone)]
enum StringOrNumber {
    String(String),
    Number(f64),
}

// In a jsg_method:
pub fn process(&self, value: StringOrNumber) -> Result<String, jsg::Error> {
    match value {
        StringOrNumber::String(s) => Ok(format!("string: {}", s)),
        StringOrNumber::Number(n) => Ok(format!("number: {}", n)),
    }
}
```

This is similar to `kj::OneOf<>` in C++ JSG.

## Feature Flags (Compatibility Flags)

`Lock::feature_flags()` provides Rust-native access to the worker's compatibility flags, backed by the Cap'n Proto Rust crate (`capnp`). The flags are deserialized from the `CompatibilityFlags` schema in `src/workerd/io/compatibility-date.capnp`.

### Reading flags

```rust
if lock.feature_flags().get_node_js_compat() {
    // Node.js compatibility behavior
}
```

`feature_flags()` returns a capnp-generated `compatibility_flags::Reader` with a getter for each boolean flag (e.g., `get_node_js_compat()`, `get_url_standard()`, `get_fetch_refuses_unknown_protocols()`).

### How it works

1. During worker initialization, C++ canonicalizes the worker's `CompatibilityFlags` via `capnp::canonicalize()` and passes the bytes to `realm_create()`, which parses them once and stores the result in the per-context `Realm`.
2. `lock.feature_flags()` reads the cached `FeatureFlags` and returns its capnp reader. No copies or re-parsing on access.

### Key types and files

| Item | Location |
|------|----------|
| `FeatureFlags` struct | `src/rust/jsg/feature_flags.rs` |
| `Lock::feature_flags()` | `src/rust/jsg/lib.rs` |
| `realm_create()` FFI | `src/rust/jsg/lib.rs` (CXX bridge) |
| C++ call site | `src/workerd/io/worker.c++` (`initIsolate`) |
| Cap'n Proto schema | `src/workerd/io/compatibility-date.capnp` |
| Generated Rust bindings | `//src/workerd/io:compatibility-date_capnp_rust` (Bazel target) |
