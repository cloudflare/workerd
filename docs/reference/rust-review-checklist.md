This document provides a checklist for reviewing Rust code in the workerd project. It covers style guidelines, common pitfalls, and critical patterns to watch for when ensuring code quality.

See `../../src/rust/jsg/README.md` for additional context.

### CXX Bridge Safety

All Rust/C++ interop uses the `cxx` crate. Each crate with a bridge declares
`#[cxx::bridge(namespace = "workerd::rust::<crate>")]` and has companion `ffi.c++`/`ffi.h` files.

- **Always** ensure that **shared structs** passed across the boundary are trivially safe to copy
  between languages.
- **Always** verify that shared types do not contain owning pointers, non-trivial destructors, or
  types whose layout differs between Rust and C++.
- **Always** verify that opaque types (`type Foo;` in CXX bridge) are passed by reference or
  `Box`/`UniquePtr`.
- **Always** verify that lifetimes are respected — an opaque C++ type behind `&T` must outlive the
  Rust reference.
- **Always** verify that bridges must use the `workerd::rust::<crate_name>` naming convention. The
  only exception is `python-parser` which uses `edgeworker::rust::python_parser`.
- **Always** verify companion files. Every CXX bridge should have hand-written `ffi.c++` and `ffi.h`
  implementing the C++ side. Generated headers (`<file>.rs.h`) are produced by the build system.

### Unsafe Code

Unsafe code is concentrated at FFI boundaries. Review every `unsafe` block and `unsafe fn`:

- **Always** verify that every `unsafe` block has a `// SAFETY:` comment explaining which invariants
  the caller is responsible for and why they hold at this call site.
- **Always** verify that functions receiving raw pointers from C++ are declared `unsafe fn`**.
- **Always** verify that `unsafe impl Send` / `unsafe impl Sync` are justified with a comment
  explaining why the type is safe to share across threads. `jsg::Ref<T>` is explicitly not `Send`
  (uses `Rc` + `UnsafeCell` internally) — flag any attempt to send it across threads.

#### Common unsafe patterns in this codebase:

- **V8 handle operations**: `Local::from_ffi()`/`into_ffi()`, isolate pointer dereference. These are
  safe only when the isolate is locked and the handle scope is active.
- **Resource wrap/unwrap**: `Ref::into_raw()` leaks a ref-counted pointer as `usize`;
  `Ref::from_raw()` reconstructs it. These must be balanced — every `into_raw` needs exactly one
  `from_raw` to avoid leaks or double-frees.
- **Trampoline closures**: A closure is cast to `usize` via raw pointer, passed through CXX, and
  reconstructed on the other side. The closure must be consumed exactly once.

### Error Handling

- **Always** ensure that library crates (e.g., `dns`, `net`, `transpiler`) use `thiserror` for
  domain-specific error types. These should implement `Display` and have descriptive error messages.
- **Always** ensure that JSG-facing crates (e.g., `api`, `jsg`) use `jsg::Error` with an
  `ExceptionType` variant (e.g., `TypeError`, `RangeError`). Domain errors should implement
  `From<DomainError> for jsg::Error` for ergonomic `?` usage.
- **Never** use `panic!` for expected errors. Panics across FFI are undefined behavior. Use
  `Result` and propagate errors through the CXX bridge. `unwrap()` / `expect()` are acceptable in
  tests (clippy is configured with `allow-unwrap-in-tests = true`).
- **Error type changes are generally not breaking** — same policy as the C++ side. Changing the
  JS exception type (e.g., from generic error to `TypeError`) is not normally a breaking change
  unless it removes properties that user code could depend on (e.g., `DOMException` has `code`
  that `TypeError` does not).

### JSG Resource Conventions

Rust types exposed to JavaScript via the JSG bindings follow these patterns:

- **`_state: jsg::ResourceState`** is a required field on all resource types. It holds the opaque
  pointers used by the C++ JSG layer to wrap/unwrap the Rust object.
- **`#[jsg_resource]`** on the impl block registers the type as a JS-visible resource.
- **`#[jsg_method]`** auto-converts Rust `snake_case` method names to JavaScript `camelCase`.
  Methods with a receiver (`&self`/`&mut self`) are registered as instance methods on the prototype;
  methods without a receiver are registered as static methods on the constructor.
  Verify the converted name is correct and matches the intended API surface.
