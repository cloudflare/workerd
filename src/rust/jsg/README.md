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

    // jsg::v8::TracedGlobal<T> fields — JS value traced with strong↔weak dual-mode switching.
    // Allows GC to detect and collect back-reference cycles (e.g. a stored callback
    // that closes over the resource's own JS wrapper).
    // Must be wrapped in Cell<_> for interior mutability (trace takes &self).
    callback: Cell<Option<jsg::v8::TracedGlobal<jsg::v8::Value>>>,
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
- **Tracing**: The `#[jsg_resource]` macro auto-generates `Traced::trace` and calls it on every field:

| Field type | Traced? | Notes |
|---|---|---|
| `jsg::Rc<T>` | Yes — strong edge | Keeps target alive through GC |
| `Option<jsg::Rc<T>>` | Yes — when `Some` | |
| `jsg::Nullable<jsg::Rc<T>>` | Yes — when `Some` | |
| `jsg::Lenient<jsg::Rc<T>>` | Yes — when `Some` or `Unconvertable` | |
| `Cell<jsg::Rc<T>>` | Yes — strong edge | Use `Cell` when field needs interior mutability |
| `Cell<Option<jsg::Rc<T>>>` | Yes — when `Some` | |
| `Cell<jsg::Nullable<jsg::Rc<T>>>` | Yes — when `Some` | |
| `Vec<jsg::Rc<T>>` | Yes — each element | Iterates and visits every `Rc` in the vec |
| `HashMap<K, jsg::Rc<T>>` | Yes — each value | Iterates `.values()` and visits each `Rc` |
| `BTreeMap<K, jsg::Rc<T>>` | Yes — each value | Iterates `.values()` and visits each `Rc` |
| `HashSet<jsg::Rc<T>>` | Yes — each element | Iterates and visits every `Rc` |
| `BTreeSet<jsg::Rc<T>>` | Yes — each element | Iterates and visits every `Rc` |
| `Cell<Vec<jsg::Rc<T>>>` | Yes — each element | `Cell` variant of above |
| `Cell<HashMap<K, jsg::Rc<T>>>` | Yes — each value | `Cell` variant of above |
| Same patterns with `jsg::v8::TracedGlobal<T>` | Yes | All collection forms work with `TracedGlobal<T>` too |
| `jsg::v8::TracedGlobal<T>` | Yes — dual strong/traced | Enables cycle collection; see below |
| `Option<jsg::v8::TracedGlobal<T>>` | Yes — when `Some` | |
| `jsg::Nullable<jsg::v8::TracedGlobal<T>>` | Yes — when `Some` | |
| `Cell<jsg::v8::TracedGlobal<T>>` | Yes — dual strong/traced | Required when set after construction |
| `Cell<Option<jsg::v8::TracedGlobal<T>>>` | Yes — when `Some` | |
| `jsg::Weak<T>` | No | Doesn't keep target alive |
| `jsg::v8::Global<T>` | Compile error — no `Traced` impl | Use `TracedGlobal<T>` in resource fields |
| Any `T: Traced` | Depends on `T` | `#[jsg_resource]` calls `Traced::trace` on every field |

- **`Cell<T>` for interior mutability**: `Traced::trace` takes `&self`. Fields that need to be mutated after construction (e.g. a callback set in a method) can use `Cell<T>`. `Cell<T>` implements `Traced` by reading through `as_ptr()` during single-threaded GC tracing.
- **`Traced` drives field tracing**: `#[jsg_resource]` now traces every named field via `Traced::trace(&self.field, visitor)`. Types with no GC edges use no-op `Traced` impls; wrappers/collections delegate recursively.
- **Nested wrappers are supported by composition**: `Option<Vec<jsg::Rc<T>>>` works as long as each layer implements `Traced`.
- **`#[jsg_resource(custom_trace)]`**: suppresses the generated `Traced` impl so you can write your own. `GarbageCollected` (`memory_name`), `Type`, `ToJS`, and `FromJS` are still generated.
- **`jsg::v8::TracedGlobal<T>` cycle collection**: Uses the same strong↔traced dual-mode as C++ `jsg::V8Ref<T>`. While the parent resource has strong Rust refs the JS handle stays strong. Once all Rust `Rc`s are dropped, `visit_traced_global` downgrades the handle to a `v8::TracedReference` that cppgc can follow — allowing cycles (e.g. a resource holding a callback that captures its own wrapper) to be detected and collected.
- **Circular references** through `jsg::Rc<T>` are **not** collected, matching C++ `jsg::Rc<T>` behavior.

