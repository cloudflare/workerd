# src/rust/

## OVERVIEW

A dozen or so Rust crates — mostly libraries, plus the `gen-compile-cache` binary — linked into workerd via CXX FFI. No Cargo workspace — entirely Bazel-driven (`wd_rust_crate.bzl` / `wd_rust_binary.bzl`). Clippy pedantic+nursery enabled; `allow-unwrap-in-tests`; `clippy.toml` and `rustfmt.toml` live at the repository root and apply to every crate.

Rust does not have to live here. A crate that belongs to a component lives beside that component's C++, in a subdirectory named for the crate: e.g. workerd's entry point (`workerd-cli`, `src/workerd/server/cli/`, declared in `src/workerd/server/BUILD.bazel` with its `srcs` globbed from that subdirectory and its `cxx_bridge_hdrs` listed explicitly) and its server (`workerd-server`, `src/workerd/server/server/`, a package of its own over the C++ worker factory in `src/workerd/server/factory/`; see `src/workerd/server/AGENTS.md`). Such crates use the component's C++ namespace for their bridge (`workerd::server`, `workerd::server::cli`), and their generated bridge header sits at the crate's C++ include path (`workerd/server/cli/bridge.rs.h`, `workerd/server/server/bridge.rs.h`). This directory holds the crates that have no C++ home of their own, and the in-tree cxx fork.

