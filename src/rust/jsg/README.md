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
- `v8::Promise` describes `Promise<any>`, `v8::VoidPromise` describes
  `Promise<void>` and `jsg::Promise<T>` describes `Promise<T>`; as in C++, a
  promise parameter also accepts a value that is not a promise.
- A `jsg_cpp::Ref<T>` refers to the C++ class's own description by the name
  `CppResource::fully_qualified_name()` gives, so the class must be in a C++
  type group of the export.
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
"node-internal:dns", |lock| Ok(DnsUtil::new(lock)), ModuleType::Internal)`. The return
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

Strong reference to a Rust resource managed by GC. Derefs to `&R`. Cloning and dropping a `Rc` tracks strong references for the garbage collector. `Rc::new` takes a `Lock`: the resource belongs to that lock's isolate, and wrapping it in another panics. See [Dropping references](#dropping-references) for drops without the isolate lock.

### `Weak<R>`

Weak reference that doesn't prevent GC collection. Use `upgrade()` to get a `Rc<R>` if the resource is still alive. It is dropped like an `Rc`.

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

### 1. Don't enter V8 while an exception or a termination is pending

When JavaScript that Rust called throws, V8 leaves the exception pending. Rust returns, and the
exception propagates to the JavaScript that catches it. When V8 terminates a script (for example
because the Worker ran out of CPU time), it throws a special termination exception, which
JavaScript cannot catch. It must reach the C++ code that started the script.

Two things break this:

- Calling a V8 API that takes a `v8::Local<v8::Context>` while an exception or a termination is
  pending: a call, a property read or write, a coercion, `Array::Iterate`,
  `ObjectTemplate::NewInstance`, `Function::New`, `FunctionTemplate::GetFunction`, `Date::New`,
  `ArrayBuffer::Detach` (which uses the current context itself) and the promise APIs. Some of
  these APIs clear the pending exception on entry, a termination included, so it is lost and
  JavaScript runs on as if nothing had happened. Others fail a V8 debug check, and in a release
  build run on with the exception still pending.
- Throwing while execution is terminating. `v8::Isolate::ThrowException` replaces the pending
  termination with the new exception, which JavaScript can catch. The Worker then keeps running.

Creating strings, numbers, BigInts, symbols, plain objects and arrays, typed arrays, error objects
and templates takes no context. It is fine at any time, also while execution is terminating. A
`DOMException` is not an error object in this sense: creating one instantiates a template in the
current context.

How the code keeps the rule:

- Every C++ shim that passes a context to V8 first checks
  `if (exceptionPending(isolate)) return ...;` (`ffi.c++`). While an exception or a termination
  is pending, it calls nothing in V8 and fails, leaving the exception pending. Rust turns that
  into an error for which `Error::is_pending()` is true, as it does when the call itself throws
  (see [Errors from JavaScript](#errors-from-javascript)). So while an exception is pending,
  every operation that would pass a context to V8 fails, even one that wouldn't run JavaScript
  for this value, such as converting a string primitive or wrapping a resource that already has
  a wrapper. A new shim follows the same pattern, or says in a comment why it doesn't need to
  (`local_type_of`).
- "Pending" includes an exception that the innermost `v8::TryCatch` has caught but not yet
  reset, as `v8::Isolate::HasPendingException()` reports it. Inside `Lock::try_catch`, once
  JavaScript has thrown, the closure should return the error rather than go on. C++ code that
  catches with its own `v8::TryCatch` resets it before calling into Rust JSG or running
  JavaScript that might. The `custom-raw-v8-trycatch` clang-tidy check flags every raw
  `v8::TryCatch`; each one has a `NOLINT` comment that says why it is safe. With debug
  assertions, a Rust JSG callback that V8 enters while an exception is pending panics.
- The `isolate_throw_*` shims throw nothing while execution is terminating, so
  `Lock::throw_exception`, `Lock::throw_error`, `Lock::throw_internal_error` and the fast API
  error path are safe. A shim that calls `isolate->ThrowException` itself must check first.
- Don't discard a `jsg::Error` from an operation that can fail with a pending error
  (`Function::call`, `Object::get`, `Object::set`, a coercion, `ToJS::to_js`). It may be the
  termination, or a JavaScript exception that is still pending (see
  [Errors from JavaScript](#errors-from-javascript)). Pass it on with `?` instead of using
  `.ok()`, `unwrap_or`, or `map_err` to a different error. To handle the exception, use
  `Lock::try_catch`.
- To terminate execution from Rust, as C++ `jsg::Lock::terminateExecutionNow()` does, return the
  error `Lock::terminate_execution_now` returns. It delivers the termination before returning,
  so no more JavaScript runs; `Lock::terminate_execution` only requests one.
- An empty result from an FFI shim doesn't always mean termination. It can also mean a stack
  overflow or an ordinary exception. Check `IsExecutionTerminating()`, or catch the exception
  with a `v8::TryCatch` in the shim and decide with `catch_exception()` (`ffi.h`).

Bad: `let name = obj.get(lock, "name").ok();`. If the script is being terminated, the termination
is lost. Fix: `let name = obj.get(lock, "name")?;`.

Tested by: `src/rust/jsg-test/tests/termination.rs` and `pending_exception.rs`, the
termination and pending-exception tests in `src/rust/jsg-test/tests/coercion_safety.rs`,
`function_new.rs` and `resource_callback.rs`, and the termination test in `promise.rs`.

### 2. Keep traced state with its one GC owner

Traced state is a `TracedGlobal`, a `jsg::Rc`, or a type that holds them. It belongs to the one
GC owner that traces it: a resource, or a `Wrappable` such as the data of a function created with
`Function::new` or the value of a typed promise. A typed promise's reaction takes its value out
of the wrapper detached, as a moved C++ `jsg::Ref` is: every `jsg::Rc` and `TracedGlobal` in it is
strong again, with no owner, so the reaction can keep it or put it in another owner (see
`GcVisitor::is_detaching`; `GcVisitor::as_ffi` returns `None` for such a visit, which a `Traced`
impl that visits through it must handle). The `jsg::` cells below detach a value the same way.

- Never put traced state inside an `std::rc::Rc`. A clone of the `Rc` held by a second GC owner
  would trace the same state again, so `std::rc::Rc` is `Traced` only for types that hold no GC
  references (`jsg::HoldsNoGcReferences`). For a `jsg::Rc`, C++ asserts a single owner
  (`Wrappable::visitRef` in `src/workerd/jsg/wrappable.c++`), so the process aborts. For a
  `TracedGlobal`, nothing asserts, but whichever owner traces it last decides whether its handle
  is strong or weak. The value can then be collected while the other owner still uses it. To give
  a callback or another party access to the state, give it a `jsg::Rc` or `jsg::Weak` to the
  resource that owns the state. A method gets one with a `this: jsg::Rc<Self>` parameter; see
  [Reaching the resource from a callback](#reaching-the-resource-from-a-callback).
- Keep a `jsg::Rc` or a `TracedGlobal` that a resource moves out of a field in a `jsg::Cell` or
  a `jsg::RefCell`, not in a `std::cell::Cell` or `RefCell`. Once GC has visited a `jsg::Rc` in
  a field, the `jsg::Rc` gives up its strong reference and relies on the field's owner to trace
  it; a `TracedGlobal` makes its handle weak the same way. Moved out of the field by a bitwise
  move, the reference is neither strong nor traced, so GC can collect the resource's JavaScript
  wrapper, or the `TracedGlobal`'s value, while Rust still holds it. The resource then gets a
  new wrapper the next time it reaches JavaScript, and nothing traces the `TracedGlobal`s it
  holds, so they can lose their values. C++ `jsg::Ref` doesn't have this problem because its
  move constructor makes the reference strong again, but Rust has no hook on moves.
  `jsg::Cell::take`, `jsg::Cell::replace` and `jsg::RefCell::borrow_mut` detach everything in
  the cell before it can leave, which makes it strong; see [Cells](#cells). A resource is
  reached only through `&self`, so a cell is the only way to move a reference out of a field,
  and the std cells are `Traced` only for types that hold no GC references
  (`jsg::HoldsNoGcReferences`). The exception is a resource's `Drop` impl, which has
  `&mut self`: a reference it moves out of a field and keeps must be cloned or dropped there.
  See [#7976](https://github.com/cloudflare/workerd/issues/7976).
- Don't put a `jsg::v8::Global` in anything GC traces. A `Global` is always strong, so if it
  points back at its owner (for example function data that holds the function itself), neither
  is ever collected. Use a `TracedGlobal`. The compiler rejects a `Global` field in a resource,
  but not inside a type with a hand-written `impl Traced`, so check those by hand.

Bad: two `Function::new` callbacks whose data are clones of one `std::rc::Rc<State>`, where
`State` holds a `TracedGlobal`, which doesn't compile. Fix: a resource holds `State`, and each
callback holds a `jsg::Rc` or `jsg::Weak` to that resource.

Bad: `pending: Cell<Option<jsg::Rc<Task>>>` or
`callback: Cell<Option<TracedGlobal<Value>>>`, which don't compile. Fix:
`pending: jsg::Cell<Option<jsg::Rc<Task>>>`, taken out with `self.pending.take(lock)`.

Tested by: the `global-is-not-traced` compile test, for a `Global` field in a resource, and the
`gc-reference-in-std-cell-is-not-traced` and `gc-reference-in-std-rc-is-not-traced` compile
tests, for a `jsg::Rc` or a `TracedGlobal` in a std cell or a std `Rc`. No test checks a
hand-written `impl Traced` that shares traced state some other way or reaches it through
another kind of cell, a std cell included, or a hand-written `impl HoldsNoGcReferences` on a type
whose `trace` visits something; reviewers check those.

### 3. Don't build on C++ JSG

C++ JSG will be removed, so Rust JSG must not depend on it. New code in the C++ shims in
`src/rust/jsg` calls V8 directly (`v8::TryCatch`, `v8::MaybeLocal`, `isolate->ThrowException`) and
leaves decisions to Rust. Some existing shims still use C++ JSG. Those uses are listed in the
allowlist described below, and are to be removed. The one exception is in
`src/workerd/io/io-context/ffi.c++`, where `io_context_await_io_and_run()` throws
`JsExceptionThrown` to pass an exception that Rust left pending to `IoContext::run()`, which
learns of exceptions no other way, and two shims call the C++ functions that `ctx.waitUntil()` and
`ctx.abort()` call, so that a Rust implementation matches C++ exactly; its "C++ JSG" section
explains why that is sound. Don't extend it.

Don't add C++ JSG to the shims. This includes `jsg::Lock` (including `Lock::from` and
`IoContext::getCurrentLock()`),
`JSG_TRY`/`JSG_CATCH`/`tryCatch`, `jsg::check`/`JsExceptionThrown`, `createTunneledException`,
`makeInternalError`, `jsg::throw*`, `jsg::Data`/`jsg::Value`/`JsValue`/`JsRef`/`V8Ref`,
`jsg::Ref`/`jsg::Promise`/`jsg::Function`/`jsg::Object`, `JSG_REQUIRE`/`JSG_ASSERT` and `liftKj`.
`CONSTRUCTS` in `cpp-jsg-allowlist-test.py` has the full list. Using `Wrappable`,
`GcVisitor`, `IsolateBase` or the module registries is fine for now, since Rust JSG still shares
them with C++ JSG. So are thin calls into `src/workerd/io`, and the serialization hooks in
`workerd/jsg/ser.h` (`writeApiObject`, `readApiObject` and `newDataCloneError`), through which
workerd's API objects serialize, since their serializers are written against C++ JSG, and
`newDOMException` in `workerd/jsg/dom-exception.h`, through which Rust creates DOMExceptions
without depending on how DOMException is implemented. These hooks take and return only V8 and KJ types.

The one exception is `src/rust/jsg-cpp`, the interop between Rust JSG and C++ JSG resources,
which exists because C++ APIs and Rust APIs have to take, return and hold each other's resources
until C++ JSG is gone. Its shims (`jsg-cpp.h` and `ffi.c++`) form the interop tier of the
allowlist, and may use `jsg::Lock`, `jsg::Ref`/`jsg::Optional`/`jsg::Object`, `jsg::V8Ref`,
`JsExceptionThrown` and `jsg::throwInternalError`: what holding, wrapping and unwrapping a C++
resource through the isolate's `TypeHandler`s and turning a C++ exception into a pending
JavaScript one take. Even there, `JSG_TRY`/`tryCatch`, `createTunneledException`,
`makeInternalError`, `jsg::check`, `liftKj`, `JSG_REQUIRE`, the other `jsg::throw*` functions
(`throwTunneledException` included), JavaScript value handles (`jsg::Data`, `jsg::Value`,
`JsValue`, `JsRef`, ...) and `jsg::Promise`/`jsg::Function`/`jsg::Sequence` are not allowed. The
containment is:

- Only files with an `interop` line in the allowlist are in the tier, and the test rejects an
  `interop` line for a file outside `src/rust/jsg-cpp`.
- Other shims must not use `jsg_cpp` (the test counts it as a construct), so `src/rust/jsg`
  cannot reach C++ JSG through it. Bazel enforces the direction too: `jsg-cpp` depends on `jsg`.
- The per-type and per-method shims that instantiate `jsg-cpp.h` for a C++ class live with the
  class, as C++ API code, not in a Rust JSG crate. They are not Rust JSG shims, so the allowlist
  does not check them, and they use C++ JSG freely, as the class does. Porting an API to Rust
  therefore moves C++ JSG use into these shims rather than removing it; it goes away with the C++
  classes the shims bind.

`cpp-jsg-allowlist-checker-test.py` tests what the tier admits and rejects.

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
`global-*`, `traced-global-*`) and `src/workerd/io/io-context/compile-tests`
(`await-io-and-run-*`), and the drop tests in `src/rust/jsg-test/tests/global.rs`,
`traced_global_lifetime.rs` and `gc.rs`.

## Resources

Rust resources integrate with V8's garbage collector through the existing C++ `Wrappable` infrastructure — the same GC system that C++ `jsg::Rc<T>` and `jsg::Object` use.

```rust
use jsg_macros::{jsg_resource, jsg_method};

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
    // Must be wrapped in jsg::Cell<_> for interior mutability (trace takes &self).
    callback: jsg::Cell<Option<jsg::v8::TracedGlobal<jsg::v8::Value>>>,
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
// Create a resource of the lock's isolate
let resource = jsg::Rc::new(&mut lock, MyResource { ... });

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
    fn schedule(this: jsg::Rc<Self>, lock: &mut jsg::Lock<'_>, scheduler: jsg::Rc<Scheduler>) {
        // `this` dereferences to the resource, like `&self`.
        this.scheduled.set(true);
        scheduler.tasks.borrow_mut(lock).push(this);
    }
}
```

Whatever holds the `jsg::Rc` must visit it in its `Traced::trace`, just as a resource visits
its `jsg::Rc` fields. A field it later moves the `jsg::Rc` out of, for example to run the
task, is a `jsg::Cell` or `jsg::RefCell` (here `tasks: jsg::RefCell<Vec<jsg::Rc<Task>>>`),
which make the `jsg::Rc` strong on the way out; see [Cells](#cells).

Don't give the callback the resource's state through an `std::rc::Rc` instead. Both rules are
explained in [Safety rules](#safety-rules), rule 2.

The callback's `jsg::Rc` keeps the resource alive, even after JavaScript drops the resource.
Cycles are fine if they go through a `TracedGlobal`: if the resource holds the callback in a
`TracedGlobal`, GC collects both once JavaScript drops them. A cycle through a
`jsg::v8::Global` is never collected. If the callback shouldn't keep the resource alive, give
it a `jsg::Weak<Self>` (`this.downgrade()`) instead.

Calling the method on something that isn't a resource of this type throws a `TypeError`, as it
does for `&self` methods. A method that takes `this` has no Fast API callback.

### Cells

A resource's methods take `&self`, so state that changes after construction lives in a cell.
For traced state, that is a `jsg::Cell` or a `jsg::RefCell`:

- `jsg::Cell<T>` moves its value in and out whole, like `std::cell::Cell`: `set(value)`,
  `take(lock)` and `replace(lock, value)`.
- `jsg::RefCell<T>` hands out borrows, like `std::cell::RefCell`: `borrow()` and
  `borrow_mut(lock)`.

`take`, `replace` and `borrow_mut` take the lock and detach what leaves the cell from its owner, as
a typed promise's reaction does with its value: every `jsg::Rc` and `TracedGlobal` in it is strong
again and forgets its owner. `borrow_mut` detaches the whole value, since the borrower can move any
of it out. GC visits a `jsg::Rc` or a `TracedGlobal` where it finds it, and once the owner is
reachable only through its wrapper the visit makes the reference weak and leaves it to the owner's
trace. Moved out of the owner by a plain Rust move, the reference would be neither strong nor
traced. The cells make it strong before it can leave, so whatever a method takes out, or pops from a
borrowed `Vec`, stays valid for as long as the method holds it. A reference that stays in the cell
is made weak again by the next GC visit. Everything in a mutably borrowed `jsg::RefCell` is strong,
so a GC that runs during the borrow, which cannot look inside, loses nothing.

```rust
#[jsg_resource]
struct Scheduler {
    pending: jsg::Cell<Option<jsg::Rc<Task>>>,
    queue: jsg::RefCell<Vec<jsg::Rc<Task>>>,
}

