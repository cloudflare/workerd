# src/rust/

## OVERVIEW

A dozen or so Rust crates — mostly libraries, plus the `gen-compile-cache` binary — linked into workerd via CXX FFI. No Cargo workspace — entirely Bazel-driven (`wd_rust_crate.bzl` / `wd_rust_binary.bzl`). Clippy pedantic+nursery enabled; `allow-unwrap-in-tests`.

> **The "CXX FFI" above is [workerd-cxx](https://github.com/cloudflare/workerd-cxx), Cloudflare's heavily modified fork of [cxx-rs](https://cxx.rs)** — not stock cxx-rs. It adds deep KJ interoperability upstream lacks: `async` fns become `kj::Promise<T>`, you can return/hold `kj::Own<T>`, `Result<T>` throws `kj::Exception`, and other KJ types cross the boundary (see CXX BRIDGE below). cxx-rs is well represented in LLM training data, so it is easy to "recall" an API that is wrong here — prefer the prior art in these crates and the workerd-cxx sources (especially its `kj-rs` crate) over upstream cxx-rs docs or memory.

## CRATES

_Snapshot — the set drifts as crates come and go; `bazel query //src/rust/...` is authoritative._

| Crate                   | Purpose                                                                                                                                                                                                                       |
| ----------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `jsg/`                  | Rust JSG bindings: `Lock`, `Rc<T>`, `Resource`, `Struct`, `Type`, `Realm`, `FeatureFlags`, module registration; V8 handle types including typed arrays, `ArrayBuffer`, `ArrayBufferView`, `SharedArrayBuffer`, `BackingStore` |
| `jsg-macros/`           | Proc macros: `#[jsg_struct]`, `#[jsg_method]`, `#[jsg_resource]`, `#[jsg_oneof]`, `#[jsg_static_constant]`, `#[jsg_constructor]`                                                                                              |
| `jsg-test/`             | Test harness (`Harness`) for JSG Rust bindings                                                                                                                                                                                |
| `api/`                  | Rust-implemented Node.js APIs; registers modules via `register_nodejs_modules()`                                                                                                                                              |
| `net/`                  | Single function: `canonicalize_ip()`                                                                                                                                                                                          |
| `encoding/`             | WHATWG legacy text decoders via `encoding_rs`; an opaque streaming `Decoder` emits UTF-16 over the CXX bridge                                                                                                                 |
| `kj/`                   | Rust bindings for KJ library (`http`, `io`, `own` submodules); `Result<T>` = `Result<T, cxx::KjError>`                                                                                                                        |
| `worker/`               | Rust counterpart of `workerd::WorkerInterface` (the `worker::Interface` trait) plus FFI bindings; multi-bridge crate                                                                                                          |
| `cxx-integration/`      | Tokio runtime init; called from C++ `main()` before anything else                                                                                                                                                             |
| `cxx-integration-test/` | Non-production crate exercising Rust/C++ integration: callbacks, shared structs, `Result` error mapping                                                                                                                       |
| `transpiler/`           | TS type stripping via SWC (`ts_strip()`, `StripOnly` mode)                                                                                                                                                                    |
| `python-parser/`        | Python import extraction via `ruff_python_parser`; **namespace: `edgeworker::rust::`**                                                                                                                                        |
| `gen-compile-cache/`    | Binary crate — V8 bytecode cache generator; calls C++ `compile()` via CXX                                                                                                                                                     |

## CONVENTIONS

- **CXX bridge**: `#[cxx::bridge(namespace = "workerd::rust::<crate>")]` with companion `ffi.c++`/`ffi.h` files
- **Namespace**: always `workerd::rust::*` except `python-parser` → `edgeworker::rust::python_parser`
- **Errors**: `thiserror` for library crates; `jsg::Error` with `ExceptionType` for JSG-facing crates
- **JSG resources**: `#[jsg_resource]` on struct + impl block; `#[jsg_method]` auto-converts `snake_case` → `camelCase`; methods with `&self`/`&mut self` become instance methods, methods without a receiver become static methods; `#[jsg_static_constant]` on `const` items exposes read-only numeric constants on both constructor and prototype (name kept as-is, no camelCase); resources integrate with GC via `Traced` + `GarbageCollected`: every named field is traced via `Traced::trace(&self.field, visitor)` and all non-traceable types use no-op `Traced` impls
- **JSG properties**: two property macros on `#[jsg_resource]` impl blocks — `#[jsg_property(prototype|instance [, name = "..."] [, readonly])]` (registers an accessor; `prototype` maps to `JSG_PROTOTYPE_PROPERTY`, `instance` maps to `JSG_INSTANCE_PROPERTY`; `readonly` is a compile-time check preventing a paired setter; `name = "..."` overrides the JS name; prefer `prototype` in almost all cases), and `#[jsg_inspect_property]` (registered under a unique symbol, invisible to normal enumeration and string-key lookup, surfaced by `node:util` `inspect()`, equivalent to `JSG_INSPECT_PROPERTY`); setter auto-detected from `set_` prefix; read-only when no setter present; getter/setter `.length` and `.name` are set correctly when `spec_compliant_property_attributes` compat flag is enabled
- **`Traced`**: core tracing trait in `jsg::wrappable`; built-ins include no-op impls for primitives/value types and delegating impls for wrappers/collections (`Option`, `Nullable`, `Vec`, maps/sets, `Cell`, `jsg::Rc`, `jsg::Weak`, `jsg::v8::Global`)
- **`#[jsg_resource(custom_trace)]`**: suppresses the auto-generated `Traced` impl so the user can write their own; `GarbageCollected` (`memory_name`), `jsg::Type`, `jsg::ToJS`, and `jsg::FromJS` are still generated
- **Formatting**: `rustfmt.toml` — `group_imports = "StdExternalCrate"`, `imports_granularity = "Item"` (one `use` per import)
- **Linting**: `just clippy <crate>` — pedantic+nursery; `allow-unwrap-in-tests`
- **Tests**: inline `#[cfg(test)]` modules; JSG tests use `jsg_test::Harness::run_in_context()`. Always run the full `src/rust/...` test suite (`bazel test //src/rust/...`) rather than targeting a single crate — changes in shared crates like `jsg` or `jsg-macros` can break downstream consumers
- **FFI pointers**: functions receiving raw pointers must be `unsafe fn` (see `jsg/README.md`)
- **Parameter ordering**: `&Lock` / `&mut Lock` must always be the first parameter in any function that takes a lock (matching the C++ convention where `jsg::Lock&` is always first). This applies to free functions, trait methods, and associated functions (excluding `&self`/`&mut self` receivers which come before `lock`).
- **Method naming**: do not use `get_` prefixes on methods — e.g. `buf.backing_store()` not `buf.get_backing_store()`. Static constructors belong on the marker struct (`impl ArrayBuffer { fn new(...) }`) not on `impl Local<'_, ArrayBuffer>`.
- **FFI naming**: instance methods on an existing handle use a `local_<type>_<method>` prefix (e.g. `local_array_buffer_byte_length`). Static constructors that create a new value do **not** use the `local_` prefix — name them `<type>_<method>` (e.g. `array_buffer_new_with_mode`, `array_buffer_maybe_new`, `backing_store_new_resizable`).
- **FFI groups**: `v8.rs` `mod ffi`, `ffi.h`, and `ffi.c++` all use matching comment groups (e.g. `// Local<T>`, `// Local<Array>`, `// Local<TypedArray>`, `// Local<ArrayBuffer>`, `// Local<ArrayBufferView>`, `// Local<SharedArrayBuffer>`, `// BackingStore`, `// Unwrappers`, `// Global<T>`, `// FunctionCallbackInfo`). When adding new FFI functions, place them in the correct group in **all three files**. Do not scatter related functions across groups.
- **Feature flags**: `Lock::feature_flags()` returns a capnp `compatibility_flags::Reader` for the current worker. Use `lock.feature_flags().get_node_js_compat()`. Flags are parsed once and stored in the `Realm` at construction; C++ passes canonical capnp bytes to `realm_create()`. Schema: `src/workerd/io/compatibility-date.capnp`, generated Rust bindings: `compatibility_date_capnp` crate.

## CXX BRIDGE: ASYNC AND ERROR HANDLING

### C++ calling async Rust (`extern "Rust"`)

Mark an `extern "Rust"` function `async` to generate a C++ function returning `kj::Promise<T>`. If the function borrows any references (including `&self`), it needs an explicit lifetime annotation, which in turn requires `unsafe`:

```rust
extern "Rust" {
    // Borrows &self — needs explicit lifetime + unsafe.
    async unsafe fn do_work<'a>(self: &'a MyType, arg: i32) -> Result<u64>;

    // Only owned parameters — no lifetime or unsafe needed.
    async fn do_work_owned(arg: i32) -> Result<u64>;
}
```

Without the explicit lifetime, the CXX macro requires the future to be `'static`, which fails if the async body references borrowed parameters.

### Rust calling async C++ (`extern "C++"`)

Mark an `extern "C++"` function `async` to wrap a C++ function returning `kj::Promise<T>` as a Rust `Future`. The Rust caller can `.await` it:

```rust
unsafe extern "C++" {
    async fn request(
        this_: Pin<&mut HttpService>,
        method: HttpMethod,
        url: &[u8],
    ) -> Result<()>;
}
```

Note: `this_` is used instead of `self` because CXX's `self` receiver syntax doesn't support `Pin<&mut T>`. This is needed for C++ types with virtual methods where the Rust side calls into a pinned C++ object.

### Error handling across FFI

Functions returning `Result<T>` in `extern "Rust"` blocks translate to C++ functions that throw `kj::Exception` on error. The error type must implement `std::error::Error` (which provides `IntoKjException`). Recommended patterns:

- **`thiserror` enums**: Define a crate-level `Error` enum for structured errors. This is the preferred pattern for crates with multiple error cases.
- **`std::io::Error`**: Acceptable for purely I/O-related errors.
- **`cxx::KjError`**: Use `KjError::new(KjExceptionType::Failed, message)` when you need direct control over the KJ exception type.

## CXX BRIDGE: BUILD WIRING

`wd_rust_crate` (and `wd_rust_binary`) generate, for each `cxx_bridge_src` / `cxx_bridge_srcs` entry, a companion `:<bridge>@cxx` cc_library — the C++ side of the bridge: the cxx-generated `<bridge>.rs.{h,cc}` plus every `**/*.h` in the package (globbed into its `hdrs`). C++ consumers `#include <workerd/rust/<pkg>/<bridge>.rs.h>` and depend on the crate (`//src/rust/<pkg>`); depend on `:<bridge>@cxx` alone if you only need the header. The header include prefix is `workerd/` + the package path with `src/` stripped. Any crate with a bridge auto-gets `@workerd-cxx//:cxx` and `@workerd-cxx//kj-rs`.

### Rust → C++ (calling a C++ function from Rust)

Template: **`gen-compile-cache`**, whose `main.rs` bridge calls C++ `compile()`. The shim header is conventionally named `cxx-bridge.h` (or `bridge.h`):

```rust
unsafe extern "C++" {
    include!("workerd/rust/gen-compile-cache/cxx-bridge.h");
    fn compile(path: &str, source_code: &str) -> Vec<u8>;
}
```

1. Put the shim header **in the package** — the macro globs `**/*.h` into the generated `@cxx` library's `hdrs`, so the `include!` resolves. Keep it light (ideally just `#include <rust/cxx.h>` for `rust::Str` / `rust::Vec`).
2. Implement the function in a `.c++` exposed as a **separate `wd_cc_library`** (e.g. `gen-compile-cache`'s `:cxx-bridge`); the heavy C++ deps (JSG, etc.) live there, not in the shim header.
3. Add that `wd_cc_library` to the crate's `deps` so the symbol resolves at the final link.

### Avoiding the C++ → Rust → C++ dependency cycle

If a C++ library already depends on your crate (C++ → Rust) and you also add a Rust → C++ shim, the shim **must be a separate `cc_library`** from the one that depends on the crate, or Bazel reports a cycle. To pass a cxx shared struct to the shim without a cycle (see the `worker` crate's `:bridge`):

- The shim's `.c++` includes `<workerd/rust/<pkg>/<bridge>.rs.h>` and the shim `cc_library` depends on the generated `:<bridge>@cxx` for that header. This is acyclic: `:<bridge>@cxx` depends only on cxx/kj-rs, never on the shim, and the shim's symbol is resolved at the final link.
- **Forward-declare the struct in the shim _header_** — do not `#include` the generated `<bridge>.rs.h` there, because that generated header `#include`s your shim header, so including it back is circular. A forward declaration is enough for a `const T&` parameter; the full definition is only needed in the `.c++`.

cxx also supports **reusing a binding type across bridges** ([docs](https://cxx.rs/extern-c++.html#reusing-existing-binding-types)): the `worker` crate's `error.rs` / `ok.rs` / `kill_switch.rs` bridges reuse `ffi.rs`'s types by depending on `:ffi.rs@cxx`. Still, keep a struct that only crosses FFI within one crate in that crate's bridge.

### Testing crates that cross the FFI

A `rust_test` exercising code that calls into heavy C++ (V8, etc.) must link those impl symbols itself via `test_deps` — the production binary already links them, so only the standalone test binary needs them. Symptom if missing: undefined C++ symbols at test link.