> **The "CXX FFI" above is the in-tree `src/rust/cxx` fork of [cxx-rs](https://cxx.rs/)** — not stock cxx-rs. It adds deep KJ interoperability upstream lacks: `async` fns become `kj::Promise<T>`, you can return/hold `kj::Own<T>`, `Result<T>` throws `kj::Exception`, and other KJ types cross the boundary (see CXX BRIDGE below). cxx-rs is well represented in LLM training data, so it is easy to "recall" an API that is wrong here — prefer the prior art in these crates and the in-tree CXX sources (especially its `kj-rs` crate) over upstream cxx-rs docs or memory.

## CRATES

_Snapshot — the set drifts as crates come and go; `bazel query //src/rust/...` is authoritative._

| Crate                   | Purpose                                                                                                                                                                                                                       |
| ----------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `jsg/`                  | Rust JSG bindings: `Lock`, `Rc<T>`, `Resource`, `Struct`, `Type`, `Realm`, `FeatureFlags`, module registration; V8 handle types in `jsg::v8` (`Local`, `Global`, `TracedGlobal` over marker types that mirror V8's classes, from `Value` and `Number` to the typed arrays, and `BackingStore`); JS functions from Rust closures (`v8::Function::new`); async context frames (`AsyncContextFrame`, `TracedAsyncContextFrame`); structured serialization in `jsg::Serializer`'s format (`ser`) |
| `jsg-macros/`           | Proc macros: `#[jsg_struct]`, `#[jsg_method]`, `#[jsg_resource]`, `#[jsg_oneof]`, `#[jsg_static_constant]`, `#[jsg_constructor]`, `#[jsg_property]`, `#[jsg_inspect_property]`, `#[jsg_wildcard_property]`                   |
| `jsg-test/`             | Test harness (`Harness`) for JSG Rust bindings                                                                                                                                                                                |
| `api/`                  | Rust-implemented Node.js APIs; registers modules via `register_nodejs_modules()`                                                                                                                                              |
| `encoding/`             | WHATWG legacy text decoders via `encoding_rs`; an opaque streaming `Decoder` emits UTF-16 over the CXX bridge                                                                                                                 |
| `kj/`                   | Rust bindings for KJ library (`http`, `io`, `own` submodules); `Result<T>` = `Result<T, cxx::KjError>`                                                                                                                        |
| `cxx/kj-hyper/`         | HTTP/1.1, WebSocket and TLS for the Rust server (hyper, rustls; WebSockets are kj's over the upgraded transport) with kj-typed seams: `server::serve_connection` dispatches to a `Handler` with kj arguments, `client::Client` implements `kj::http::Service`; see `src/rust/cxx/AGENTS.md`   |
| `worker/`               | Rust counterpart of `workerd::WorkerInterface`: the `worker::Interface` trait (`into_kj` hands an implementation to C++ as `KjOwn<WorkerInterface>`; `not_supported` answers a `CustomEvent` as C++'s `event->notSupported()` does), `PromisedInterface` (an `Interface` whose target is still starting, as C++'s `PromisedWorkerInterface`) plus FFI bindings; multi-bridge crate |
| `async-trace/`          | Async activity tracking core (`async_hooks`-style): per-isolate `IsolateState`, per-IoContext `Tracker` (turns, scopes, binding adoption, stats), `Sink` trait, NDJSON and recording sinks; opaque `Isolate`/`Tracker`/`Writer` over CXX |
| `cxx-integration/`      | One bridge function, `trigger_panic`: the hook C++ tests use to exercise the panic-to-`kj::Exception` conversion at the bridge                                                                                                 |
| `cxx-integration-test/` | Non-production crate exercising Rust/C++ integration: callbacks, shared structs, `Result` error mapping                                                                                                                       |
| `transpiler/`           | TS type stripping via SWC (`ts_strip()`, `StripOnly` mode)                                                                                                                                                                    |
| `python-parser/`        | Python import extraction via `ruff_python_parser`; **namespace: `edgeworker::rust::`**                                                                                                                                        |
| `gen-compile-cache/`    | Binary crate — V8 bytecode cache generator; calls C++ `compile()` via CXX                                                                                                                                                     |

## CONVENTIONS

- **CXX bridge**: `#[cxx::bridge(namespace = "workerd::rust::<crate>")]` with companion `ffi.c++`/`ffi.h` files
- **Namespace**: `workerd::rust::*` for crates in this directory, except `python-parser` → `edgeworker::rust::python_parser`; a crate living beside C++ uses that C++'s namespace (e.g. `workerd::server::cli`)
- **Errors**: `thiserror` for library crates; `jsg::Error` with `ExceptionType` for JSG-facing crates
- **JSG nullish values**: `Option<T>` is `T | undefined`; `Nullable<T>` is `T | null` (coerces `undefined` to `Null`, but method arguments remain required). Prefer `NullOrUndefined<T>` to `Option<Nullable<T>>` for three distinct `Some(T)`, `Null`, and `Undefined` states; omitted arguments become `Undefined`. `Lenient<T>` additionally preserves shallow mismatches as `Unconvertable`; converting it to `NullOrUndefined<T>` maps those to `Undefined`, and `Lenient::to_parts()` also returns the original JS value. See [Rust JSG nullish values](jsg/README.md#nullablet-and-nullorundefinedt).
- **JSG resources**: `#[jsg_resource]` on struct + impl block; `#[jsg_method]` auto-converts `snake_case` → `camelCase`; methods with `&self` become instance methods, methods without a receiver become static methods, and any other receiver is a compile error (JavaScript can re-enter a resource, so state that methods change goes in `Cell`/`RefCell` fields; never hold a `RefCell` borrow across a call that can run JavaScript, nor a `borrow_mut()` of one holding GC references across a call that can allocate on the V8 heap, wrap the resource or create or drop a `jsg::Rc` to it, since tracing skips a mutably borrowed `RefCell`); a method or accessor can take `this: jsg::Rc<Self>` as its first parameter instead of `&self` to get a `jsg::Rc` of its resource (like `JSG_THIS`) to hand to a callback; don't share the resource's state with a callback through an `std::rc::Rc` (see `jsg/README.md` §Reaching the resource from a callback); a `#[jsg_method]` is registered with the type's `v8::Signature`, so its `&self` callback resolves the receiver without the `WORKERD_RUST_WRAPPABLE_TAG` marker check (`WrappableRc::resolve_method_receiver`); callbacks without a signature (accessors, inspect properties, wildcard interceptors) use `resolve_receiver`, which reads the marker before unwrapping so that it accepts any value; an eligible `#[jsg_method]` also gets a V8 Fast API callback (types in `jsg/fast_api.rs`) and must not call into V8; a `#[jsg_constructor]` returns `Self` or `Result<Self, E>` (an `Err` is thrown) and throws `TypeError` when called without `new`; `#[jsg_static_constant]` on `const` items exposes read-only numeric constants on both constructor and prototype (name kept as-is, no camelCase); `#[jsg_resource(nested_types(Foo, Name = Bar))]` on the impl block puts other resources' constructors on the prototype (`JSG_NESTED_TYPE`); resources integrate with GC via `Traced` + `GarbageCollected`: every named field is traced via `Traced::trace(&self.field, visitor)` and all non-traceable types use no-op `Traced` impls
- **Resources created by C++**: C++ creates a Rust resource through an `extern "Rust"` function of the crate that defines it, which returns the wrapper through `jsg::v8::run_ffi_callback`; C++ holds the wrapper (a `jsg::JsRef`), since it has no typed reference to a Rust resource, and checks a value's type through one that returns `jsg::resource::ffi_is::<R>`, a `kj::Maybe<bool>` that is none when an exception is pending. See `jsg/README.md` §Creating resources from C++.
- **Custom JSG registration**: `#[jsg_resource(custom_members)]` on an impl generates member factories but leaves `impl Resource` to the author. `Resource::members(flags)` uses ordinary Rust to select members, order, and property placement. Factories are `<rust_name>_member()` (constant names lowercased); ordinary property factories are `<getter_name>_member(PropertyPlacement)` (`Prototype`, `Instance` or `LazyInstance`) and include the paired setter. See `jsg-macros/README.md` §Custom member registration.
- **JSG properties**: three property macros on `#[jsg_resource]` impl blocks — `#[jsg_property(prototype|instance|lazy_instance [, name = "..."] [, readonly])]` (`prototype` registers an accessor on the prototype, equivalent to `JSG_PROTOTYPE_PROPERTY`; `instance` registers an own data property via V8 `SetNativeDataProperty`, equivalent to `JSG_INSTANCE_PROPERTY`; `lazy_instance` registers an own data property via V8 `SetLazyDataProperty` whose getter runs once, on first access, equivalent to `JSG_LAZY_INSTANCE_PROPERTY`: it has no setter, is writable unless `readonly` (`JSG_LAZY_READONLY_INSTANCE_PROPERTY`); declaration order sets `Object.keys()` order for automatic registration; `readonly` is a compile-time check preventing a paired setter; `name = "..."` overrides the JS name; prefer `prototype` in almost all cases), `#[jsg_inspect_property]` (registered under a unique symbol, invisible to normal enumeration and string-key lookup, surfaced by `node:util` `inspect()`, equivalent to `JSG_INSPECT_PROPERTY`), and `#[jsg_wildcard_property]` (answers string-named properties the object and its prototypes lack, at most one per type, equivalent to `JSG_WILDCARD_PROPERTY`; see `jsg/README.md`); setter auto-detected from `set_` prefix; read-only when no setter present; getter/setter `.length` and `.name` are set correctly when `spec_compliant_property_attributes` compat flag is enabled
- **`jsg::v8::Global` / `TracedGlobal`**: persistent handles. A `Global` is for handles Rust owns outside resources: GC never visits it, so it is always strong, holds its handle inline (no box), has no `Traced` impl (it cannot be a resource field), and is `Send` and `Sync`. A `TracedGlobal` is for resource fields, which GC traces to collect cycles; it is `Send` (not `Sync`). Either may be dropped on any thread. Under the isolate lock its strong handle is reset at once; without the lock it is queued and reset at the next `IsolateBase::Lock`, through `IsolateBase::applyDeferredActions()`; once the `Realm` is dropped (under the lock, before `Dispose`) the handles are left to `Dispose`. See `jsg/isolate_state.rs`. Hand-written FFI that resets a V8 handle directly must hold the lock. Rust never resets a `TracedGlobal`'s traced reference: every GC visit lets go of it and makes the strong handle strong again (`ClearWeak`) before C++ installs a new one if needed, and a drop just lets go of it; V8 frees untraced nodes at the next full GC. See `GcVisitor::visit_traced_global`. A `TracedGlobal` keeps its handles in a `Box<GlobalSlot>`, since V8 holds the address of a weak handle's slot; the slot of a `TracedGlobal` dropped after the `Realm` is kept until the isolate is disposed
- **JSG parameters**: function `.length` is the Web IDL required-argument count under `spec_compliant_property_attributes` (`this: jsg::Rc<Self>` and a leading `&mut Lock` are not counted; counting stops at the first type whose `FromJS::is_optional()` returns true), so put optional parameters last
- **`Traced`**: core tracing trait in `jsg::wrappable`; built-ins include no-op impls for primitives/value types and delegating impls for wrappers/collections (`Option`, `Nullable`, `NullOrUndefined`, `Lenient`, `Vec`, maps/sets, `Cell`, `jsg::Rc`, `jsg::Weak`, `jsg::v8::TracedGlobal`); `jsg::v8::Global` deliberately has none
- **Sequences to JavaScript**: `Vec<T>` converts to an `Array`, numbers included (`vec![1u8, 2]` makes `[1, 2]`). A typed array or an `ArrayBuffer` is an explicit conversion of a slice: `to_js_typed_array(lock)` makes the typed array of the element type, and `to_js_array_buffer(lock)` makes an `ArrayBuffer` of bytes (`[u8]` only), as C++ JSG returns for a `kj::Array<kj::byte>`. See `jsg/README.md` §Arrays, typed arrays and `ArrayBuffer`s
- **`#[jsg_resource(custom_trace)]`**: suppresses the auto-generated `Traced` impl so the user can write their own; `GarbageCollected` (`memory_name`), `jsg::Type`, `jsg::ToJS`, and `jsg::FromJS` are still generated
- **Rust JSG safety rules** (full text and examples in `src/rust/jsg/README.md` §Safety rules):
  1. Never throw while execution is terminating, and never discard a `jsg::Error` from a call that ran JavaScript or from `ToJS::to_js`, which can fail; pass it on with `?`.
  2. Never put traced state (`TracedGlobal`, `jsg::Rc`, or a type holding them) behind `std::rc::Rc`. To give a callback or another party access, give it a `jsg::Rc` or `jsg::Weak` to the resource that owns the state (a method gets one as `this: jsg::Rc<Self>`). Clone a `jsg::Rc` right after moving it out of a traced field (`take`, `replace`, `pop`), since the moved one is neither strong nor traced ([#7976](https://github.com/cloudflare/workerd/issues/7976)). Don't put a `Global` in anything GC traces.
  3. The C++ shims call V8 directly, not C++ JSG (`JSG_TRY`, `jsg::check`, `jsg::Lock::from`, `createTunneledException`, `makeInternalError`, `jsg::Data`, ...). `//src/rust/jsg:cpp-jsg-allowlist-test` enforces this for the files in `CPP_SHIMS` in `jsg/BUILD.bazel`; add shim files outside `src/rust/jsg` there.
  4. `Local<'s>` and `Lock<'s>` stay in their scope, and futures passed to async I/O are `'static`.
- **Formatting**: `rustfmt.toml` — `group_imports = "StdExternalCrate"`, `imports_granularity = "Item"` (one `use` per import)
- **Linting**: `just clippy <crate>` for a crate here, `just clippy <label>` for one elsewhere — pedantic+nursery; `allow-unwrap-in-tests`
- **Lint defaults**: the `wd_rust_*` macros give every crate the lint levels in `//build/rust:lints`: `unsafe_code`, `unsafe_op_in_unsafe_fn`, `clippy::undocumented_unsafe_blocks` and `clippy::{unwrap_used, expect_used, panic, todo, unimplemented}` are denied. Do not repeat them as crate-root attributes, and do not relax them for a whole crate. The file or item that needs an exception says so, and says why: `clippy::allow_attributes_without_reason` is denied too, so every `allow` and `expect`, of any lint, carries a `reason`.
  - A module with `unsafe` in it, which includes every `#[cxx::bridge]` (the macro expands to `unsafe`), puts `#![allow(unsafe_code, reason = "...")]` at the top of its file. The layout to aim for is one such module per crate, `ffi.rs`, holding the bridge and every hand-written `unsafe` (`kj-rs-io`, `kj-rs-tokio`); put a new crate's bridge there.
  - In a crate root that attribute would cover the whole crate, so there the item takes `#[expect(unsafe_code, reason = "...")]` instead: the bridge module (after its `#[cxx::bridge]` line), or the function or `impl`.
  - What can fail returns a `Result`; a panic asserts what cannot happen. `unreachable!` says exactly that and is allowed everywhere. Any other deliberate panic takes `#[expect(clippy::<lint>, reason = "...")]` on its function.
  - Test code is not held to any of this. A module's `<module>-test.rs` is compiled only into the crate's `<name>_test` target, and test targets get no lint levels from the macros; neither do `testonly` crates (`wd_rust_crate(testonly = True)`: test harnesses, the Rust half of a C++ test). So a test file needs no opt-ins, and the production files are still checked where the library is built.
  - `#![forbid(unsafe_code)]` in a crate root (`memory-cache`, `jsg-macros`) is stronger than the default and stays.
  - The crates derived from upstream cxx (`src/rust/cxx/{src,syntax,gen,macro,tests}`) are outside these defaults; the `kj-rs*` crates beside them are inside.
- **Tests**: a module's unit tests live beside it in `<module>-test.rs` (`dns.rs` → `dns-test.rs`, `lib.rs` → `lib-test.rs`), never in an inline `mod tests { ... }`, so a diff shows production changes and test changes as separate files. See UNIT TEST FILES below. JSG tests use `jsg_test::Harness::run_in_context()`; `Harness::with_flags(|mut f| f.set_<flag>(true))` sets raw compat flags and uses workers' shared `configureJsgIsolate()` helper for isolate-level JSG settings. It starts with all flags false and applies no compatibility date or flag implications; see `jsg/README.md` §Testing with flags. `jsg_test::set_autogates(&["gate-name"])` configures the process's autogates for as long as the returned guard lives. Always run the full `src/rust/...` test suite (`bazel test //src/rust/...`) rather than targeting a single crate — changes in shared crates like `jsg` or `jsg-macros` can break downstream consumers
- **FFI pointers**: functions receiving raw pointers must be `unsafe fn` (see `jsg/README.md`)
- **`KjOwn<T>`**: dropping one runs `kj::Own<T>::~Own()` through a C++ function generated per type, so `T: kj_rs::OwnTarget` is implemented by the bridge that declares `T` (`type T;`) and holds it in a `KjOwn`. A bridge that only aliases `T` gets none; add `impl KjOwn<T> {}` to the declaring bridge. Details in `cxx/kj-rs/README.md` and `cxx/AGENTS.md`
- **Scopes and handles**: a `Lock<'s>` stands for the innermost open `HandleScope`. It is only handed out as `&'s mut Lock<'s>`, and every handle is a `Local<'s, T>` that cannot outlive it. A `Local` is a non-null pointer; a handle that may be empty is an `Option<Local>`. See `jsg/README.md`.
  - Generated callbacks get their lock from `jsg::function_callback` / `getter_callback` / `setter_callback`, which take a closure that captures nothing and need no `unsafe`. Other Rust code that C++ calls with a scope open uses the `unsafe` `jsg::callback_scope!`. A panic that unwinds into C++ aborts, so such an entry point pins a `jsg::CallbackScope` and opens it with `jsg::catch_panic`, or, when it returns a handle to C++, goes through `jsg::v8::run_ffi_callback`.
  - Code that only needs the isolate locked, with no context entered or scope open, takes `&mut jsg::Isolate`. `Lock` dereferences to it, and C++ hands one over through the `unsafe` `jsg::Isolate::from_raw`.
  - `src/rust/jsg-compile-tests` holds snippets that must not compile (or, prefixed `ok-`, must). Add one for every new lifetime or `Send`/`Sync` rule; the `//~ ERROR` annotations must not depend on how many errors rustc reports.
- **Parameter ordering**: `&Lock` / `&mut Lock` must always be the first parameter in any function that takes a lock (matching the C++ convention where `jsg::Lock&` is always first). This applies to free functions, trait methods, and associated functions (excluding `&self`/`&mut self` receivers and a JSG method's `this: jsg::Rc<Self>`, which come before `lock`).
- **Method naming**: do not use `get_` prefixes on methods — e.g. `buf.backing_store()` not `buf.get_backing_store()`. Static constructors belong on the marker struct (`impl ArrayBuffer { fn new(...) }`) not on `impl Local<'_, ArrayBuffer>`.
- **`v8` module conventions**: handles in, handles out; `FromJS`/`ToJS` are the only conversions between Rust and JavaScript values. A `v8` method returns a Rust value only where V8 returns a C scalar, from a typed handle (`value`, `length`, `is_*`, `value_of`) or from a coercion of any value (`int32_value`, `uint32_value`, `integer_value`, which return `Result`), or copies memory out (`to_vec`, `to_string`), and takes raw Rust data only where V8 takes C data (constructor input, indices, times, `&str` property keys). So `Function::call` returns a `Local<Value>`; for a Rust value write `let result = f.call(lock, None, &[])?;` then `f64::from_js(lock, result)?`. Anything that can run JavaScript or needs a context returns `Result`; `Option` means only that something is absent or that V8 refused with nothing pending (the `String` constructors past `MAX_LENGTH`, except `new_from_utf8_literal`, which panics); never an infallible signature over a C++ shim that can throw. Names: `as_*` borrows for free, `to_*` costs something, `value`/`*_value` read a scalar, named as in V8. Full text in `src/rust/jsg/README.md` §Conventions of the `v8` module.
- **FFI naming**: instance methods on an existing handle use a `local_<type>_<method>` prefix (e.g. `local_array_buffer_byte_length`). Static constructors that create a new value do **not** use the `local_` prefix — name them `<type>_<method>` (e.g. `array_buffer_new_with_mode`, `array_buffer_maybe_new`, `backing_store_new_resizable`).
- **FFI groups**: `v8.rs` `mod ffi`, `ffi.h`, and `ffi.c++` all use matching comment groups (e.g. `// Local<T>`, `// Local<Array>`, `// Local<TypedArray>`, `// Local<ArrayBuffer>`, `// Local<ArrayBufferView>`, `// Local<SharedArrayBuffer>`, `// BackingStore`, `// Unwrappers`, `// TracedGlobal<T>`, `// FunctionCallbackInfo`). When adding new FFI functions, place them in the correct group in **all three files**. Do not scatter related functions across groups.
- **Feature flags**: `Lock::feature_flags()` returns a capnp `compatibility_flags::Reader` for the current worker. Use `lock.feature_flags().get_node_js_compat()`. Flags are parsed once and stored in the `Realm` at construction; C++ passes canonical capnp bytes to `realm_create()`. Schema: `src/workerd/io/compatibility-date.capnp`, generated Rust bindings: `compatibility_date_capnp` crate.

## UNIT TEST FILES

This applies to every Rust crate in the repository, wherever it lives. The module under test declares its tests as the last item in the file:

```rust
#[cfg(test)]
#[path = "dns-test.rs"]
mod tests;
```

`dns-test.rs` sits in the same directory and holds what would otherwise be the body of `mod tests { ... }`. It is an ordinary child module: `use super::*;` reaches the private items of the module under test, and a module-wide lint attribute is an inner attribute (`#![expect(...)]`) at the top of the file. The `#[path]` is required because a hyphen cannot appear in a module name.

- One test file per source file, and the module is always named `tests`.
- Production files contain no `#[test]` functions and no inline `#[cfg(test)]` modules. A helper that only tests use belongs in the test file.
- `wd_rust_crate`, `wd_rust_binary` and `wd_rust_proc_macro` take the `*-test.rs` files out of the crate's `srcs` and compile them only into its `<name>_test` target (`split_rust_test_srcs` in `build/wd_rust_test.bzl`). A crate defined with `rust_library` directly does the same by hand: `exclude = ["*-test.rs"]` in the library's `srcs` glob, and `compile_data = glob(["*-test.rs"])` on its `wd_rust_test` (a `rust_test` built from a `crate` rejects `srcs`). An unconditional `mod` declaration of a test file therefore fails the production build.
- Crates that are test code throughout are exempt: a test-support crate in a `tests/` directory (e.g. `cxx/kj-rs/tests/`), and a `wd_rust_test` built from its own `srcs` (e.g. `tsan/`).

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

The reverse direction has a stricter, non-optional rule: any `extern "C++"` shim that can throw (`jsg::check()`, `KJ_REQUIRE`/`KJ_ASSERT`, constructing a `rust::String`/`rust::Vec` that can panic, etc.) **MUST** return `Result<T>`. The fork's generated shim catches every C++ exception at the bridge. For an infallible signature, ordinary exceptions become a Rust panic, which callers cannot handle; a `Result<T>` signature turns them into an `Err` instead (`//src/rust/cxx/gen/src/write.rs`, `write_cxx_function_shim`). `kj::CanceledException` is the exception: with either signature it propagates as a dedicated Rust panic, so cancellation unwinds rather than surfacing as an `Err` (`//src/rust/cxx/src/result.rs`, `into_result`). See `//src/rust/jsg/v8.rs` "Unwrappers" section for worked examples: `unwrap_string` is fallible (it constructs a `rust::String`), while `unwrap_boolean` and `unwrap_number` are the deliberate infallible counterexamples (no `jsg::check()`, no fallible construction). Fallible shims typically funnel their C++-side `TryCatch`/`KJ_REQUIRE` handling into a `cxx::KjException`, which `impl From<cxx::KjException> for jsg::Error` (`//src/rust/jsg/lib.rs`) then converts back into a throwable JS error on the Rust side. That conversion keeps only the error's type and message. A shim that can run JavaScript (a call, a coercion, a property read, array iteration), whose exception must reach JavaScript unchanged as in C++ JSG, instead leaves the exception pending and returns an empty result or `false` (`local_function_call`, `unwrap_string`, `local_object_get_property`), which Rust turns into an `Error` for which `is_pending()` is true and `Lock::throw_exception` throws nothing (all through `call_into_js()` in `jsg/v8.rs`, which also refuses to call while execution is terminating); `Lock::try_catch` catches such exceptions explicitly, and a shim that catches one in C++ decides with `catch_exception()` (`ffi.h`), which applies `jsg::Lock::tryCatch()`'s termination rule.

## CXX BRIDGE: BUILD WIRING

`wd_rust_crate` (and `wd_rust_binary`) generate, for each `cxx_bridge_src` / `cxx_bridge_srcs` entry, a companion `:<bridge>@cxx` cc_library — the C++ side of the bridge: the cxx-generated `<bridge>.rs.{h,cc}` plus the headers the bridge `include!()`s (`cxx_bridge_hdrs`, by default every `**/*.h` in the package). C++ consumers `#include` the generated header at the package's C++ include path — `<workerd/rust/<pkg>/<bridge>.rs.h>` for `src/rust/<pkg>`, `<workerd/server/<bridge>.rs.h>` for `src/workerd/server` — and depend on the crate; depend on `:<bridge>@cxx` alone if you only need the header. Any crate with a bridge auto-gets `//src/rust/cxx:cxx` and `//src/rust/cxx/kj-rs`.

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

**V8 handles must always cross the FFI as the shared `jsg::v8::ffi` types, never as a bare `usize`.** When another crate's bridge passes a V8 `Local`/`Global`, reuse the jsg shared struct via a type alias (`type Local = jsg::v8::ffi::Local;`) plus `include!("workerd/rust/jsg/v8.rs.h")`, and depend on `//src/rust/jsg` (which supplies the generated header transitively). Do not smuggle the handle word through a `usize` — the shared type keeps both sides in one canonical, cxx-verified definition. See `node-exceptions/lib.rs`. A handle that may be empty crosses as `ffi::MaybeLocal`, because cxx cannot name `Option<Local>`; convert it with `Local::from_ffi_maybe` / `Local::into_ffi_maybe` at the boundary and use `Option<Local>` everywhere else. The one exception is the `extern "C"` instance-property callbacks that `jsg` hands V8 as `AccessorNameGetterCallback`/`AccessorNameSetterCallbackV2`: V8 calls them directly with `v8::Local` arguments, so they take each handle as a `usize`, and `ffi.c++` static-asserts that `v8::Local` is one trivially copyable pointer. Keep that confined to `jsg`.

### Testing crates that cross the FFI

A `rust_test` exercising code that calls into heavy C++ (V8, etc.) must link those impl symbols itself via `test_deps` — the production binary already links them, so only the standalone test binary needs them. Symptom if missing: undefined C++ symbols at test link.
