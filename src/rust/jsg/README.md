# JSG (JavaScript Glue) Rust Bindings

Rust bindings for the JSG (JavaScript Glue) layer, enabling Rust code to integrate with workerd's JavaScript runtime.

## RTTI and TypeScript declarations

`TypeInfo::describe()` describes a binding's JavaScript representation without
constructing a resource or acquiring a V8 lock. `#[jsg_struct]`, `#[jsg_resource]`,
and `#[jsg_oneof]` generate implementations that compose the descriptions of
their fields, signatures, and variants. Rust resolves aliases and imported types
through trait dispatch. Custom conversion types implement `TypeInfo` explicitly.

- One `jsg::rtti::Context` collects all modules in an export, interning shared and
  recursive definitions before visiting their members.
- Rust's fully qualified type identity is separate from the declaration name,
  which honors `name = "..."`. Explicit names must be unique among emitted Rust
  and C++ declarations; inactive compatibility variants may reuse a name.
- `Option<T>` describes `T | undefined` and `Nullable<T>` describes `T | null`,
  so a `Nullable<T>` parameter is required, as at runtime. `NullOrUndefined<T>`,
  `Option<Nullable<T>>` and `Lenient<T>` describe an optional `T | null | undefined`.
- `Dict<V>` describes `Record<string, V>`, as C++ describes `jsg::Dict`.
- Vectors and slices describe JavaScript arrays, numbers included; a typed array
  is described by its `v8` handle type, such as `v8::Local<v8::Uint8Array>`.
- Integers of up to 32 bits describe `number`; `i64` and `u64` describe
  `number | bigint`, as C++ RTTI describes `int64_t`.
- `v8::Promise` describes `Promise<any>`; as in C++, a promise parameter also
  accepts a value that is not a promise.
- Fallible returns describe their success type. Injected `this` and `Lock`
  parameters and inspect-only properties are omitted.
- A nested type is described with its whole structure, as in C++, so it is
  declared as a class even when nesting is the only way to reach it.
- Parameter names use camelCase, omit unused-parameter underscores, and escape
  TypeScript reserved words. Bindings with colliding normalized parameter names
  are rejected. Members are described in registration order, as in C++.

Both C++ and Rust emit `src/workerd/jsg/rtti.capnp` for the `types/` generator.
Resources are described with the members they register under the context's
compatibility flags; in `workerd:rtti`, these are the flags `jsg::rtti::Builder`
gives C++ resources.

Register modules with a callback receiving `&mut Lock<'s>` and returning
`jsg::Result<Rc<Exports>>`, for example `jsg::modules::add_builtin(registry,
"node-internal:dns", |_lock| Ok(DnsUtil::new()), ModuleType::Internal)`. The return
type determines both runtime wrapping and RTTI. `workerd:rtti` collects each module's
export type and reachable definitions without invoking the callback, using the same
context for every module. Registration-time compatibility gates also govern RTTI
discovery.

Definitions are emitted into ambient TypeScript only when reachable from a
TypeScript root. Use `#[jsg_resource(ts_root)]` or `#[jsg_struct(ts_root)]` on a
struct to mark such a root, analogous to C++ `JSG_TS_ROOT`. Internal module
registration alone does not make a type an ambient declaration.

## Core Types

### `Lock`

Provides access to V8 operations within an isolate lock. Passed to resource methods and callbacks.

### `Rc<R>`

Strong reference to a Rust resource managed by GC. Derefs to `&R`. Cloning and dropping a `Rc` tracks strong references for the garbage collector.

### `Weak<R>`

Weak reference that doesn't prevent GC collection. Use `upgrade()` to get a `Rc<R>` if the resource is still alive.

### `Realm`

Per-isolate state for Rust resources exposed to JavaScript. Stores cached function templates.

### `Nullable<T>` and `NullOrUndefined<T>`

Use `Nullable<T>` for `T | null`: it coerces `undefined` to `null` on input and
method arguments remain required. Use `NullOrUndefined<T>` for `T | null | undefined`
with distinct `Some(T)`, `Null`, and `Undefined` states and optional method arguments.
Prefer it to `Option<Nullable<T>>`. Both wrappers trace their contained value.

| Rust type | Nullish input conversion | Omitted method argument |
|---|---|---|
| `Option<T>` | `undefined` → `None`; `null` is converted by `T` | `None` |
| `Nullable<T>` | `null` or `undefined` → `Null` | `TypeError` |
| `NullOrUndefined<T>` | `null` → `Null`; `undefined` → `Undefined` | `Undefined` |

`NullOrUndefined::default()` is `Undefined`, without requiring `T: Default`.
`as_ref()`, `as_mut()`, and `map()` preserve `Null` and `Undefined`.
Conversions to and from `Option<Nullable<T>>` preserve all three states;
`Nullable<T>` also converts into `NullOrUndefined<T>`.

In a `#[jsg_struct]`, a missing `NullOrUndefined<T>` field reads as `Undefined`.
When converting back to JavaScript, `Null` produces a `null` property; `Undefined`
is omitted without `fast_jsg_struct`, or becomes an own `undefined` property with
that flag, just as `None` does for an `Option<T>` field.

`Lenient<T>` additionally preserves shallow conversion mismatches as `Unconvertable`.
Converting it to `NullOrUndefined<T>` maps those mismatches to `Undefined`;
`Lenient::to_parts()` returns `(NullOrUndefined<T>, Option<TracedGlobal<Value>>)`,
with the original mismatched JavaScript value in the second element when present.
Errors from nested conversions or JavaScript execution still propagate.

## Safety rules

Every change to Rust JSG, and to the C++ shims under it, must follow these four rules. Each rule
lists the tests that check it. Reviewers check the rest.

### 1. Never drop or replace a pending termination

When V8 terminates a script (for example because the Worker ran out of CPU time), it throws a
special termination exception that JavaScript cannot catch. Once that exception is pending
(`isolate->IsExecutionTerminating()` in C++, `Isolate::is_execution_terminating` in Rust), it must
reach the C++ code that started the script.

Throwing anything else at that point replaces it: `v8::Isolate::ThrowException` discards the
pending termination and throws the new exception, which JavaScript can catch. The Worker then
keeps running.