## V8 Handle Types

### `Lock<'s>` and handle scopes

`Lock<'s>` stands for "the isolate is locked and the `HandleScope` `'s` is the innermost open
one". Locks are only handed out as `&'s mut Lock<'s>`, borrowed from the pinned storage of
a `jsg::CallbackScope`, so no handle created through one (`Local<'s, _>`) can outlive the
scope. There is no public constructor; locks come from:

- `#[jsg_method]` / `#[jsg_constructor]` callbacks, one lock per callback. They are built
  with `jsg::function_callback`, which turns a closure that captures nothing into a V8
  function callback without any `unsafe`, as `rusty_v8` does; `#[jsg_property(instance)]`
  accessors likewise with `jsg::getter_callback` / `jsg::setter_callback`,
- `jsg::modules::add_builtin`, whose callback is a closure that captures nothing, of type
  `for<'s> Fn(&mut Lock<'s>) -> Local<'s, Value>`,
- `jsg::callback_scope!(unsafe let lock, isolate)` for other Rust code that C++ calls with a
  scope already open; the caller vouches for the isolate's state, hence `unsafe`. A panic
  that reaches C++ aborts, so entry points instead pin a `jsg::CallbackScope` and open it
  with `jsg::catch_panic`, which turns a panic into a JS internal error,
- `jsg_test::Harness::run_in_context`.

Code that only needs the isolate to be locked, with no context or scope, takes
`&mut jsg::Isolate`. A `Lock` dereferences to its isolate, and C++ that only locks the
isolate passes it through `unsafe` `jsg::Isolate::from_raw`.

All scopes open on a thread at the same time must belong to one isolate: opening a scope of
another isolate while one is open panics, so a handle is never used with another isolate's
lock. `jsg::allow_scopes_of_another_isolate` (`unsafe`) lifts the rule for code that must open
scopes of a second isolate. Handles that outlive scopes remember their isolate and panic when
used in another: `Global<T>`, `TracedGlobal<T>`, and the JS wrapper of a `jsg::Rc<T>`.

### `Local<'s, T>`

A `Copy` handle to a V8 value, valid for the scope `'s` it was created in. Every API that
creates a handle takes `&mut Lock<'s>` and returns `Local<'s, _>`; methods that may run
JavaScript take `&mut Lock<'s>` as well, and borrowed views into V8 memory
(`as_slice(&lock)`) borrow the lock, so no JavaScript can run while they are alive.

A `Local` is one non-null pointer, laid out like `v8::Local<T>`. It does not record its
isolate; methods that need the isolate take the lock. A handle that may be empty is an
`Option<Local<'s, T>>`, laid out like `v8::MaybeLocal<T>`: for example, the `String`
constructors return `None` when V8 cannot create the string.

A cast to a supertype, such as `Local<Function>` to `Local<Object>` or `Local<Value>`, is a
`From` and costs nothing, whichever supertype it targets. A cast to a subtype is a
`TryFrom`, which checks the value and fails with a `DataError`.

```rust
let str_value = "hello".to_js(lock);
let num_value = 42u32.to_local(lock); // `ToLocalValue` also covers integers
let global = str_value.to_global(lock);

let name: v8::Local<v8::Name> = v8::Local::<v8::String>::try_from(str_value).unwrap().into();
assert!(v8::Local::<v8::Function>::try_from(str_value).is_err());
```

### `Global<T>`