#[jsg_resource]
impl Scheduler {
    #[jsg_method]
    fn run(&self, lock: &mut jsg::Lock<'_>) {
        if let Some(task) = self.pending.take(lock) {
            task.run(lock); // `task` is strong, whatever ran in between
        }
        // Moved out of the borrow first, so that `run`, which can run JavaScript, doesn't find
        // the queue borrowed.
        let tasks = std::mem::take(&mut *self.queue.borrow_mut(lock));
        for task in tasks {
            task.run(lock);
        }
    }
}
```

`std::cell::Cell` and `std::cell::RefCell` are `Traced` only for types that hold no GC
references, which the marker trait `jsg::HoldsNoGcReferences` marks; a `jsg::Rc` or a
`TracedGlobal` in one is a compile error. Wrappers and collections have the marker when their
contents do, and `#[jsg_struct]` and `#[jsg_oneof]` derive it when every field or variant has it,
so for them it can't be claimed by hand. So a `jsg::Rc` or `TracedGlobal` that leaves a traced
field of a live resource is strong, unless a hand-written `impl Traced` reaches it through a std
cell; see [Safety rules](#safety-rules), rule 2, for the exceptions.

### GC Behavior

- **No JS wrapper**: Dropping the last `Rc` under the isolate lock immediately destroys the resource (no GC needed).
- **With JS wrapper**: Dropping all `Rc`s makes the wrapper eligible for V8 GC. When collected, the resource is destroyed.
- **Tracing**: The `#[jsg_resource]` macro auto-generates `Traced::trace` and calls it on every field:

| Field type | Traced? | Notes |
|---|---|---|
| `jsg::Rc<T>` | Yes — strong edge | Keeps target alive through GC |
| `Option<jsg::Rc<T>>` | Yes — when `Some` | |
| `jsg::Nullable<jsg::Rc<T>>` | Yes — when `Some` | |
| `jsg::NullOrUndefined<jsg::Rc<T>>` | Yes — when `Some` | |
| `jsg::Lenient<jsg::Rc<T>>` | Yes — when `Some` or `Unconvertable` | |
| `jsg::Cell<Option<jsg::Rc<T>>>` | Yes — when `Some` | Use `jsg::Cell` when the field is set after construction; `take` and `replace` make the `Rc` strong |
| `jsg::Cell<jsg::Nullable<jsg::Rc<T>>>` | Yes — when `Some` | |
| `jsg::Cell<jsg::NullOrUndefined<jsg::Rc<T>>>` | Yes — when `Some` | |
| `jsg::RefCell<Vec<jsg::Rc<T>>>` | Yes — each element | Use `jsg::RefCell` when the field is changed in place; `borrow_mut` makes every `Rc` strong |
| `Cell<T>` / `RefCell<T>` where `T` holds a `jsg::Rc` or a `TracedGlobal` | Compile error — no `Traced` impl | Use `jsg::Cell` / `jsg::RefCell` ([Cells](#cells)) |
| `Vec<jsg::Rc<T>>` | Yes — each element | Iterates and visits every `Rc` in the vec |
| `HashMap<K, jsg::Rc<T>>` | Yes — each value | Iterates `.values()` and visits each `Rc` |
| `BTreeMap<K, jsg::Rc<T>>` | Yes — each value | Iterates `.values()` and visits each `Rc` |
| `jsg::Dict<jsg::Rc<T>>` | Yes — each value | Visits the value of every entry |
| `HashSet<jsg::Rc<T>>` | Yes — each element | Iterates and visits every `Rc` |
| `BTreeSet<jsg::Rc<T>>` | Yes — each element | Iterates and visits every `Rc` |
| `jsg::Cell<Vec<jsg::Rc<T>>>` | Yes — each element | `jsg::Cell` variant of above |
| `jsg::Cell<HashMap<K, jsg::Rc<T>>>` | Yes — each value | `jsg::Cell` variant of above |
| Same patterns with `jsg::v8::TracedGlobal<T>` | Yes | All collection forms work with `TracedGlobal<T>` too, in `jsg::` cells |
| `jsg::v8::TracedGlobal<T>` | Yes — dual strong/traced | Enables cycle collection; see below |
| `Option<jsg::v8::TracedGlobal<T>>` | Yes — when `Some` | |
| `jsg::Nullable<jsg::v8::TracedGlobal<T>>` | Yes — when `Some` | |
| `jsg::NullOrUndefined<jsg::v8::TracedGlobal<T>>` | Yes — when `Some` | |
| `jsg::Cell<jsg::v8::TracedGlobal<T>>` | Yes — dual strong/traced | Required when set after construction; `replace` makes the handle strong again |
| `jsg::Cell<Option<jsg::v8::TracedGlobal<T>>>` | Yes — when `Some` | |
| `jsg::Weak<T>` | No | Doesn't keep target alive |
| `jsg::v8::Global<T>` | Compile error — no `Traced` impl | Use `TracedGlobal<T>` in resource fields |
| `jsg::TracedAsyncContextFrame` | Yes — like `TracedGlobal<T>` | Nothing to trace for the root frame |
| `jsg::AsyncContextFrame` | Compile error — no `Traced` impl | Use `TracedAsyncContextFrame` in resource fields |
| `std::rc::Rc<T>` where `T` holds a `jsg::Rc` or a `TracedGlobal` | Compile error — no `Traced` impl | Share a resource through a `jsg::Rc` or `jsg::Weak` instead (Safety rules, rule 2) |
| Any `T: Traced` | Depends on `T` | `#[jsg_resource]` calls `Traced::trace` on every field |

- **Cells for interior mutability**: `Traced::trace` takes `&self`. Fields that need to be mutated after construction (e.g. a callback set in a method) use a cell: `jsg::Cell<T>` or `jsg::RefCell<T>` for a field that holds a `jsg::Rc` or a `TracedGlobal`, and `std::cell::Cell<T>` or `RefCell<T>` for anything else; see [Cells](#cells).
- **`Traced` drives field tracing**: `#[jsg_resource]` now traces every named field via `Traced::trace(&self.field, visitor)`. Types with no GC edges use no-op `Traced` impls, which `jsg::jsg_traced!` generates; `trace` has no default, so a hand-written impl can't leave it out. Wrappers/collections delegate recursively.
- **Nested wrappers are supported by composition**: `Option<Vec<jsg::Rc<T>>>` works as long as each layer implements `Traced`.
- **`#[jsg_resource(custom_trace)]`**: suppresses the generated `Traced` impl so you can write your own. `GarbageCollected` (`memory_name`), `Type`, `ToJS`, and `FromJS` are still generated.
- **Heap snapshots**: every resource is a node of its own, linked to its JavaScript wrapper, as a C++ resource is. `#[jsg_resource(memory_info)]` has it also report its fields through a `jsg::VisitForMemoryInfo` impl generated from the named fields (`#[jsg_memory_info(skip)]` leaves one out), the counterpart of C++ `visitForMemoryInfo`; `#[jsg_resource(custom_memory_info)]` takes a hand-written impl instead, which reports with `tracker.track_field(c"name", &self.field)` for any `jsg::MemoryInfo`; a helper type implements `jsg::MemoryRetainer`, the counterpart of `JSG_MEMORY_INFO`, and registers it with `jsg::jsg_memory_retainer!`. See `memory.rs` and [`jsg-macros/README.md`](../jsg-macros/README.md#heap-snapshots).
- **`jsg::v8::TracedGlobal<T>` cycle collection**: Uses the same strong↔traced dual-mode as C++ `jsg::V8Ref<T>`. While the parent resource has strong Rust refs the JS handle stays strong. Once all Rust `Rc`s are dropped, `visit_traced_global` downgrades the handle to a `v8::TracedReference` that cppgc can follow — allowing cycles (e.g. a resource holding a callback that captures its own wrapper) to be detected and collected.
- **Circular references** through `jsg::Rc<T>` are **not** collected, matching C++ `jsg::Rc<T>` behavior.

### Dropping references

An `Rc` holds the resource in an `std::rc::Rc` and its C++ `Wrappable` in a `kj::Rc`, and a `Weak`
holds an `std::rc::Weak`. None of their reference counts is atomic: the isolate lock is what keeps
two threads from updating them at once. Releasing a strong reference may also touch the resource's
wrapper. Yet an `Rc` or a `Weak` can be dropped without the lock, for example by C++ code that
owns the Rust object holding it. So each one keeps the state of the isolate its resource was
created in, which the `Realm` shares with every persistent handle of it, and a drop:

- under the isolate lock, releases the reference at once;
- without the lock, on any thread, moves its parts into a queue without touching any count, and
  the next `IsolateBase::Lock` releases them, before it resets the queued handles of `Global`s and
  `TracedGlobal`s. Until then the reference keeps the resource alive, as a C++ `jsg::Ref` in the
  deferred destruction queue does;
- once the `Realm` is dropped, on any thread, keeps the reference until the isolate is disposed.
  Until then the isolate still detaches wrappers and destroys the `CppgcShim`s that own
  `Wrappable`s, and no thread can ask whether it holds the lock of an isolate that may already be
  gone. Once the isolate is disposed, the kept references are released, and so are later drops.
  Those releases only give back the counts, with no wrapper left to update, since a `Wrappable`
  that teardown never wrapped may still point to the disposed isolate. No lock serializes them,
  so they go through the queue, which one thread at a time drains; a clone is not serialized,
  so a resource that outlives its isolate must be used by one thread at a time.

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
            let resource = jsg::Rc::new(lock, MyResource { label: label.to_owned() });
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
`Lock` of its isolate except dropping it and putting it in a new resource (`jsg::Rc::new` takes the
lock of the resource's isolate, which may be another one), and both leave its handles alone on a
thread that does not hold its isolate lock.

A `TracedGlobal` in traced mode also holds a `v8::TracedReference`, which Rust never resets. A
`TracedGlobal` taken out of its resource is traced by nothing, so the next full GC may free its
node, and nothing tells the `TracedGlobal` that it moved, the way C++ JSG's `jsg::Data` move
constructor re-strengthens its handle. So each GC visit lets go of the old node and makes the
strong handle strong again before C++ decides afresh whether to install a new node, and a drop
just lets go of it. A detaching visit, which `jsg::Cell::take` and the other ways out of a
`jsg::` cell run, does the same, so a `TracedGlobal` leaves its resource strong. V8 frees nodes
that nothing traces at the next full GC; see `GcVisitor::visit_traced_global` for the cost of
this and how to avoid it.

Bitwise moves are also why a `TracedGlobal` keeps its handles in a box. In traced mode the strong
handle is weak, and V8 keeps the address of its slot to null it when the value dies; the box keeps
that address valid however the `TracedGlobal` itself moves. A `TracedGlobal` leaves a `jsg::` cell
strong, and a `std::cell::Cell` or `RefCell` cannot hold one, so a weak `TracedGlobal` moves only on
its way to being dropped (`jsg::Cell::set`) or in a resource's `Drop` impl, which has `&mut self`.
The box lives until the strong handle has been reset. GC can still run after the `Realm` is dropped,
so the box of a `TracedGlobal` dropped then is kept until the isolate is disposed.

A `TracedGlobal` taken out of a `jsg::Cell` with `take(lock)` or `replace(lock, ..)`, or moved
out of a mutably borrowed `jsg::RefCell`, is strong and keeps its value; see [Cells](#cells).
Tracing skips a `jsg::RefCell` while it is mutably borrowed, which is safe because `borrow_mut`
made everything in it strong.

`TracedGlobal<T>` fields on `#[jsg_resource]` structs participate in GC tracing when visited via `GcVisitor::visit_traced_global`. This enables the garbage collector to detect and collect back-reference cycles — for example, a resource that stores a JS callback which closes over the resource's own JS wrapper:

```rust
#[jsg_resource]
struct EventEmitter {
    // jsg::Cell<Option<_>> for interior mutability: the callback is set after
    // construction, and trace() receives &self.
    on_event: jsg::Cell<Option<jsg::v8::TracedGlobal<jsg::v8::Value>>>,
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

- A reference that `T::trace` does not visit, such as a `v8::Global` (which has no `Traced`
  impl, but can hide inside a type with a hand-written one). If it can reach the function, the cycle is invisible to the garbage
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

### Async context

`jsg::AsyncContextFrame::current(lock)` captures the async context frame that is current now,
which carries the values of `AsyncLocalStorage`, and `frame.enter(lock, |lock| ...)` runs a
closure with it current, as C++ JSG's `AsyncContextFrame::currentRef()` and
`AsyncContextFrame::Scope` do. `enter` makes the previous frame current again however the closure
ends, including with an error, while execution is terminating, or by panicking. It opens no handle
scope, so handles created in the closure stay valid after it. A microtask checkpoint run inside the
closure leaves the root frame current after it, as under C++ JSG's `AsyncContextFrame::Scope`.

V8 carries the current frame into promise reactions, those that `Local<Promise>::then` registers
included, so they need nothing more. Code that resumes JavaScript from outside a JavaScript
continuation does: the continuation of async I/O, or a callback that runs later, captures the frame
when the work starts and enters it when it resumes.

The current frame is the isolate's continuation-preserved embedder data: `undefined` for the root
frame (`AsyncContextFrame::root()`, `is_root()`), otherwise the opaque wrapper of a C++
`jsg::AsyncContextFrame`. A captured frame holds that wrapper, which owns the C++ frame, so the
frame and its values live as long as an `AsyncContextFrame` holding it, or as long as the resource
that traces a `TracedAsyncContextFrame` holding it.

As with `Global` and `TracedGlobal`, there are two types:

- `AsyncContextFrame` holds a `Global`, for a frame that Rust holds outside a resource, such as in
  a future passed to async I/O. It is `Send` and `Sync`, and has no `Traced` impl.
- `TracedAsyncContextFrame` holds a `TracedGlobal`, for a resource field or `Function::new` data,
  so that GC collects a cycle through the frame's values. It is `Send`, not `Sync`, and must have
  exactly one owner that GC traces. `TracedAsyncContextFrame::from` converts an
  `AsyncContextFrame`, and `to_async_context_frame(lock)` goes the other way, for example to pass
  a resource's frame to async I/O. Like a `TracedGlobal`, a `TracedAsyncContextFrame` that a
  resource changes after construction lives in a `jsg::Cell` or `jsg::RefCell`, which make it
  strong as it leaves; see [Cells](#cells).

```rust
let frame = jsg::AsyncContextFrame::current(lock);
// ... later, under another lock:
frame.enter(lock, |lock| callback.call(lock, None, &[]))?;
```

## Typed promises

`jsg::Promise<'s, T>` is the counterpart of C++ JSG's `jsg::Promise<T>`: a promise that carries
a Rust `T`, which need not have a JavaScript form. The JavaScript promise under it is settled
with an opaque wrapper of the value, which JavaScript never sees, and the value is converted
only where the promise crosses into or out of JavaScript, where C++ JSG converts it:

- Returned to JavaScript (`ToJS`), it becomes the promise of a reaction that converts the value,
  as C++ `PromiseWrapper::wrap()` does: it settles one microtask after the typed promise, and is
  marked as handled if the typed promise was (`mark_as_handled`).
- Taken from JavaScript (`FromJS`, as a method parameter), a promise gets one reaction that
  converts its value, as C++ `thenUnwrap()` does. A value of the wrong type rejects it with
  C++ JSG's `TypeError` ("Incorrect type for Promise: the Promise did not resolve to 'T'.");
  a conversion that fails, such as of a non-integer to a `u32`, rejects it with its own error.
  A value that is not a promise is converted at once; a thenable is followed first if
  `unwrap_custom_thenables` is on.

`T` is any `Traced` type, including ones that hold `jsg::Rc`s and `TracedGlobal`s: a
reaction gets the value detached from the wrapper (Safety rules, rule 2). `Promise<()>` is
`jsg::Promise<void>`: it is settled with `undefined`, and as an argument it accepts any value.
`Promise<v8::Value>` is `jsg::Promise<jsg::Value>`: it carries the JavaScript value itself, and
its reactions take a `Local<v8::Value>`. A marker such as `v8::Uint8Array` names a kind of
handle, not a value, so there is no `Promise<v8::Uint8Array>`; use `Promise<v8::Value>`. The
untyped `Local<v8::Promise>` stays the counterpart of `JsPromise`.

As in C++, a typed promise is used once: `then`, `catch` and `then_or_catch` consume it, so only
one reaction ever takes the value, and moves it out of its wrapper. `when_resolved` gives a
`Promise<()>` without consuming it. The callbacks follow the rules of `Local<v8::Promise>::then`:
they capture nothing, and their state is the reaction's `data`. Their return type is the
projection `U::Value<'a>`, so the `U` of the promise they return is inferred from where that
promise goes, such as a method's return type, or from an annotation
(`let p: jsg::Promise<String> = ...`, or `then::<_, String, _>(...)`):

```rust
#[jsg_method]
fn describe<'s>(
    &self,
    lock: &mut jsg::Lock<'s>,
    input: jsg::Promise<'s, u32>,
) -> Result<jsg::Promise<'s, String>, jsg::Error> {
    let doubled: jsg::Promise<'s, u32> = input.then(lock, (), |_lock, (), n| Ok(n * 2))?;
    doubled.then(lock, (), |_lock, (), n| Ok(format!("twice that is {n}")))
}
```

A callback that returns a promise, for the returned promise to follow, goes to `then_promise`,
`catch_promise` or `then_or_catch_promise`, which C++ JSG's `then()` and `catch_()` cover by
chaining a continuation that returns a `jsg::Promise<U>`.

`Promise::new_with_resolver` gives a promise and its `PromiseResolver<'s, T>`. To keep either
beyond the current scope, as in a resource field, make it a `TracedPromise<T>` or a
`TracedPromiseResolver<T>` with `into_traced`; the garbage collector traces both. A method that
returns a promise reports its errors by rejecting it rather than by throwing: `Lock::eval_now`
runs a closure that returns a promise and turns what it fails with into a rejected promise, as
C++ JSG's `Lock::evalNow()` does; a closure that has a plain value returns
`Promise::resolved(lock, value)`.

The promise that an instance `#[jsg_method]` or property returns keeps the resource alive until
it settles, as C++ JSG's do: the method passes its receiver to `ToJS::to_js_with_creator`, and the
promise's conversion reaction holds it, as `PromiseWrapper::wrap()` makes `creator` the data of
its reaction. Containers, `#[jsg_struct]` fields and `#[jsg_oneof]` variants pass the creator on,
so a promise inside them keeps it too.

A value that JavaScript settled a typed promise with, which can only happen through a
`Promise.prototype.then` it replaced (V8 calls it when `resolve_with_promise` resolves one promise
with another, as C++ JSG's `Resolver::resolve()` does), rejects the reaction with an internal
error, as C++ JSG fails with a `KJ_ASSERT`.

## Errors from JavaScript

When a function that `Local<Function>::call` calls throws, the exception stays pending in V8, as with C++ JSG's `JsExceptionThrown`, and `call` returns an error for which `Error::is_pending()` is true. The same goes for these other operations that run JavaScript: converting a value to a string or a number (its `toString`, `valueOf` or `Symbol.toPrimitive`, or a Proxy trap), including with `Utf8Value::new`; `Object::get` and `Object::has` (a getter or Proxy trap), `Object::set` (a setter, also one on the prototype chain, or a Proxy trap) and `Array::set` (a setter, also one on the prototype chain), `Array::iterate` and so converting to a `Vec` (an element's getter), `ser::serialize` (a getter or Proxy trap, or a `DataCloneError`), and `json::parse` and `json::stringify` (a `SyntaxError`; a getter, `toJSON()` or Proxy trap). Building the array when converting a `Vec` to JavaScript runs no JavaScript: like C++ JSG, it defines the elements as data properties (`Array::with_elements`), so no setter on `Array.prototype` runs. Nor does converting a `jsg::Dict` to JavaScript, which defines its entries as data properties. A `#[jsg_struct]` follows C++ JSG's `fast_jsg_struct` flag: with it, the fields are defined as data properties (`Object::with_data_properties`), an absent optional field as `undefined`, and no JavaScript runs; without it, they are set one by one with `Object::set`, an absent optional field is omitted, so a setter on `Object.prototype` runs, and what it throws is passed on. `Error`'s own `FromJS`, which reads `name` and `message`, passes such an error on too. `Lock::throw_exception` throws nothing for that error, so a callback that returns it propagates the original exception to its JavaScript caller. V8 keeps reporting the exception where it was thrown, and DevTools' "pause on uncaught exceptions" still sees whether JavaScript further up catches it. A termination is handled the same way.

A `#[jsg_method]`, property getter or wildcard property converts its return value with `ToJS::to_js_with_creator`, passing the resource's wrapper for an instance member and `None` for a static one, as C++ JSG's `wrap()` takes a `creator`. Only a value that stays pending, a `jsg::Promise`, uses it, and a container passes it on; a `ToJS` impl that holds such values should forward it too.

Converting a value to JavaScript can fail too, as C++ JSG's `wrap()` can throw, so `ToJS::to_js` returns a `Result`: creating a `Date` or a resource's wrapper fails, leaving the exception pending, when V8 throws or an exception or a termination is already pending, and a container passes on the error from converting an element, whichever kind it is. A `&str` or `String` longer than `v8::String::MAX_LENGTH` fails with a `RangeError` ("Invalid string length", as V8 reports it), which is not pending; every other conversion of a single value in this crate fails only with a pending error. A `#[jsg_method]`, property getter or wildcard property throws an error from converting its return value as it throws one the method returns, with `Lock::throw_exception`: for a pending error the original exception, or the termination, keeps propagating, and any other error is thrown as a new JavaScript error of its type. The method has already run by then; only its return value is lost.

Code that gets a pending error must not call into JavaScript, and should return the error. Converting it to another error and throwing that replaces the exception, as when C++ code throws while a `JsExceptionThrown` unwinds; nothing ever replaces a termination. While an exception or a termination is pending, none of these operations runs: each fails with a pending error without entering V8, which would clear the exception or run JavaScript with it pending, even for a value that would convert without running JavaScript (Safety rules, rule 1). A pending error is only meaningful while its exception is pending: one kept past that, such as a clone stored after `try_catch` caught the exception, is thrown as an internal error when V8 has no exception pending.

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

## Errors thrown from Rust

An `Error`'s `name` is an `ErrorName`: a JavaScript error class (`TypeError`, `RangeError`,
`ReferenceError`, `SyntaxError`, `Error`), or a `DOMException` with any name, as C++
`JSG_REQUIRE(..., DOMAbortError, ...)` throws. The two are separate because `SyntaxError` is both.
The `ExceptionType`s named after a DOMException (`OperationError`, `DataCloneError`, `AbortError`
and the rest, for which `ExceptionType::is_dom_exception()` is true) are shorthands for that
`DOMException`, as in `jsg_require!(..., AbortError, ...)`; `Error::new_dom_exception` takes any
name, including `SyntaxError` and names that are no `ExceptionType`, such as `QuotaExceededError`.
A C++ error that reaches Rust as a `kj::Exception` keeps its DOMException name, whatever it is, and
`FromJS` for `Error` reads a JavaScript error as its class, even one whose `name` is a DOMException
name. An `ErrorName` compares equal to the `ExceptionType` that is thrown the same way, so
`err.name == ExceptionType::TypeError` works. `Lock::dom_exception` creates a `DOMException` with
any name, as C++ `jsg::Lock::domException()` does. Both create it through `jsg::newDOMException()`
(Safety rules, rule 3), which needs the current context: while an exception or a termination is
pending, or if creating it throws, `Lock::dom_exception` and `Lock::exception` fail with a pending
error, and `Lock::throw_exception` throws nothing, so the pending exception propagates. A fast
callback never throws a `DOMException`, since only a method that returns no `Result` has one.

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

## JSON

`jsg::json::parse` and `jsg::json::stringify` are C++ JSG's `jsg::Lock::parseJson()` and
`serializeJson()`: `JSON.parse()` without a reviver and `JSON.stringify()` without a replacer,
through V8 directly, so a script that replaces the `JSON` global doesn't affect them.
`jsg::json::stringify_with_gap` indents the result, as `JSON.stringify()`'s third argument does.

```rust
let value = jsg::json::parse(lock, r#"{"a": [1, 2]}"#)?;
assert_eq!(jsg::json::stringify(lock, value)?, r#"{"a":[1,2]}"#);
```

Invalid JSON leaves a `SyntaxError` pending. What stringifying runs (getters, `toJSON()`, Proxy
traps) can throw, as can a cycle or a `BigInt`. Both return a pending error then (see
[Errors from JavaScript](#errors-from-javascript)). A `text` or `gap` longer than
`v8::String::MAX_LENGTH` bytes fails with `Error::invalid_string_length()`, as converting a
`&str` to JavaScript does. A value that JSON can't represent, which
`JSON.stringify()` returns `undefined` for (such as `undefined`, a function or a symbol), gives
the string `"undefined"`, as in C++ JSG.

## Type Conversions

The module docs of `wrappable.rs` list every Rust type that converts to or from JavaScript, and
how. Most convert both ways. `&[T]` converts only from JavaScript, so it can be a method parameter
but not a return value. A number is an `f64`, which converts like C++ JSG's `double`: to a number,
and from any value with JavaScript's `ToNumber`. An `f32` converts only to JavaScript. A number
that stays on the V8 heap is a `v8::Local<'s, v8::Number>`, which accepts only a number. A
`String` converts from any value with `ToString`, copying it out of the V8 heap. A
`jsg::JsString<'s>` parameter performs the same coercion without copying or decoding the string,
like C++ JSG's `jsg::JsString`. It preserves lone UTF-16 surrogates and returns to JavaScript
unchanged. It dereferences to `v8::Local<'s, v8::String>` for string operations, and converts to
and from that handle with `From`/`Into`. Like the handle, it cannot outlive its scope. For an
explicit coercion of a `v8::Local<'s, v8::Value>`, call `to_js_string(lock)`.

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

### Rest parameters

`jsg::Arguments<T>`, C++ JSG's `jsg::Arguments<T>`, is a rest parameter: as the last parameter of
a `#[jsg_method]` or a `#[jsg_constructor]`, it takes every argument from its position on, each
converted to `T`.

```rust
#[jsg_method]
pub fn exec(&self, query: String, bindings: jsg::Arguments<Binding>) -> Cursor { /* ... */ }
// JS: sql.exec("SELECT ?, ?", 1, "a")  // bindings == [1, "a"]
```

- Without remaining arguments it is empty; omitting them is never an error.
- Each argument converts as a parameter of type `T` would, and a mismatch is the same
  `TypeError`, which names the rest parameter's position for every argument, as C++ JSG's does.
- It is not counted in `.length`, and a parameter after it is a compile error.
- It holds the values in a `Vec`: it dereferences to `[T]`, iterates, and `into_inner()` returns
  the `Vec`. It converts only from the arguments of a call; a JavaScript array is a `Vec<T>`.
- RTTI describes it as C++ RTTI does, without the element type, so TypeScript declares it as
  `...name: any[]`; give a precise type with a TypeScript override.

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
strings (including `jsg::JsString<'s>`), numbers, the integer types, booleans, `SystemTime`,
resources, structs, `Vec<T>`, `Dict<V>`, `Nullable<T>`, `NonCoercible<T>`, other unions, and
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

## Explicit resource management

`#[jsg_dispose]` installs a method as `Symbol.dispose` of the prototype, which `using` calls when
the variable goes out of scope, like C++ JSG's `JSG_DISPOSE`. `#[jsg_async_dispose]` installs one
as `Symbol.asyncDispose`, which `await using` calls and whose returned promise it awaits, like
`JSG_ASYNC_DISPOSE`:

```rust
#[jsg_resource]
impl Connection {
    // close() and [Symbol.dispose](), like JSG_METHOD(close); JSG_DISPOSE(close);
    #[jsg_method]
    #[jsg_dispose]
    pub fn close(&self) { /* ... */ }
}

#[jsg_resource]
impl Flusher {
    // Only [Symbol.asyncDispose](). The Rust name is irrelevant: without #[jsg_method], the
    // method has no JavaScript name of its own.
    #[jsg_async_dispose]
    pub fn shutdown<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<jsg::Promise<'s, ()>> {
        /* ... */
    }
}
```

- The method takes `&self`, or `this: jsg::Rc<Self>` in its place, and is called like a
  `#[jsg_method]`, with the type's `v8::Signature`. It is a non-enumerable property of the
  prototype, with `.length` 0, that cannot be called with `new`. It may also be a
  `#[jsg_method]` or a `#[jsg_callable]`, but not a `#[jsg_property]`, an
  `#[jsg_inspect_property]` or a `#[jsg_wildcard_property]`, which are registered without the
  signature its callback uses.
- An async disposer should return a promise (`jsg::Promise<'s, T>` or
  `v8::Local<'s, v8::Promise>`, optionally in a `Result`). `await using` awaits whatever it
  returns, so any other value makes it a sync disposer that finishes a microtask later: write a
  `#[jsg_dispose]` instead, which `await using` calls too.
- A type has at most one disposer of each kind, as in C++ JSG. It may have both a sync and an
  async one, as Node.js's `readline/promises` `Interface` does, though C++ JSG discourages it:
  `using` calls only the sync one and `await using` prefers the async one. One method cannot be
  both. With automatic registration a second disposer of a kind is a compile error;
  `custom_members` may choose between several by compatibility flag, registering
  `Self::<name>_dispose_member()` or `Self::<name>_async_dispose_member()`, and registering two of
  a kind panics when the template is created.
- RTTI does not describe disposers. C++ RTTI does, but its TypeScript generator ignores them.

As for C++ JSG (see the comment on `JSG_DISPOSE` in `src/workerd/jsg/jsg.h`):

- Prefer `#[jsg_dispose]`: `await using` accepts a sync disposer too.
- Disposal usually runs because the scope threw. Expose clean shutdown as an explicit method, and
  make the disposer only release the resource.
- The disposer is not told about the exception, and an error it returns replaces the exception,
  which becomes the `suppressed` of a `SuppressedError`.
- JavaScript can call the disposer directly, any number of times, so it must be idempotent. It
  must also be safe to re-enter: a disposer that runs JavaScript, such as a callback or an event
  listener, can be called again before it returns. Mark the resource disposed before calling out,
  so the nested call finds nothing left to do, and hold no `RefCell` borrow across the call.

## Callable resources

`#[jsg_callable]`, the equivalent of C++ JSG's `JSG_CALLABLE`, makes the instances of a resource
type callable as functions:

```rust
#[jsg_resource]
impl Statement {
    // `statement(1, 2)` runs the method.
    #[jsg_callable]
    pub fn run(&self, lock: &mut jsg::Lock, a: f64, b: Option<f64>) -> String {
        /* ... */
    }
}
```

A callable resource behaves as in C++ JSG:

- The method takes `&self`, or `this: jsg::Rc<Self>` in its place, and otherwise has the shape
  of a `#[jsg_method]`: an optional `lock: &mut jsg::Lock`, then the JavaScript arguments, which
  are converted as a method's are. An argument error names the method `(called as function)`.
  A type is callable at most one way, though `custom_members` may choose between several by
  compatibility flag. The method may also be a `#[jsg_method]`, to be callable both ways.
- The method runs whether the instance is called with or without `new`.
- `this` is always the instance, however the call names it: a callable read from another object
  (`const f = obj.method; f()`), or called with `Reflect.apply()` and another receiver, still runs
  on the instance. The resource cannot tell how it was called.
- `typeof` is `"function"`, but the instance is an ordinary instance of the resource type: its
  prototype is the type's prototype, not `Function.prototype`, so it has no `call`, `apply` or
  `bind`, and `instanceof Function` is false.
- Only the instance is callable: an object that inherits from it, as `Object.create(obj)` does,
  is not.
- With a [wildcard property](#wildcard-properties) as well, every property can be callable, which
  is how C++ JSG's RPC stubs are built.
- RTTI describes the method as the type's call signature (the `callable` of its structure), as
  C++ RTTI describes a `JSG_CALLABLE`. The TypeScript generator does not read it, so C++ types
  declare their call signatures with `JSG_TS_OVERRIDE`.

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