- Don't throw while execution is terminating. The `isolate_throw_*` shims already check this
  and throw nothing, so `Lock::throw_exception`, `Lock::throw_error`, `Lock::throw_internal_error`
  and the fast API error path are safe. A shim that calls `isolate->ThrowException` itself must
  check first.
- Don't discard a `jsg::Error` from a call that ran JavaScript (`Function::call`, `Object::get`,
  `Object::set`, a coercion). It may be the termination, or a JavaScript exception that is still
  pending (see [Errors from JavaScript](#errors-from-javascript)). Pass it on with `?` instead of
  using `.ok()`, `unwrap_or`, or `map_err` to a different error. To handle the exception, use
  `Lock::try_catch`.
- An empty result from an FFI shim doesn't always mean termination. It can also mean a stack
  overflow or an ordinary exception. Check `IsExecutionTerminating()`, or catch the exception
  with a `v8::TryCatch` in the shim and decide with `catch_exception()` (`ffi.h`).

Bad: `let name = obj.get(lock, "name").ok();`. If the script is being terminated, the termination
is lost. Fix: `let name = obj.get(lock, "name")?;`.

Tested by: `src/rust/jsg-test/tests/termination.rs`, and the termination tests in
`src/rust/jsg-test/tests/resource_callback.rs`.

### 2. Keep traced state with its one GC owner

Traced state is a `TracedGlobal`, a `jsg::Rc`, or a type that holds them. It belongs to the one
GC owner that traces it: a resource, or a `Wrappable` such as the data of a function created with
`Function::new`.

- Never put traced state inside an `std::rc::Rc`. A clone of the `Rc` held by a second GC owner
  traces the same state again. For a `jsg::Rc`, C++ asserts a single owner
  (`Wrappable::visitRef` in `src/workerd/jsg/wrappable.c++`), so the process aborts. For a
  `TracedGlobal`, nothing asserts, but whichever owner traces it last decides whether its handle
  is strong or weak. The value can then be collected while the other owner still uses it. To give
  a callback or another party access to the state, give it a `jsg::Rc` or `jsg::Weak` to the
  resource that owns the state. A method gets one with a `this: jsg::Rc<Self>` parameter; see
  [Reaching the resource from a callback](#reaching-the-resource-from-a-callback).
- Clone a `jsg::Rc` that you move out of a traced field (with `Cell::take`, `Option::take` or
  `mem::replace`, or by popping it from a traced `Vec`), and use the clone. Once GC has visited a
  `jsg::Rc` in a field, the `jsg::Rc` gives up its strong reference and relies on the field's
  owner to trace it. Moved out of the field, it is neither strong nor traced, so GC can collect
  the resource's JavaScript wrapper while Rust still holds it. The resource then gets a new
  wrapper the next time it reaches JavaScript, and nothing traces the `TracedGlobal`s it holds,
  so they can lose their values. C++ `jsg::Ref` doesn't have this problem because its move
  constructor makes the reference strong again, but Rust has no hook on moves. A clone always
  starts strong. Clone it right away, or at least before anything that can run JavaScript or
  allocate on the V8 heap. See [#7976](https://github.com/cloudflare/workerd/issues/7976).
- Don't put a `jsg::v8::Global` in anything GC traces. A `Global` is always strong, so if it
  points back at its owner (for example function data that holds the function itself), neither
  is ever collected. Use a `TracedGlobal`. The compiler rejects a `Global` field in a resource,
  but not inside a type with a hand-written `impl Traced`, so check those by hand.

Bad: two `Function::new` callbacks whose data are clones of one `std::rc::Rc<State>`, where
`State` holds a `TracedGlobal`. Fix: a resource holds `State`, and each callback holds a
`jsg::Rc` or `jsg::Weak` to that resource.

Bad: `let task = self.pending.take().unwrap();`, then a call into JavaScript before `task` is
used. Fix: `let task = self.pending.take().unwrap().clone();`.

Tested by: the `global-is-not-traced` compile test, for a `Global` field in a resource. No test
checks for traced state behind an `std::rc::Rc`, or for a `jsg::Rc` used after it was moved out
of a field; reviewers check those.

### 3. Don't build on C++ JSG

C++ JSG will be removed, so Rust JSG must not depend on it. New code in the C++ shims in
`src/rust/jsg` calls V8 directly (`v8::TryCatch`, `v8::MaybeLocal`, `isolate->ThrowException`) and
leaves decisions to Rust. Some existing shims still use C++ JSG. Those uses are listed in the
allowlist described below, and are to be removed.

Don't add C++ JSG to the shims. This includes `jsg::Lock` (including `Lock::from`),
`JSG_TRY`/`JSG_CATCH`/`tryCatch`, `jsg::check`/`JsExceptionThrown`, `createTunneledException`,
`makeInternalError`, `jsg::throw*`, `jsg::Data`/`jsg::Value`/`JsValue`/`JsRef`/`V8Ref`,
`jsg::Ref`/`jsg::Promise`/`jsg::Function`/`jsg::Object`, `JSG_REQUIRE`/`JSG_ASSERT` and `liftKj`.
`CONSTRUCTS` in `cpp-jsg-allowlist-test.py` has the full list. Using `Wrappable`,
`GcVisitor`, `IsolateBase` or the module registries is fine for now, since Rust JSG still shares
them with C++ JSG. So are thin calls into `src/workerd/io`, and the serialization hooks in
`workerd/jsg/ser.h` (`writeApiObject`, `readApiObject` and `newDataCloneError`), through which
workerd's API objects serialize, since their serializers are written against C++ JSG.

Bad: a shim that wraps a V8 call in `JSG_TRY(js) { ... } JSG_CATCH(error) {
kj::throwException(createTunneledException(...)); }`. Fix: catch with a `v8::TryCatch` in the
shim, return the exception through a `MaybeLocal` out-parameter, and build the `Error` in Rust.

Tested by: `//src/rust/jsg:cpp-jsg-allowlist-test`. It counts these names in each shim file and
compares the counts with `src/rust/jsg/cpp-jsg-allowlist.txt`, which lists the uses that exist
today. A new use fails the test. When you remove a use, lower its count in the allowlist. The
test only checks the files in `CPP_SHIMS` in `src/rust/jsg/BUILD.bazel`, which lists every `.c++`
and `.h` file in `src/rust/jsg`. To check a shim file in another package, export it with
`exports_files` and add it to `CPP_SHIMS`.

### 4. Handles and locks stay in their scope

- A `Local<'s, T>` must not outlive the `Lock<'s>` it came from. A `Lock<'s>`, or anything else
  with the `'s` lifetime, must not be stored, outlive the callback, or be used inside a nested
  callback or a later request.
- A future passed to async I/O, and the callback that runs when it completes, must be
  `'static`, because they run after the current callback has returned.
- Use a `Global` for a handle that Rust holds outside a resource, and a `TracedGlobal` for a
  resource field. Both can be dropped without the lock, and after the isolate is gone.

Tested by: the compile tests in `src/rust/jsg-compile-tests` (`local-*`, `lock-*`, `callback-*`,
`await-io-*`, `global-*`, `traced-global-*`) and the drop tests in
`src/rust/jsg-test/tests/global.rs` and `traced_global_lifetime.rs`.

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
    optional_nullable_child: jsg::NullOrUndefined<jsg::Rc<OtherResource>>,

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
let js_obj = resource.to_js(&mut lock)?;

// Convert from JS back to a Ref
let r: jsg::Rc<MyResource> = jsg::Rc::from_js(&mut lock, js_val)?;
```

### Reaching the resource from a callback

A method's `&self` is only valid during the call. Code that runs later, such as a promise
reaction or a function the method creates, needs its own reference to the resource. For that,
write `this: jsg::Rc<Self>` as the first parameter instead of `&self`. It is a `jsg::Rc` of the
resource the method was called on, like `JSG_THIS` in C++ JSG. Clone it into whatever runs
later.

```rust
#[jsg_resource]
struct Task {
    scheduled: Cell<bool>,
}

#[jsg_resource]
impl Task {
    /// Runs the task later: `scheduler` keeps a `jsg::Rc` of it until then.
    #[jsg_method]
    fn schedule(this: jsg::Rc<Self>, scheduler: jsg::Rc<Scheduler>) {
        // `this` dereferences to the resource, like `&self`.
        this.scheduled.set(true);
        scheduler.tasks.borrow_mut().push(this);
    }
}
```

Whatever holds the `jsg::Rc` must visit it in its `Traced::trace`, just as a resource visits
its `jsg::Rc` fields. When it later moves the `jsg::Rc` out of that field, for example to run
the task, it must clone it before use.

Don't give the callback the resource's state through an `std::rc::Rc` instead. Both rules are
explained in [Safety rules](#safety-rules), rule 2.

The callback's `jsg::Rc` keeps the resource alive, even after JavaScript drops the resource.
Cycles are fine if they go through a `TracedGlobal`: if the resource holds the callback in a
`TracedGlobal`, GC collects both once JavaScript drops them. A cycle through a
`jsg::v8::Global` is never collected. If the callback shouldn't keep the resource alive, give
it a `jsg::Weak<Self>` (`this.downgrade()`) instead.

Calling the method on something that isn't a resource of this type throws a `TypeError`, as it
does for `&self` methods. A method that takes `this` has no Fast API callback.

### GC Behavior

- **No JS wrapper**: Dropping the last `Rc` immediately destroys the resource (no GC needed).
- **With JS wrapper**: Dropping all `Rc`s makes the wrapper eligible for V8 GC. When collected, the resource is destroyed.
- **Tracing**: The `#[jsg_resource]` macro auto-generates `Traced::trace` and calls it on every field:

| Field type | Traced? | Notes |
|---|---|---|
| `jsg::Rc<T>` | Yes — strong edge | Keeps target alive through GC |
| `Option<jsg::Rc<T>>` | Yes — when `Some` | |
| `jsg::Nullable<jsg::Rc<T>>` | Yes — when `Some` | |
| `jsg::NullOrUndefined<jsg::Rc<T>>` | Yes — when `Some` | |
| `jsg::Lenient<jsg::Rc<T>>` | Yes — when `Some` or `Unconvertable` | |
| `Cell<jsg::Rc<T>>` | Yes — strong edge | Use `Cell` when field needs interior mutability |
| `Cell<Option<jsg::Rc<T>>>` | Yes — when `Some` | |
| `Cell<jsg::Nullable<jsg::Rc<T>>>` | Yes — when `Some` | |
| `Cell<jsg::NullOrUndefined<jsg::Rc<T>>>` | Yes — when `Some` | |
| `Vec<jsg::Rc<T>>` | Yes — each element | Iterates and visits every `Rc` in the vec |
| `HashMap<K, jsg::Rc<T>>` | Yes — each value | Iterates `.values()` and visits each `Rc` |
| `BTreeMap<K, jsg::Rc<T>>` | Yes — each value | Iterates `.values()` and visits each `Rc` |
| `jsg::Dict<jsg::Rc<T>>` | Yes — each value | Visits the value of every entry |
| `HashSet<jsg::Rc<T>>` | Yes — each element | Iterates and visits every `Rc` |
| `BTreeSet<jsg::Rc<T>>` | Yes — each element | Iterates and visits every `Rc` |
| `Cell<Vec<jsg::Rc<T>>>` | Yes — each element | `Cell` variant of above |
| `Cell<HashMap<K, jsg::Rc<T>>>` | Yes — each value | `Cell` variant of above |
| Same patterns with `jsg::v8::TracedGlobal<T>` | Yes | All collection forms work with `TracedGlobal<T>` too |
| `jsg::v8::TracedGlobal<T>` | Yes — dual strong/traced | Enables cycle collection; see below |
| `Option<jsg::v8::TracedGlobal<T>>` | Yes — when `Some` | |
| `jsg::Nullable<jsg::v8::TracedGlobal<T>>` | Yes — when `Some` | |
| `jsg::NullOrUndefined<jsg::v8::TracedGlobal<T>>` | Yes — when `Some` | |
| `Cell<jsg::v8::TracedGlobal<T>>` | Yes — dual strong/traced | Required when set after construction |
| `Cell<Option<jsg::v8::TracedGlobal<T>>>` | Yes — when `Some` | |
| `jsg::Weak<T>` | No | Doesn't keep target alive |
| `jsg::v8::Global<T>` | Compile error — no `Traced` impl | Use `TracedGlobal<T>` in resource fields |
| `std::rc::Rc<T>` | Depends on `T` | Only for plain data; never around a `TracedGlobal` or `jsg::Rc` (Safety rules, rule 2) |
| Any `T: Traced` | Depends on `T` | `#[jsg_resource]` calls `Traced::trace` on every field |

- **`Cell<T>` for interior mutability**: `Traced::trace` takes `&self`. Fields that need to be mutated after construction (e.g. a callback set in a method) can use `Cell<T>`. `Cell<T>` implements `Traced` by reading through `as_ptr()` during single-threaded GC tracing.
- **`Traced` drives field tracing**: `#[jsg_resource]` now traces every named field via `Traced::trace(&self.field, visitor)`. Types with no GC edges use no-op `Traced` impls; wrappers/collections delegate recursively.
- **Nested wrappers are supported by composition**: `Option<Vec<jsg::Rc<T>>>` works as long as each layer implements `Traced`.
- **`#[jsg_resource(custom_trace)]`**: suppresses the generated `Traced` impl so you can write your own. `GarbageCollected` (`memory_name`), `Type`, `ToJS`, and `FromJS` are still generated.
- **`jsg::v8::TracedGlobal<T>` cycle collection**: Uses the same strong↔traced dual-mode as C++ `jsg::V8Ref<T>`. While the parent resource has strong Rust refs the JS handle stays strong. Once all Rust `Rc`s are dropped, `visit_traced_global` downgrades the handle to a `v8::TracedReference` that cppgc can follow — allowing cycles (e.g. a resource holding a callback that captures its own wrapper) to be detected and collected.
- **Circular references** through `jsg::Rc<T>` are **not** collected, matching C++ `jsg::Rc<T>` behavior.

### Creating resources from C++

C++ code that creates a Rust resource, for example to pass it to JavaScript as a handler argument,
calls an `extern "Rust"` function in the crate that defines the resource. The function returns the
resource's wrapper through `jsg::v8::run_ffi_callback`, which enters the caller's scope. If the
closure fails or panics, it returns an empty handle with an exception pending, which C++
propagates:

```rust
#[cxx::bridge(namespace = "workerd::rust::my_api")]
mod ffi {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        type Isolate = jsg::v8::ffi::Isolate;
        type Local = jsg::v8::ffi::Local;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    extern "Rust" {
        unsafe fn my_resource_new(isolate: *mut Isolate, label: &str) -> MaybeLocal;
        unsafe fn is_my_resource(isolate: *mut Isolate, value: Local) -> KjMaybe<bool>;
    }
}

use kj_rs::KjMaybe;

/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open.
unsafe fn my_resource_new(isolate: *mut ffi::Isolate, label: &str) -> ffi::MaybeLocal {
    use jsg::ToJS as _;
    // SAFETY: forwarded from the caller.
    unsafe {
        jsg::v8::run_ffi_callback(isolate, |lock| {
            let resource = jsg::Rc::new(MyResource { label: label.to_owned() });
            resource.to_js(lock)
        })
    }
}

/// # Safety
/// As for `my_resource_new`, and `value` is a handle in the caller's scope.
unsafe fn is_my_resource(isolate: *mut ffi::Isolate, value: ffi::Local) -> KjMaybe<bool> {
    // SAFETY: forwarded from the caller.
    unsafe { jsg::resource::ffi_is::<MyResource>(isolate, value) }
}
```

- The wrapper owns the resource. C++ keeps it alive by holding the wrapper, in a `jsg::JsRef` (or a
  `v8::Global`). C++ has no typed reference to a Rust resource, such as a `jsg::Ref<T>`, so a C++
  JSG signature that takes one takes a JavaScript value, and C++ checks its type with a function
  like `is_my_resource`.
- `jsg::resource::ffi_is::<R>` returns `kj::none`, as `v8::Maybe<bool>` returns `Nothing`, when an
  exception is pending: the check panicked, leaving an internal error pending and execution
  terminating. C++ propagates it as `jsg::JsExceptionThrown`; throwing a `TypeError` for a
  mismatch instead would replace the termination:

  ```c++
  KJ_IF_SOME(isMine, rust::my_api::is_my_resource(isolate, value)) {
    JSG_REQUIRE(isMine, TypeError, "expected a MyResource");
  } else {
    throw jsg::JsExceptionThrown();
  }
  ```

- `jsg::resource::is::<R>` creates no `jsg::Rc`, so use it rather than a conversion to `jsg::Rc`
  when only the type matters.
- The resource's template comes from the isolate's Rust `Realm`, which `Worker::Isolate` creates
  for every worker isolate.

Tested by `src/rust/jsg-test/tests/resource_from_cpp.rs`, through a C++ helper that holds the
wrapper only in a `v8::Global` across a full GC.

## V8 Handle Types

### `Lock<'s>` and handle scopes

`Lock<'s>` stands for "the isolate is locked and the `HandleScope` `'s` is the innermost open
one". Locks are only handed out as `&'s mut Lock<'s>`, borrowed from the pinned storage of
a `jsg::CallbackScope`, so no handle created through one (`Local<'s, _>`) can outlive the
scope. There is no public constructor; locks come from:

- `#[jsg_method]` / `#[jsg_constructor]` callbacks, one lock per callback. They are built
  with `jsg::function_callback`, which turns a closure that captures nothing into a V8
  function callback without any `unsafe`, as `rusty_v8` does; `#[jsg_property(instance)]`
  accessors likewise with `jsg::getter_callback` / `jsg::setter_callback`, and
  `#[jsg_property(lazy_instance)]` accessors with `jsg::getter_callback`,
- `jsg::modules::add_builtin`, whose callback is a closure that captures nothing, of type
  `for<'s> Fn(&mut Lock<'s>) -> Result<Rc<Exports>, jsg::Error>`; registration wraps its
  result in the caller's scope and derives RTTI from the resource type,
- `v8::Function::new`, whose closure captures nothing and gets a lock per call,
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

`T` is a marker type that mirrors the V8 class of the same name: `v8::Value`, `v8::Boolean`,
`v8::Number`, `v8::String`, `v8::Object`, `v8::Function`, `v8::Uint8Array` and so on, all in
`jsg::v8` and nowhere else, so that `v8::String` reads as it does in C++ and never collides with
`std::string::String`. A marker's `Local<T>` methods are named after the methods of its V8 class,
in snake case (`Local<Number>::value()`, `Local<String>::length()`), and a marker has a
constructor where V8 has one (`v8::Number::new(lock, 1.5)`). The markers are defined in `v8.rs`
(promises in `promise.rs`); the comment above `Value` lists the V8 classes that have no marker
yet. Other V8 classes that Rust JSG wraps, such as `v8::BackingStore`, are also only in
`jsg::v8`.

A `Local` is one non-null pointer, laid out like `v8::Local<T>`. It does not record its
isolate; methods that need the isolate take the lock. A handle that may be empty is an
`Option<Local<'s, T>>`, laid out like `v8::MaybeLocal<T>`: for example, the `String`
constructors that take runtime input return `None` for a string longer than
`String::MAX_LENGTH`, which V8 refuses with no exception pending.

A cast to a supertype, such as `Local<Function>` to `Local<Object>` or `Local<Value>`, is a
`From` and costs nothing, whichever supertype it targets. A cast to a subtype is a
`TryFrom`, which checks the value and fails with a `DataError`.

```rust
let str_value = "hello".to_js(lock)?;
let num_value = 42u32.to_js(lock)?;
let global = str_value.to_global(lock);

let name: v8::Local<v8::Name> = v8::Local::<v8::String>::try_from(str_value).unwrap().into();
assert!(v8::Local::<v8::Function>::try_from(str_value).is_err());
```

A method parameter can be a handle of the types that C++ JSG's `v8::Local<T>` parameters
take: `Local<Value>` takes any value, and `Local<String>`, `Local<Object>`,
`Local<Function>`, `Local<BigInt>`, `Local<TypedArray>`, `Local<ArrayBuffer>`,
`Local<ArrayBufferView>` and the typed arrays take a value of exactly that type, with no
coercion, and throw a `TypeError` for any other. `Local<Promise>` takes any value, as a
promise resolved with it.

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

### Functions from Rust closures

`v8::Function::new(lock, data, f)` creates a JavaScript function that calls `f` with `data`,
the counterpart of C++ JSG's `Lock::wrapReturningFunction()`. `f` captures nothing; its state
is `data`, which the function owns through a C++ `Wrappable`, as a resource's wrapper owns the
resource. The garbage collector traces `data` (`T: Traced`) through the function and drops it
once the function is unreachable, so the `TracedGlobal`s and `jsg::Rc`s that `data` traces may
lead back to the function. Two things break this:

- A reference that `T::trace` does not visit, such as a `v8::Global` (`Traced::trace` does
  nothing by default). If it can reach the function, the cycle is invisible to the garbage
  collector, and the function and `data` are freed only when the isolate is torn down.
- Another owner that the garbage collector also traces, such as a resource sharing part of
  `data` through a `std::rc::Rc`. `data` must be the only owner that traces what it traces:
  tracing one `jsg::Rc` from two owners fails an assertion, and a `TracedGlobal` traced from
  two owners can be freed while one of them still uses it. To share state, hold a `jsg::Rc` or
  `jsg::Weak` to the resource that owns it.

```rust
let greet = v8::Function::new(lock, String::from("hello"), |lock, greeting, args| {
    let name = String::from_js(lock, args.get(0))?;
    format!("{greeting}, {name}").to_js(lock)
})?;
```

`f` returns `Result<Local<Value>, jsg::Error>`: an `Err` is thrown, and a panic throws an
internal error and terminates execution, as in a `#[jsg_method]`. While execution is terminating,
or a JavaScript exception is pending, `Function::new` creates nothing and returns a pending error
(see "Errors from JavaScript"), as `call` does; return it to keep the termination or the exception
propagating. As with the functions C++ JSG wraps, the function's `name` is empty, its `length` is
0, and it can be called with `new`.

### Conventions of the `v8` module

The `v8` module takes handles and returns handles. Converting between a Rust value and a
JavaScript value is the job of `FromJS` and `ToJS`, not of a `v8` method.

- A method returns a Rust value only where V8 itself returns a C scalar, or copies memory out
  (`to_vec`, `to_string`). The scalar comes from a typed handle (`value`, `length`, `value_of`,
  `int64_value`, `is_*`, `byte_length`), or from one of V8's own coercions of any value
  (`int32_value`, `uint32_value`, `integer_value`). `Function::call` and `Object::get` return a
  `Local<Value>`; convert it where a Rust value is wanted:

  ```rust
  let result = func.call(lock, None, &[x, y])?;
  let sum = f64::from_js(lock, result)?;
  ```

- A method takes raw Rust data only where V8 takes C data: a constructor's input
  (`String::new_from_str`, `Number::new`, `Integer::new`), an index, a time, and a `&str`
  property key as a convenience (`obj.get(lock, "name")`).
- A method that can run JavaScript, or that needs a context, returns a `Result`, as the
  coercions do; its error is usually pending (see
  [Errors from JavaScript](#errors-from-javascript)). It returns an `Option` only for "absent",
  as `int64_value` does for a `BigInt` that does not fit, or for "V8 refused, with nothing
  pending", as the `String` constructors do for input longer than `String::MAX_LENGTH`. The
  exception is `String::new_from_utf8_literal`, which panics for an overlong literal, as V8's
  `NewFromUtf8Literal` fails a `static_assert`. A method never has an infallible signature over
  a C++ shim that can throw: the shim is declared `Result` in the bridge, or Rust decides before
  calling it, as `String::new_from_utf8` checks the length.
- Naming: `as_*` is free and borrows (`as_slice`, `as_local`); `to_*` costs a copy or an
  allocation (`to_vec`, `to_string`, `to_global`); `value` and `*_value` read a scalar under
  the name V8 gives it (`Local<Number>::value`, `int64_value`, `int32_value`).

## Errors from JavaScript

When a function that `Local<Function>::call` calls throws, the exception stays pending in V8, as with C++ JSG's `JsExceptionThrown`, and `call` returns an error for which `Error::is_pending()` is true. The same goes for these other operations that run JavaScript: converting a value to a string or a number (its `toString`, `valueOf` or `Symbol.toPrimitive`, or a Proxy trap), `Object::get` and `Object::has` (a getter or Proxy trap), `Object::set` (a setter, also one on the prototype chain, or a Proxy trap) and `Array::set` (a setter, also one on the prototype chain), `Array::iterate` and so converting to a `Vec` (an element's getter), and `ser::serialize` (a getter or Proxy trap, or a `DataCloneError`). Building the array when converting a `Vec` to JavaScript runs no JavaScript: like C++ JSG, it defines the elements as data properties (`Array::with_elements`), so no setter on `Array.prototype` runs. Nor does converting a `jsg::Dict` to JavaScript, which defines its entries as data properties. A `#[jsg_struct]` follows C++ JSG's `fast_jsg_struct` flag: with it, the fields are defined as data properties (`Object::with_data_properties`), an absent optional field as `undefined`, and no JavaScript runs; without it, they are set one by one with `Object::set`, an absent optional field is omitted, so a setter on `Object.prototype` runs, and what it throws is passed on. `Error`'s own `FromJS`, which reads `name` and `message`, passes such an error on too. `Lock::throw_exception` throws nothing for that error, so a callback that returns it propagates the original exception to its JavaScript caller. V8 keeps reporting the exception where it was thrown, and DevTools' "pause on uncaught exceptions" still sees whether JavaScript further up catches it. A termination is handled the same way.

Converting a value to JavaScript can fail too, as C++ JSG's `wrap()` can throw, so `ToJS::to_js` returns a `Result`: creating a `Date` or a resource's wrapper fails, leaving the exception pending, when V8 throws or execution is terminating, and a container passes on the error from converting an element, whichever kind it is. A `&str` or `String` longer than `v8::String::MAX_LENGTH` fails with a `RangeError` ("Invalid string length", as V8 reports it), which is not pending; every other conversion of a single value in this crate fails only with a pending error. A `#[jsg_method]`, property getter or wildcard property throws an error from converting its return value as it throws one the method returns, with `Lock::throw_exception`: for a pending error the original exception, or the termination, keeps propagating, and any other error is thrown as a new JavaScript error of its type. The method has already run by then; only its return value is lost.

Code that gets a pending error must not call into JavaScript, and should return the error. Converting it to another error and throwing that replaces the exception, as when C++ code throws while a `JsExceptionThrown` unwinds; nothing ever replaces a termination, and none of these run while execution is terminating: they fail with a pending error without preparing V8 to run JavaScript, which would clear the termination, even for a value that would convert without running JavaScript. A pending error is only meaningful while its exception is pending: one kept past that, such as a clone stored after `try_catch` caught the exception, is thrown as an internal error when V8 has no exception pending.

To handle an exception instead, run the code in `Lock::try_catch`, which catches as C++ `jsg::Lock::tryCatch()` does: the exception, or an error the closure returns, as a value; a termination keeps propagating.

```rust
match lock.try_catch(|lock| {
    let result = callback.call(lock, None, &[])?;
    f64::from_js(lock, result)
})? {
    Ok(result) => { /* ... */ }
    Err(exception) => { /* handle what the callback threw */ }
}
```

## Serialization

`jsg::ser::serialize` and `jsg::ser::deserialize` convert a value to and from bytes in the format
of C++ JSG's `jsg::Serializer` and `jsg::Deserializer`, with their default options: the bytes are
the same, and each reads what the other writes, so the two can share stored data. Native errors
are written as host objects when the `enhanced_error_serialization` flag is on, and workerd API
objects such as `Blob` and `Headers` through their C++ serializers; Rust resources can't be
serialized. A `SharedArrayBuffer` is written as a reference to a buffer that is not part of the
bytes, which neither `ser::deserialize` nor a `jsg::Deserializer` given only the bytes can
resolve, so data containing one can't be read back. Deserialization runs no JavaScript. Transfer
lists, externals and the other options are not supported. The C++ side is `src/rust/jsg/ser.c++`.

## Type Conversions

The module docs of `wrappable.rs` list every Rust type that converts to or from JavaScript, and
how. Most convert both ways. `&[T]` converts only from JavaScript, so it can be a method parameter
but not a return value. A number is an `f64`, which converts like C++ JSG's `double`: to a number,
and from any value with JavaScript's `ToNumber`. An `f32` converts only to JavaScript. A number
that stays on the V8 heap is a `v8::Local<'s, v8::Number>`, which accepts only a number.

Integers convert like C++ JSG's. `u8`, `u16`, `u32`, `i8`, `i16` and `i32` convert to a number,
and from a value converted to a number, which must be in range; whether a fraction just past a
range limit (`255.5` or `-0.5` for `u8`) is truncated before that check, as Web IDL's
`[EnforceRange]` does, or is out of range depends on the `JSG_INTEGER_ENFORCE_RANGE` autogate (see
the module docs of `wrappable.rs`). `i64` and `u64` convert to a `BigInt`, like `int64_t` and
`uint64_t`, and from a `BigInt` that fits, or any other value converted to a number, which must be
finite and in range (the autogate does not apply), and truncates toward zero. An integer that stays
on the V8 heap is a `v8::Local<'s, v8::Int32>`, `v8::Uint32` or `v8::Integer` (either), which
accepts only an integral number in that range, as `v8::Local<v8::Number>` accepts only a number.
`Local<Value>::int32_value`, `uint32_value` and `integer_value` convert any value with
JavaScript's `ToInt32`, `ToUint32` and `ToInteger`, as the V8 methods of those names do.

Like C++ JSG's `kj::Date`, `SystemTime` converts to a `Date` of the milliseconds since the epoch,
truncated toward zero, and from a `Date` or a number of milliseconds within the range of `kj::Date`.

`jsg::Dict<V>` is C++ JSG's `jsg::Dict<V>`, Web IDL's `record<DOMString, V>`: a plain object used
as a map from strings to values. It holds its entries in order, in a `Vec<(String, V)>`. From
JavaScript, it takes any object other than an array, and reads its own enumerable properties
that are not symbols, as `Object.keys()` lists them; a value that is not a `V` is a `TypeError`
naming its key. To JavaScript, it is a new plain object with each entry as an own data property
(`Object::with_data_properties`), as Web IDL defines a record. C++ JSG's `DictWrapper` sets the
entries instead, so with it a setter on `Object.prototype` runs and a `__proto__` entry replaces
the prototype or is dropped; with Rust JSG, no JavaScript runs and `__proto__` is an own property,
as with `Object.fromEntries()`. `Local<Object>::keys()` and
`get_by_name()` list and read properties the same way.

### Arrays, typed arrays and `ArrayBuffer`s

A `Vec<T>` converts to a JavaScript `Array` of the converted elements, numbers included:
`vec![1u8, 2, 3].to_js(lock)` makes `[1, 2, 3]`, and a `Vec<i64>` makes an `Array` of `BigInt`s.
A typed array or an `ArrayBuffer` is an explicit conversion of a slice:

```rust
use jsg::ToJS;
use jsg::ToJsArrayBuffer;
use jsg::ToJsTypedArray;

let bytes: Vec<u8> = encode(input);
let typed: v8::Local<v8::Uint8Array> = bytes.to_js_typed_array(lock); // Uint8Array
let buffer: v8::Local<v8::ArrayBuffer> = bytes.to_js_array_buffer(lock); // ArrayBuffer
let array: v8::Local<v8::Value> = bytes.to_js(lock)?; // Array of numbers
```

`to_js_typed_array` makes the typed array of the element type, from `Uint8Array` for `[u8]` to
`BigUint64Array` for `[u64]`. `to_js_array_buffer` makes an `ArrayBuffer` holding the bytes, which
is what C++ JSG returns for a `kj::Array<kj::byte>`; only `[u8]` has it. A method returns the
handle, and so takes `lock: &mut Lock<'s>` and names the scope:

```rust
#[jsg_method]
pub fn digest<'s>(
    &self,
    lock: &mut jsg::Lock<'s>,
    data: v8::Local<'s, v8::Uint8Array>,
) -> v8::Local<'s, v8::Uint8Array> {
    hash(data.as_slice(lock)).to_js_typed_array(lock)
}
```

From JavaScript, a `Vec<T>` or `&[T]` parameter reads an `Array`. A typed array parameter is a
handle, whose `as_slice(lock)` reads its memory in place; copy it out with `to_vec()` if the
method keeps it. An `ArrayBuffer` parameter is a `v8::Local<'s, v8::ArrayBuffer>`, and a
`v8::Local<'s, v8::ArrayBufferView>` accepts any typed array or `DataView`. To return an
`ArrayBuffer` of bytes computed in Rust, call `to_js_array_buffer(lock)` on them; to fill one in
place, allocate it with `ArrayBuffer::new_zeroed_with` and view it with `Uint8Array::from_buffer`.

To port a C++ JSG signature:

| C++ JSG | Rust JSG |
| --- | --- |
| `kj::Array<kj::byte>` returned | `v8::Local<'s, v8::ArrayBuffer>`, from `to_js_array_buffer` |
| `kj::Array<kj::byte>` parameter | `v8::Local<'s, v8::ArrayBufferView>`; C++ JSG also accepts a bare `ArrayBuffer`, for which take a `v8::Local<'s, v8::Value>` and `try_into()` each |
| `jsg::BufferSource` | `v8::Local<'s, v8::Value>`; check `is_array_buffer()` / `is_array_buffer_view()` and `try_into()` each |
| `v8::Local<v8::Uint8Array>` returned | `v8::Local<'s, v8::Uint8Array>`, from `to_js_typed_array` |
| `kj::Array<T>` of other numbers returned | `Vec<T>`, an `Array` |
| `kj::Array<T>` of integers parameter | `Vec<T>` |
| `kj::Array<double>` parameter | `Vec<f64>` |
| `kj::Array<bool>` | `Vec<bool>` |
| `kj::Array<T>` of anything else | `Vec<T>` |

A `#[jsg_struct]` field converts with its `ToJS` impl like any other value, so a `Vec<u8>` field
becomes an `Array` of numbers. A field has no way to choose a typed array or an `ArrayBuffer`, so
a `JSG_STRUCT` with a `kj::Array<kj::byte>` field has no direct port.

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

A union converts a value as the equivalent `kj::OneOf<>` does in C++ JSG, following
Web IDL's union conversion. It tries its variants in passes, in declaration order
within each pass, and takes the first that converts:

1. resource types (`jsg::Rc<R>`);
2. every type that is not a string, numeric or boolean type, such as resources, arrays,
   typed arrays and structs;
3. boolean types, if the value is a boolean;
4. numeric types, if the value is a number or a `BigInt`;
5. string types;
6. numeric types;
7. boolean types.

So a value is coerced only when no variant takes it as it is, and coerced to a string
in preference to a number or boolean: `StringOrNumber` takes `true` as `"true"`, and
`null` as `"null"`. An error from a variant's conversion, such as a `Symbol` that
cannot become a string, or an exception thrown by `toString()`, is thrown rather than
trying another variant. A `NonCoercible<T>` variant, where `T` is `String`, `bool`,
`f64` or an integer type, takes part in `T`'s passes but converts only a value of
exactly that type. A variant that is itself a union takes part
in each pass with its own variants, as a nested `kj::OneOf` does.

Variants can be any type that converts from JavaScript and that a `kj::OneOf` can hold:
strings, numbers, the integer types, booleans, `SystemTime`, resources, structs,
`Vec<T>`, `Dict<V>`, `Nullable<T>`, `NonCoercible<T>`, other unions, and
handles (`v8::Local<'s, T>`). Each is tried in the passes of its C++ counterpart, so a few
behave as they do in C++ rather than as their position suggests:

- `SystemTime`, like `kj::Date`, is not a numeric type, so it is tried in pass 2, where it
  also takes a number of milliseconds, before any numeric variant does.
- `Nullable<T>`, like `kj::Maybe<T>`, is tried in pass 2 whatever `T` is: it takes `null`
  and `undefined`, and converts any other value as `T` would, coercion included. A value
  that `T` does not take is not taken as `null`, as in C++ JSG with the
  `dont_substitute_null_on_type_error` compatibility flag, which Rust JSG always follows.
- A handle takes only a value of its type, as C++ JSG's `v8::Local<T>` does:
  `v8::Local<'s, v8::String>` is a string type, but never coerces, and
  `v8::Local<'s, v8::BigInt>` is a numeric type that takes only a `BigInt`.
  `v8::Local<'s, v8::Value>` takes any value, in pass 2.

A union that holds handles takes their scope as its one lifetime parameter, and can't
outlive it:

```rust
#[jsg_oneof]
enum ObjectOrString<'s> {
    Object(jsg::v8::Local<'s, jsg::v8::Object>),
    String(String),
}
```

Like C++ JSG's `webidl::UnionTypeValidator` (`src/workerd/jsg/web-idl.h`), `#[jsg_oneof]`
rejects at compile time a union that breaks Web IDL's rules for unions, counting the
variants of nested unions and the inner types of nullable types: at most one boolean,
numeric, string, dictionary-like (struct or `Dict<V>`) and sequence-like type; at most one
nullable or struct type in all; an object handle (`v8::Local<'s, v8::Object>`) only
without resources, dictionary-like or sequence-like types; no `Option<T>` or
`Lenient<T>`, which mark optional parameters and fields; and no type twice, counting a
`Nullable<T>` as `T` and the variants of a nested union as the union's own. A union may
have several resource types, as Web IDL allows, but not the same one in both a union and
a union nested in it.

The union traces the variant it holds, so a union with a `jsg::Rc<R>` variant can be a
resource field; one that holds handles can't. A union implements `jsg::ToJS` when every
variant does.

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

Autogates (`src/workerd/util/autogate.h`) are process-wide, not per harness.
`jsg_test::set_autogates(&["jsg-integer-enforce-range"])` configures exactly the named gates
(kebab-case, without the `workerd-autogate-` prefix) for as long as the returned guard lives, and
an empty slice disables them all, whatever `WORKERD_ALL_AUTOGATES` says. Dropping the guard
deinitializes the configuration, so the gates follow `WORKERD_ALL_AUTOGATES` again. The tests run
serially, so a test holds the guard for its duration; the guards do not nest.

Use `Harness::with_fast_api_and_flags()` to combine compatibility flags with V8 Fast API callbacks.

The harness sets raw flag values; it does not apply a compatibility date or resolve flag
implications. It uses the same `configureJsgIsolate()` helper as workers to apply isolate-level
JSG settings, including template behavior, Node.js compatibility, and module settings.
It does not reproduce worker-specific initialization such as logging, inspectors, or limits.

### How it works

1. During worker initialization, C++ canonicalizes the worker's `CompatibilityFlags` via `capnp::canonicalize()` and passes the bytes to `realm_create()`, which parses them once and stores the result in the per-context `Realm`.
2. `lock.feature_flags()` reads the cached `FeatureFlags` and returns its capnp reader. No copies or re-parsing on access.

### Compatibility-dependent API surfaces

`#[jsg_resource(custom_members)]` generates member factories while leaving
`Resource::members(registry, flags)` to the resource author. Use ordinary Rust conditionals to select
members and choose property placement when the template is built. RTTI export runs the same function
with the export's flags, so TypeScript declarations follow the same choices. See [custom member registration](../jsg-macros/README.md#custom-member-registration)
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

Three macros expose properties on resource types: `#[jsg_property]` for prototype accessors,
instance data properties and lazily computed instance data properties, `#[jsg_inspect_property]`
for debug-only symbol-keyed properties, and `#[jsg_wildcard_property]` for properties of any name
(see [Wildcard properties](#wildcard-properties)).

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
    pub fn get_value(&self) -> u32 { /* ... */ }

    #[jsg_property(prototype)]
    pub fn set_value(&self, v: u32) { /* ... */ }

    // Read-only prototype property (no paired `set_value`)
    #[jsg_property(prototype, readonly)]
    pub fn get_label(&self) -> String { /* ... */ }

    // Own data property (use sparingly; inhibits minor-GC)
    #[jsg_property(instance)]
    pub fn get_id(&self) -> String { /* ... */ }

    // Own data property computed on first access, then stored (use sparingly, as above)
    #[jsg_property(lazy_instance)]
    pub fn get_details(&self) -> Vec<String> { /* ... */ }

    // Inspect-only: shown by util.inspect() / console.log, invisible to JS
    #[jsg_inspect_property]
    pub fn debug_info(&self) -> String { /* ... */ }
}
```

Methods **must** start with `get_` (getter) or `set_` (setter). The prefix is stripped for the
JS name. Omitting the setter (or adding `readonly`) makes a `prototype` or `instance` property
read-only. A `lazy_instance` property has no setter and is writable unless marked `readonly`.

### Wildcard properties

`#[jsg_wildcard_property]`, the equivalent of C++ JSG's `JSG_WILDCARD_PROPERTY`, answers reads
of string-named properties that neither the object nor its prototypes have:

```rust
#[jsg_resource]
impl Headers {
    // `None` if there is no such property.
    #[jsg_wildcard_property]
    pub fn get_named(&self, name: String) -> Option<String> { /* ... */ }
}
```

A wildcard property behaves as in C++ JSG:

- The method takes `&self`, optionally `lock: &mut jsg::Lock`, and the name as a `String` or
  `&str`, and returns `Option<T>` or `Result<Option<T>, E>`. A type has at most one, though
  `custom_members` may choose between several by compatibility flag.
- Properties of the object and its prototypes, such as methods and `toString`, take precedence;
  the method is called only for names that neither has.
- Only string names reach it. A symbol, or an integer key such as `obj[1]`, is looked up as
  usual.
- Besides reads, `name in obj`, `Reflect.has()`, `propertyIsEnumerable()` and assignment ask
  V8 whether the property exists, which calls the method too. So the method must not have side
  effects, and an error it returns is thrown from those as well.
- A read throws an error from converting the value with `ToJS` as it throws one the method
  returns. Only reads convert the value; `name in obj` and the other existence checks don't.
- Assignment and `Object.defineProperty()` define an own property, which then takes precedence.
- Wildcard properties are not own properties: `hasOwnProperty()` is false,
  `Object.getOwnPropertyDescriptor()` is undefined, `delete` has no effect on them, and
  `Object.keys()`, `for...in` and spreading omit them. `propertyIsEnumerable()` is true for them
  all the same.
- An object that inherits from the resource, as `Object.create(obj)` does, reads them too.
- A wildcard property named `then` makes the object a thenable, which `await` would call.
  Return `None` for `then` unless that is intended.

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
An integer argument reaches the callback as a `double` and converts with the slow path's checks,
following the `JSG_INTEGER_ENFORCE_RANGE` autogate as the slow path does; an integer of up to 32
bits returns as an `int32_t` or a `uint32_t`, while a method that returns an `i64` or a `u64` has
no fast callback.

## FFI Functions with Raw Pointers

Functions exposed to C++ via FFI that receive raw pointers must be marked `unsafe fn`:

```rust
pub unsafe fn realm_create(isolate: *mut v8::ffi::Isolate, feature_flags_data: &[u8]) -> Box<Realm> {
    // ...
}
```