A persistent handle that outlives `HandleScope`s, for handles that Rust code owns outside
resources, such as the `Realm`'s resource templates. GC never visits it, so it is strong for as
long as it exists, and a cycle through it is never collected. Opening or cloning it in an isolate
other than the one it was created in panics. It holds its handle inline, with no box: V8 keeps
the address of a handle's slot only while the handle is weak. It is `Send` and `Sync`, since
nothing writes into it through a shared reference.

A `Global` has no `Traced` impl, so it cannot be a field of a `#[jsg_resource]`. A field holds a
`TracedGlobal`, which GC traces; `TracedGlobal::from` turns a `Global` into one.

Dropping a `Global` or a `TracedGlobal` resets its V8 handle, which needs the isolate lock; what
the drop does depends on the state of the isolate, which the `Realm` shares with every persistent
handle of it:

- Under the isolate lock, the handle is reset at once.
- Without the lock, on any thread, the handle is queued and reset the next time the isolate is
  locked, as C++ JSG's deferred destruction queue does. Until then it keeps its value alive. A
  `Global`'s handle is boxed to be queued.
- Once the isolate is being disposed, the handle is left alone: `v8::Isolate::Dispose` frees
  every handle of the isolate in bulk, and a reset after it would touch freed memory. The
  `Realm` is dropped under the lock, before `Dispose`, and closes the state then.

The queue is emptied from `IsolateBase::applyDeferredActions()`, which `IsolateBase::Lock` calls
whenever it locks the isolate, as workerd and the test harness do. A bare `v8::Locker`, or
relocking after a `v8::Unlocker`, does not empty it; the handles wait for the next
`IsolateBase::Lock`.

### `TracedGlobal<T>`

A persistent handle for a field of a `#[jsg_resource]`, which GC traces so that a cycle through it
can be collected. Opening or cloning it in an isolate other than the one it was created in panics,
and it is dropped like a `Global`. A `TracedGlobal` is `Send`, and not `Sync`: every use takes a
`Lock` except dropping it and putting it in a new resource (`jsg::Rc::new` takes no `Lock`), and
both leave its handles alone on a thread that does not hold the isolate lock.

A `TracedGlobal` in traced mode also holds a `v8::TracedReference`, which Rust never resets. A
`TracedGlobal` taken out of its resource, for instance by `Cell::take()`, is traced by nothing, so
the next full GC may free its node, and nothing tells the `TracedGlobal` that it moved, the way C++
JSG's `jsg::Data` move constructor re-strengthens its handle. So each GC visit lets go of the old
node and makes the strong handle strong again before C++ decides afresh whether to install a new
node, and a drop just lets go of it. V8 frees nodes that nothing traces at the next full GC; see
`GcVisitor::visit_traced_global` for the cost of this and how to avoid it.

Bitwise moves are also why a `TracedGlobal` keeps its handles in a box. In traced mode the strong
handle is weak, and V8 keeps the address of its slot to null it when the value dies; the box
keeps that address valid however the `TracedGlobal` itself moves, for example when the `Vec` holding
it grows. The box lives until the strong handle has been reset. GC can still run after the
`Realm` is dropped, so the box of a `TracedGlobal` dropped then is kept until the isolate is
disposed.

A `TracedGlobal` taken out of its resource is still weak, and nothing traces its value: unless
something else keeps the value alive, any GC from then on may free it, which leaves the
`TracedGlobal` empty, and `as_local` on it panics. Put the `TracedGlobal` back, or hold the value
through another handle, before a GC can run. Tracing likewise skips a `RefCell` while it is
mutably borrowed, so the `TracedGlobal`s in it are untraced for as long as the borrow lasts; debug
builds abort when that happens.

`TracedGlobal<T>` fields on `#[jsg_resource]` structs participate in GC tracing when visited via `GcVisitor::visit_traced_global`. This enables the garbage collector to detect and collect back-reference cycles — for example, a resource that stores a JS callback which closes over the resource's own JS wrapper:

```rust
#[jsg_resource]
struct EventEmitter {
    // Cell<Option<_>> for interior mutability: the callback is set after
    // construction, and trace() receives &self.
    on_event: Cell<Option<jsg::v8::TracedGlobal<jsg::v8::Value>>>,
}

#[jsg_resource]
impl EventEmitter {
    #[jsg_method]
    fn set_callback<'s>(&self, lock: &mut jsg::Lock<'s>, cb: jsg::v8::Local<'s, jsg::v8::Value>) {
        self.on_event.set(Some(cb.to_traced_global(lock)));
    }
}
```

Without tracing, storing a `TracedGlobal` back to the resource's own wrapper creates an unbreakable reference cycle that leaks until the worker is torn down. With `visit_traced_global` tracing (generated automatically by `#[jsg_resource]`), the cycle is collected by the next full GC after all strong Rust `Rc`s are dropped.

## Errors from JavaScript

When a function that `Local<Function>::call` calls throws, the exception stays pending in V8, as with C++ JSG's `JsExceptionThrown`, and `call` returns an error for which `Error::is_pending()` is true. `Lock::throw_exception` throws nothing for that error, so a callback that returns it propagates the original exception to its JavaScript caller. V8 keeps reporting the exception where it was thrown, and DevTools' "pause on uncaught exceptions" still sees whether JavaScript further up catches it. A termination is handled the same way.

Code that gets a pending error must not call into JavaScript, and should return the error. Converting it to another error and throwing that replaces the exception, as when C++ code throws while a `JsExceptionThrown` unwinds; nothing ever replaces a termination, and `call` refuses to run while execution is terminating. A pending error is only meaningful while its exception is pending: one kept past that, such as a clone stored after `try_catch` caught the exception, is thrown as an internal error when V8 has no exception pending.

To handle an exception instead, run the code in `Lock::try_catch`, which catches as C++ `jsg::Lock::tryCatch()` does: the exception, or an error the closure returns, as a value; a termination keeps propagating.

```rust
match lock.try_catch(|lock| callback.call::<Number>(lock, None, &[]))? {
    Ok(result) => { /* ... */ }
    Err(exception) => { /* handle what the callback threw */ }
}
```

## Type Conversions

The module docs of `wrappable.rs` list every Rust type that converts to or from JavaScript, and
how. Most convert both ways. Some convert only from JavaScript, so they can be method parameters
but not return values:

- `u8`, `u16`, `u32`, `i8`, `i16` and `i32`: a value converted to a number, which must be in range.
  Like C++ JSG's integers.
- `i64`: a `BigInt` that fits, or any other value converted to a number, which must be finite and
  in range, and truncates toward zero. Like C++ JSG's `int64_t`.
- `SystemTime`: a `Date`, or a number of milliseconds since the epoch, within the range of
  `kj::Date`. Like C++ JSG's `kj::Date`.
- `&[T]` and typed-array slices such as `&[u8]`.

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

### Testing with flags

`jsg_test::Harness::new()` starts with every compatibility flag false. Use `Harness::with_flags()`
to set individual flags before creating resource templates:

```rust
let harness = jsg_test::Harness::with_flags(|mut flags| {
    flags.set_node_js_compat(true);
    flags.set_set_to_string_tag(true);
});
harness.run_in_context(|lock, _ctx| {
    assert!(lock.feature_flags().get_node_js_compat());
    Ok(())
});
```

The harness sets raw flag values; it does not apply a compatibility date or resolve flag
implications. It uses the same `configureJsgIsolate()` helper as workers to apply isolate-level
JSG settings, including template behavior, Node.js compatibility, and module settings.
It does not reproduce worker-specific initialization such as logging, inspectors, or limits.

### How it works

1. During worker initialization, C++ canonicalizes the worker's `CompatibilityFlags` via `capnp::canonicalize()` and passes the bytes to `realm_create()`, which parses them once and stores the result in the per-context `Realm`.
2. `lock.feature_flags()` reads the cached `FeatureFlags` and returns its capnp reader. No copies or re-parsing on access.

### Compatibility-dependent API surfaces

