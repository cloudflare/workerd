# JSG (JavaScript Glue) Rust Bindings

Rust bindings for the JSG (JavaScript Glue) layer, enabling Rust code to integrate with workerd's JavaScript runtime.

## Core Types

### `Lock`

Provides access to V8 operations within an isolate lock. Passed to resource methods and callbacks.

### `Rc<R>`

Strong reference to a Rust resource managed by GC. Derefs to `&R`. Cloning and dropping a `Rc` tracks strong references for the garbage collector.

### `Weak<R>`

Weak reference that doesn't prevent GC collection. Use `upgrade()` to get a `Rc<R>` if the resource is still alive.

### `Realm`

Per-isolate state for Rust resources exposed to JavaScript. Stores cached function templates.

## Resources

Rust resources integrate with V8's garbage collector through the existing C++ `Wrappable` infrastructure — the same GC system that C++ `jsg::Rc<T>` and `jsg::Object` use.

```rust
use jsg_macros::{jsg_resource, jsg_method};
use std::cell::Cell;

#[jsg_resource]
struct MyResource {
    name: String,

    // jsg::Rc<T> fields — strong GC edges, automatically traced
    child: jsg::Rc<OtherResource>,
    maybe_child: Option<jsg::Rc<OtherResource>>,
    nullable_child: jsg::Nullable<jsg::Rc<OtherResource>>,

    // jsg::Weak<T> fields — weak reference, does not keep the target alive
    observer: jsg::Weak<OtherResource>,

    // jsg::v8::Global<T> fields — JS value traced with strong↔weak dual-mode switching.
    // Allows GC to detect and collect back-reference cycles (e.g. a stored callback
    // that closes over the resource's own JS wrapper).
    // Must be wrapped in Cell<_> for interior mutability (trace takes &self).
    callback: Cell<Option<jsg::v8::Global<jsg::v8::Value>>>,
}

#[jsg_resource]
impl MyResource {
    #[jsg_method]
    fn get_name(&self) -> Result<String, jsg::Error> {
        Ok(self.name.clone())
    }
}
```

### Lifecycle

```rust
// Create a resource
let resource = jsg::Rc::new(MyResource { ... });

// Convert to a JS object (uses cached FunctionTemplate)
let js_obj = resource.to_js(&mut lock);

// Convert from JS back to a Ref
let r: jsg::Rc<MyResource> = jsg::Rc::from_js(&mut lock, js_val)?;
```

### GC Behavior

- **No JS wrapper**: Dropping the last `Rc` immediately destroys the resource (no GC needed).
- **With JS wrapper**: Dropping all `Rc`s makes the wrapper eligible for V8 GC. When collected, the resource is destroyed.
- **Tracing**: The `#[jsg_resource]` macro auto-generates `GarbageCollected::trace` based on field types:

| Field type | Traced? | Notes |
|---|---|---|
| `jsg::Rc<T>` | Yes — strong edge | Keeps target alive through GC |
| `Option<jsg::Rc<T>>` | Yes — when `Some` | |
| `jsg::Nullable<jsg::Rc<T>>` | Yes — when `Some` | |
| `Cell<jsg::Rc<T>>` | Yes — strong edge | Use `Cell` when field needs interior mutability |
| `Cell<Option<jsg::Rc<T>>>` | Yes — when `Some` | |
| `Cell<jsg::Nullable<jsg::Rc<T>>>` | Yes — when `Some` | |
| `jsg::v8::Global<T>` | Yes — dual strong/traced | Enables cycle collection; see below |
| `Option<jsg::v8::Global<T>>` | Yes — when `Some` | |
| `jsg::Nullable<jsg::v8::Global<T>>` | Yes — when `Some` | |
| `Cell<jsg::v8::Global<T>>` | Yes — dual strong/traced | Required when set after construction |
| `Cell<Option<jsg::v8::Global<T>>>` | Yes — when `Some` | |
| `jsg::Weak<T>` | No | Doesn't keep target alive |
| Anything else | No | Plain data fields are ignored |