- **`#[jsg_static_constant]`** on a `const` item inside a `#[jsg_resource]` impl block exposes it
  as a read-only numeric constant on both the constructor and prototype (Rust equivalent of
  `JSG_STATIC_CONSTANT`). The name is used as-is (no camelCase conversion).
- **`#[jsg_struct]`** is for value types (passed by value across the JS boundary).
- **`#[jsg_oneof]`** is for union/variant types (mapped from JS values by trying each variant).
- **Type mappings**: `jsg::Number` wraps JS numbers (distinct from `f64`). `Vec<u8>` maps to
  `Uint8Array`, not a regular JS `Array`. `Option<T>` maps to `T | undefined` (rejects `null`).
  `Nullable<T>` maps to `T | null | undefined`. `String`/`&str` map to JS strings.
- **GC tracing**: `Ref<T>`, `Option<Ref<T>>`, and `Nullable<Ref<T>>` fields on `#[jsg_resource]`
  structs are automatically traced during GC. `WeakRef<T>` fields are not traced (no-op). Verify
  that any resource holding a `Ref<T>` or `Nullable<Ref<T>>` to another resource is properly
  traced — missing traces cause use-after-free when the child is collected prematurely.

### Linting & Style

- **Clippy** runs with **pedantic + nursery** lint groups. Run `just clippy <crate>` on all
  Rust changes.
- **`#[expect(clippy::...)]` over `#[allow(clippy::...)]`** — `expect` is stricter: it warns if
  the suppressed lint no longer fires, preventing stale suppressions from accumulating.
- **Import formatting**: one `use` per import line, grouped as std / external / crate (enforced
  by `rustfmt.toml`). Run `just format` to auto-fix.

### Testing

- **JSG test harness** (`jsg_test::Harness`): creates a V8 isolate, runs Rust code in a real V8
  context via `run_in_context(|lock, ctx| { ... })`. Used for testing JSG resources.
- **C++ KJ tests for Rust crates**: some crates have companion `.c++` test files using `kj_test()`.
  These test the C++ side of the FFI bridge.
- **Inline `#[cfg(test)]` modules**: standard Rust unit tests for pure-Rust logic.
- All test targets run with `RUST_BACKTRACE=1` and `RUST_TEST_THREADS=1` (serial execution).

### Review Checklist

When reviewing Rust code in workerd, check for each of these items.

- **Never** use **magic numbers** (Numeric literals) without explanation or named constants.
- **Always** use `#[must_use]`. Functions returning `Result`, `Option`, or expensive-to-compute values
   that callers should not silently discard.
- **Never** use `bool` function parameters. Prefer an enum or options struct for clarity at call
  sites. E.g., `fn connect(secure: bool)` should be `fn connect(mode: SecureMode)` or take an
  options struct.
- **Always** use `const fn` / `const` for compile-time evaluable functions or constants where
  appropriate.
- **Always** avoid reinvented utility. Avoid custom code duplicating functionality already in the
  `jsg` or `kj` Rust crates, or in well-known ecosystem crates already vendored by the project.
- **Always** avoid using `static mut`. Use `OnceLock`, `LazyLock`, or other safe synchronization
  primitives where global state is genuinely needed.
- **Always** flag missing `// SAFETY:` comments on `unsafe`.
- **Always** flag unjustified `unsafe impl Send/Sync` uses.
- **Always** flag unnecessary `.clone()`/copies where borrowing or moving would suffice.
- **Always** flag unnecessary use of `String` where `&str` works, or `Vec<T>` where a slice would do.
- **Always** flag locations where **`#[allow(...)]` is used where `#[expect(...)]` would work**.
  Prefer `expect` to prevent stale suppressions.
- **Always** ensure copyright header on new files, Every new `.rs` file must begin with the project
  copyright/license header using the current year. Expected format:
    ```
    // Copyright (c) <current-year> Cloudflare, Inc.
    // Licensed under the Apache 2.0 license found in the LICENSE file or at:
    //     https://opensource.org/licenses/Apache-2.0
    ```
    Flag any new file that uses a stale year (e.g., `2017-2022` in a file created in 2026) or omits
    the header entirely.