`#[jsg_resource(custom_members)]` generates member factories while leaving `Resource::members(flags)`
to the resource author. Use ordinary Rust conditionals to select members and choose property placement
when the template is built. See [custom member registration](../jsg-macros/README.md#custom-member-registration)
for the factory naming conventions and a complete example.

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

- The method must be static (no `self` receiver) and must return `Self` or `Result<Self, E>`. An `Err` is converted with `Into<jsg::Error>` and thrown to the caller, as for `#[jsg_method]`.
- Only one `#[jsg_constructor]` is allowed per impl block.
- The first parameter may be `&mut Lock` (or `&mut jsg::Lock`) if the constructor needs isolate access; it is not exposed as a JS argument.
- If no `#[jsg_constructor]` is present, `new MyResource()` throws an `Illegal constructor` error, matching C++ JSG behavior.
- Calling the constructor without `new` throws a `TypeError`, matching C++ JSG behavior.

## Properties

Two macros expose properties on resource types: `#[jsg_property]` for prototype accessors and
instance data properties, and `#[jsg_inspect_property]` for debug-only symbol-keyed properties.

Full reference documentation (arguments, naming rules, compat-flag behavior, examples) lives in
**[`jsg-macros/README.md`](../jsg-macros/README.md#jsg_propertyrequired-placement--name---readonly)**.
The summary below covers the most common patterns:

```rust
use std::cell::{Cell, RefCell};
use jsg_macros::{jsg_resource, jsg_property, jsg_inspect_property};

#[jsg_resource]
impl Counter {
    // Prototype property (on the proto chain; use this in almost all cases)
    #[jsg_property(prototype)]
    pub fn get_value(&self) -> jsg::Number { /* ... */ }

    #[jsg_property(prototype)]
    pub fn set_value(&self, v: jsg::Number) { /* ... */ }

    // Read-only prototype property (no paired `set_value`)
    #[jsg_property(prototype, readonly)]
    pub fn get_label(&self) -> String { /* ... */ }

    // Own data property (use sparingly; inhibits minor-GC)
    #[jsg_property(instance)]
    pub fn get_id(&self) -> String { /* ... */ }

    // Inspect-only: shown by util.inspect() / console.log, invisible to JS
    #[jsg_inspect_property]
    pub fn debug_info(&self) -> String { /* ... */ }
}
```

Methods **must** start with `get_` (getter) or `set_` (setter). The prefix is stripped for the
JS name. Omitting the setter (or adding `readonly`) makes the property read-only.

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

## Nested Types

To expose other resource types' constructors on a resource class (equivalent to `JSG_NESTED_TYPE` and `JSG_NESTED_TYPE_NAMED` in C++), list them in `nested_types(...)` on the `#[jsg_resource]` impl block:

```rust
#[jsg_resource(nested_types(Database, Stmt = Statement))]
impl Module {}
// JS: instance.Database, instance.Stmt
```

Each nested type's constructor is a property of the prototype, named after the type's JavaScript class name unless given a name. It is the same function as `jsg::resource::function_template_of::<T>()`, so `instanceof` works on instances created from Rust. A nested type without a `#[jsg_constructor]` throws `Illegal constructor` when called with `new`.

- Nested types are added to the prototype after its other members, so they come last in `Object.keys(prototype)`.
- As in C++ JSG, V8 names a function after the property through which it is first instantiated. The `name` of a renamed nested type's constructor is the property name if the constructor is first reached through that property, for every later access too.
- A resource type must not nest itself, directly or through other nested types. Creating its template panics, naming the types in the cycle.
- Listing a type twice without a name, or giving a name twice, does not compile. `nested_types(...)` on the struct, or together with `custom_members`, does not compile either.

## Fast API

`#[jsg_method]` adds a V8 Fast API callback for the types in `fast_api.rs`; it must not call into V8.

## FFI Functions with Raw Pointers

Functions exposed to C++ via FFI that receive raw pointers must be marked `unsafe fn`:

```rust
pub unsafe fn realm_create(isolate: *mut v8::ffi::Isolate, feature_flags_data: &[u8]) -> Box<Realm> {
    // ...
}
```