- **`Cell<T>` for interior mutability**: `GarbageCollected::trace` takes `&self`. Fields that need to be mutated after construction (e.g. a callback set in a method) must be wrapped in `Cell<T>`. Both `Cell<T>` and `std::cell::Cell<T>` are recognised.
- **`jsg::v8::Global<T>` cycle collection**: Uses the same strong↔traced dual-mode as C++ `jsg::V8Ref<T>`. While the parent resource has strong Rust refs the JS handle stays strong. Once all Rust `Rc`s are dropped, `visit_global` downgrades the handle to a `v8::TracedReference` that cppgc can follow — allowing cycles (e.g. a resource holding a callback that captures its own wrapper) to be detected and collected.
- **Circular references** through `jsg::Rc<T>` are **not** collected, matching C++ `jsg::Rc<T>` behavior.

## V8 Handle Types

### `Local<'a, T>`

A stack-allocated handle to a V8 value. The lifetime `'a` is tied to the `HandleScope` that created it.

```rust
let str_value = "hello".to_local(&mut lock);
let num_value = 42u32.to_local(&mut lock);
let global = local.to_global(&mut lock);
```

### `Global<T>`

A persistent handle that outlives `HandleScope`s. Must be explicitly managed.

`Global<T>` fields on `#[jsg_resource]` structs participate in GC tracing when visited via `GcVisitor::visit_global`. This enables the garbage collector to detect and collect back-reference cycles — for example, a resource that stores a JS callback which closes over the resource's own JS wrapper:

```rust
#[jsg_resource]
struct EventEmitter {
    // Cell<Option<_>> for interior mutability: the callback is set after
    // construction, and trace() receives &self.
    on_event: Cell<Option<jsg::v8::Global<jsg::v8::Value>>>,
}

#[jsg_resource]
impl EventEmitter {
    #[jsg_method]
    fn set_callback(&self, lock: &mut jsg::Lock, cb: jsg::v8::Local<jsg::v8::Value>) {
        self.on_event.set(Some(cb.to_global(lock)));
    }
}
```

Without tracing, storing a `Global` back to the resource's own wrapper creates an unbreakable reference cycle that leaks until the worker is torn down. With `visit_global` tracing (generated automatically by `#[jsg_resource]`), the cycle is collected by the next full GC after all strong Rust `Rc`s are dropped.

## Union Types

To accept JavaScript values that can be one of several types, define an enum with `#[jsg_oneof]`:

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


## Constructors

To allow JavaScript to create instances of a resource via `new MyResource(args)`, mark a static method with `#[jsg_constructor]`:

```rust
use jsg_macros::{jsg_resource, jsg_method, jsg_constructor};

#[jsg_resource]
struct Greeting {
    message: String,
}

#[jsg_resource]
impl Greeting {
    #[jsg_constructor]
    fn constructor(message: String) -> Self {
        Self { message }
    }

    #[jsg_method]
    fn get_message(&self) -> String {
        self.message.clone()
    }
}
// JS: let g = new Greeting("hello"); g.getMessage() === "hello"
```

**Rules:**
- The method must be static (no `self` receiver) and must return `Self`.
- Only one `#[jsg_constructor]` is allowed per impl block.
- The first parameter may be `&mut Lock` (or `&mut jsg::Lock`) if the constructor needs isolate access; it is not exposed as a JS argument.
- If no `#[jsg_constructor]` is present, `new MyResource()` throws an `Illegal constructor` error, matching C++ JSG behavior.

## Static Constants

To expose numeric constants on a resource class (equivalent to `JSG_STATIC_CONSTANT` in C++), use `#[jsg_static_constant]` on `const` items inside a `#[jsg_resource]` impl block:

```rust
use jsg_macros::jsg_static_constant;

#[jsg_resource]
impl WebSocket {
    #[jsg_static_constant]
    pub const CONNECTING: i32 = 0;

    #[jsg_static_constant]
    pub const OPEN: i32 = 1;
}
// JS: WebSocket.CONNECTING === 0, instance.OPEN === 1
```

Constants are set on both the constructor and prototype as read-only, non-configurable properties per Web IDL. The name is used as-is (no camelCase conversion). Only numeric types are supported (`i8`..`i64`, `u8`..`u64`, `f32`, `f64`).

## FFI Functions with Raw Pointers

Functions exposed to C++ via FFI that receive raw pointers must be marked `unsafe fn`:

```rust
pub unsafe fn realm_create(isolate: *mut v8::ffi::Isolate, feature_flags_data: &[u8]) -> Box<Realm> {
    // ...
}
```
