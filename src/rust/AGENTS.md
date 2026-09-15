# src/rust/

## OVERVIEW

A dozen or so Rust crates — mostly libraries, plus the `gen-compile-cache` binary — linked into workerd via CXX FFI. No Cargo workspace — entirely Bazel-driven (`wd_rust_crate.bzl` / `wd_rust_binary.bzl`). Clippy pedantic+nursery enabled; `allow-unwrap-in-tests`.

> **The "CXX FFI" above is the in-tree `src/rust/cxx` fork of [cxx-rs](https://cxx.rs/)** — not stock cxx-rs. It adds deep KJ interoperability upstream lacks: `async` fns become `kj::Promise<T>`, you can return/hold `kj::Own<T>`, `Result<T>` throws `kj::Exception`, and other KJ types cross the boundary (see CXX BRIDGE below). cxx-rs is well represented in LLM training data, so it is easy to "recall" an API that is wrong here — prefer the prior art in these crates and the in-tree CXX sources (especially its `kj-rs` crate) over upstream cxx-rs docs or memory.

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

## OWNERSHIP AND LIFETIMES ACROSS THE FFI

These are the load-bearing rules for any code that stores state across the Rust/C++ boundary or
holds references past a single synchronous call. Violations here are how use-after-free hides in a
codebase where each side trusts the other's lifetime comments.

- **No borrowed reference or raw pointer may be *stored* beyond the synchronous call that received
  it.** Passing `&T`/`const T&` down a call is fine; squirreling it away in a member for a later
  callback is not — no matter how good the "the referent outlives us because…" comment is. Every
  stored edge must be one of:
  - **Owning**: `kj::Own`, `kj::Arc`, `rust::Box`, `Box`, or an owned raw handle with single-owner
    drop glue (the `OwnPromiseNode` pattern: raw pointer + `!Send` + `Drop` that destroys it).
  - **Checked-weak**: `kj::Weak<T>` with a `kj::PtrTarget` base (expires automatically; but see
    the threading rule below), or an owner-invalidated cell where the owner's destructor
    structurally nulls the link (RAII guard, not a destructor body remembering to).
  - **Assertion-enforced intrusive**: `kj::List`/`kj::ListLink` (a leaf destroyed while linked
    trips `ListLink`'s destructor assert).
- **Ownership must be expressed in the type system, not in comments.** If a design needs a "caller
  must call exactly one of X/Y" or "must outlive Z" convention, reach for a type that enforces it
  before writing the comment. On this bridge that is almost always available:
  - C++ owning a Rust object: opaque `extern "Rust"` type held as `rust::Box<T>` — move-only,
    automatic drop glue, methods via `fn f(self: &T)` in the bridge. This is how C++ should hold
    a `std::task::Waker` clone, a channel handle, etc.
  - Rust owning a C++ object: `kj::Own`/`kj::Arc` via `ExternType` (`KjOwn`/`KjArc`), as kj-rs
    does for `FutureWakerCell`.
- **Never smuggle a pointer through an integer or a word-pair.** The existing V8 rule ("handles
  never cross as a bare `usize`") is a special case of the general one: if you find yourself
  decomposing an object into raw words so the other language can hold it, use an opaque bridged
  type instead. Raw-parts designs push ownership back into comments (see previous rule) and defeat
  both languages' tooling.
- **Know each primitive's thread contract before storing it in a cross-thread object.**
  `kj::Weak`/`kj::Ptr`/`kj::Pin` are single-threaded (non-atomic control block); KJ promises and
  `kj::Own` are single-loop; only `kj::CrossThreadPromiseFulfiller`, `kj::Executor`,
  `kj::AtomicRefcounted`/`kj::Arc` cross threads. Anything reachable from a `std::task::Waker`
  must be `Send + Sync` — `Waker` is unconditionally both, and type erasure means the compiler
  will not catch a single-threaded type hiding behind one (kj-rs's `FutureWakerCell` exists
  precisely because `kj::Weak` cannot live in a cell that foreign threads may destroy).
- **Prefer methods on bridged types over free functions**, in both directions — `fn f(self: &T)`
  in the bridge, not `fn f(t: &T)` — so the API surfaces as `handle.f()` on the C++ side.
- **Opaque-size handshakes**: when Rust owns storage for a C++ object (bindgen-style
  `_bindgen_opaque_blob`), the C++ side must `static_assert` size/alignment against the generated
  repr (see awaiter.c++). When the C++ type changes, rebuild and let the assert tell you the new
  size rather than computing it by hand.

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
- **`cxx::KjError`**: Use the `kj` crate's error macros (`kj::failed!`, `kj::overloaded!`, `kj::disconnected!`, `kj::not_implemented!` to construct a value; `kj::fail_require!`, `kj::overloaded_require!`, `kj::disconnected_require!`, `kj::not_implemented_require!` to early-return `Err(...)`) when you need direct control over the KJ exception type. These mirror C++'s `KJ_EXCEPTION`/`KJ_FAIL_REQUIRE`/`KJ_UNIMPLEMENTED` macros from `kj/debug.h`; see `src/rust/kj/macros.rs`.

The reverse direction has a stricter, non-optional rule: any `extern "C++"` shim that can throw (`jsg::check()`, `KJ_REQUIRE`/`KJ_ASSERT`, constructing a `rust::String`/`rust::Vec` that can panic, etc.) **MUST** return `Result<T>`. Unlike Rust panics unwinding into C++ (which `cxx` catches at the bridge), a C++ exception unwinding across the Rust `nounwind` FFI frame aborts the process — there is no equivalent automatic catch on that side. See `//src/rust/jsg/v8.rs` "Unwrappers" section for worked examples: `unwrap_string`/`unwrap_number`/the typed-array unwrap shims are fallible (they call `jsg::check()` / construct `rust::String`), while `unwrap_boolean` is the deliberate infallible counterexample (no `jsg::check()`, no fallible construction). Fallible shims typically funnel their C++-side `TryCatch`/`KJ_REQUIRE` handling into a `cxx::KjException`, which `impl From<cxx::KjException> for jsg::Error` (`//src/rust/jsg/lib.rs`) then converts back into a throwable JS error on the Rust side.

## CXX BRIDGE: BUILD WIRING

`wd_rust_crate` (and `wd_rust_binary`) generate, for each `cxx_bridge_src` / `cxx_bridge_srcs` entry, a companion `:<bridge>@cxx` cc_library — the C++ side of the bridge: the cxx-generated `<bridge>.rs.{h,cc}` plus every `**/*.h` in the package (globbed into its `hdrs`). C++ consumers `#include <workerd/rust/<pkg>/<bridge>.rs.h>` and depend on the crate (`//src/rust/<pkg>`); depend on `:<bridge>@cxx` alone if you only need the header. The header include prefix is `workerd/` + the package path with `src/` stripped. Any crate with a bridge auto-gets `//src/rust/cxx:cxx` and `//src/rust/cxx/kj-rs`.

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
3. Add that `wd_cc_library` to the crate's `link_deps` so the symbol resolves at the final link.

### Avoiding the C++ → Rust → C++ dependency cycle

If a C++ library already depends on your crate (C++ → Rust) and you also add a Rust → C++ shim, the shim **must be a separate `cc_library`** from the one that depends on the crate, or Bazel reports a cycle. To pass a cxx shared struct to the shim without a cycle (see the `worker` crate's `:bridge`):

- The shim's `.c++` includes `<workerd/rust/<pkg>/<bridge>.rs.h>` and the shim `cc_library` depends on the generated `:<bridge>@cxx` for that header. This is acyclic: `:<bridge>@cxx` depends only on cxx/kj-rs, never on the shim, and the shim's symbol is resolved at the final link.
- **Forward-declare the struct in the shim _header_** — do not `#include` the generated `<bridge>.rs.h` there, because that generated header `#include`s your shim header, so including it back is circular. A forward declaration is enough for a `const T&` parameter; the full definition is only needed in the `.c++`.

cxx also supports **reusing a binding type across bridges** ([docs](https://cxx.rs/extern-c++.html#reusing-existing-binding-types)): the `worker` crate's `error.rs` / `ok.rs` / `kill_switch.rs` bridges reuse `ffi.rs`'s types by depending on `:ffi.rs@cxx`. Still, keep a struct that only crosses FFI within one crate in that crate's bridge.

**V8 handles must always cross the FFI as the shared `jsg::v8::ffi` types, never as a bare `usize`.** When another crate's bridge passes a V8 `Local`/`Global`, reuse the jsg shared struct via a type alias (`type Local = jsg::v8::ffi::Local;`) plus `include!("workerd/rust/jsg/v8.rs.h")`, and depend on `//src/rust/jsg` (which supplies the generated header transitively). Do not smuggle the handle word through a `usize` — the shared type keeps both sides in one canonical, cxx-verified definition. See `node-exceptions/lib.rs`.

### Testing crates that cross the FFI

A `rust_test` exercising code that calls into heavy C++ (V8, etc.) must link those impl symbols itself via `test_deps` — the production binary already links them, so only the standalone test binary needs them. Symptom if missing: undefined C++ symbols at test link.
