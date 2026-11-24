These checkists are to be used when performing code reviews for memory safety, thread safety, and concurrency correctness. They are not exhaustive but cover common patterns and issues to look for. Always consider the specific context of the code being reviewed and apply judgment accordingly.

### Memory Safety

- **Always** identify potential memory leaks, use-after-free, and dangling pointers or references
- **Always** review ownership semantics and lifetime management
- **Always** analyze smart pointer usage (`kj::Own`, `kj::Rc`, `kj::Maybe`)
- **Always** check for proper RAII, CRTP patterns
- **Always** look for potential buffer overflows and bounds checking issues
- **Always** identify raw pointer usage that could be safer with owning types
- **Always** review destructor correctness and cleanup order
- **Always** analyze lambda captures for lifetime safety
- **Always** consider patterns where weakrefs (see `util/weak-refs.h`) or other techniques would be safer
- **Always** verify that methods returning references, pointers, `kj::ArrayPtr`, `kj::StringPtr`, or other non-owning views into data owned by `this` (or by a parameter) are annotated with `KJ_LIFETIMEBOUND`. This expands to `[[clang::lifetimebound]]` and enables the compiler to warn when the returned
  view outlives the object it borrows from.
- Functions returning owned resources, `kj::Maybe`, error indicators, or expensive-to-compute
  results **should** be annotated with `KJ_WARN_UNUSED_RESULT`. This catches two classes of problems:
  silently discarding results the caller must act on (e.g., error codes, `kj::Promise`), and
  performing expensive computation whose result is thrown away.
- Lambdas capturing `jsg::Ref<T>` or other GC-traced references **should** use `JSG_VISITABLE_LAMBDA`
  (see `jsg/function.h`) so V8's GC can trace through the captures.
- **Always** verify that RAII scope guards, locks, and other types with positional semantics use
  `KJ_DISALLOW_COPY_AND_MOVE` to prevent accidental moves that break scope invariants. Use
  `KJ_DISALLOW_COPY` only when explicit move semantics are intentionally provided. Flag new
  scope-guard or lock types that are missing these annotations.

### Thread Safety & Concurrency

- **Always** identify data races and race conditions
- **Always** review lock ordering and deadlock potential
- **Always** analyze shared state access patterns
- **Always** check for proper synchronization primitives usage
- **Always** reeview promise/async patterns for correctness
- **Always** identify thread-unsafe code in concurrent contexts
- **Always** analyze KJ event loop interactions
- **Always** ensure that code does not attempt to use isolate locks across suspension points in coroutines.
  Types that must never be held across `co_await` should carry the `KJ_DISALLOW_AS_COROUTINE_PARAM`
  annotation for compile-time enforcement. This is already applied to `jsg::Lock`, `Worker::Lock`,
  `jsg::V8StackScope`, and similar types. Flag new lock or scope types that are missing it.
- **Always** ensure that RAII objects and other types that capture raw pointers or references are not unsafely
  used across suspension points without validating that captured references and pointers remain valid.
- **Always** pay particular attention to V8 garbage collection interactions and cleanup order
- **Always** verify that kj I/O objects are never held by a V8-heap object (especially `jsg::Object` instances) without use of `IoOwn` or `IoPtr` (see `io/io-own.h`) to ensure proper lifetime and thread-safety.
- **Always** watch carefully for places where `kj::Refcounted` is used when `kj::AtomicRefcounted` is required
  for thread safety.

### Critical Detection Patterns

**Always** watch for these concrete patterns. When you encounter these, flag them at the indicated severity.

**Always** watch for non-obvious complexity at V8/KJ boundaries, including:
- re-entrancy bugs where a C++ callback unexpectedly re-enters JavaScript
- subtle interactions between KJ event loop scheduling and V8 GC timing
- cases where destruction order depends on runtime conditions.

**CRITICAL / HIGH:**

- **V8 callback throwing C++ exception**: A V8 callback (JSG method, property getter/setter) that
  can throw a C++ exception without using `liftKj` (see `jsg/util.h`). V8 callbacks **must** catch
  C++ exceptions and convert them to JS exceptions.
- **V8 heap object holding kj I/O object directly**: A `jsg::Object` subclass storing `kj::Own<T>`,
  `kj::Rc<T>`, `kj::Arc<T>` for an I/O-layer object without wrapping in `IoOwn` or `IoPtr` (see
  `io/io-own.h`). Causes lifetime and thread-safety bugs.
- **`kj::Refcounted` in cross-thread context**: A class using `kj::Refcounted` whose instances can
  be accessed from both the I/O thread and the JS isolate thread. Needs `kj::AtomicRefcounted`.
- **Isolate lock held across `co_await`**: Holding a `jsg::Lock`, V8 `HandleScope`, or similar V8
  scope object across a coroutine suspension point. This is undefined behavior.
- **RAII object with raw pointer/reference across `co_await`**: Any RAII type or variable capturing
  a raw pointer or reference used across a coroutine suspension point without `kj::coCapture` to
  ensure correct lifetime.
- **Reference cycle between `jsg::Object` subclasses**: Two or more `jsg::Object` subclasses holding
  strong references (`jsg::Ref<T>` or `kj::Own<T>`) to each other without GC tracing via `JSG_TRACE`.
  Causes memory leaks invisible to V8's GC. This includes transitive cycles (e.g., A holds B, B
  holds C, C holds A) and cycles involving more than two objects.

**MEDIUM (safety-related):**

- **JavaScript during deserialization**: A `JSG_SERIALIZABLE` type's `deserialize()`, or code it
  calls, doing something that can run JavaScript outside a `deserializer.afterRead()` callback. The
  read then fails with `DataCloneError`. Walk it as "Reviewing a `deserialize()`" below
  describes.
- **Broad capture in async lambda**: Lambda passed to `.then()` or stored for deferred execution
  using `[&]` or `[this]` when only specific members are needed. Prefer explicit captures and
  carefully consider captured variable lifetimes.
- **Implicit GC trigger in sensitive context**: V8 object allocations (e.g., `ArrayBuffer` backing
  store creation, string flattening, `v8::Object::New()`) inside hot loops or time-sensitive
  callbacks may trigger GC unexpectedly.
- **Missing `DISALLOW_KJ_IO_DESTRUCTORS_SCOPE` awareness**: The `DISALLOW_KJ_IO_DESTRUCTORS_SCOPE`
  macro (see `jsg/wrappable.h`) creates a scope that crashes the process if any KJ async/I/O object
  is destroyed. It enforces that JS-heap objects use `IoOwn`/`IoPtr` rather than holding I/O objects
  directly. `IoOwn`'s destructor creates a matching `AllowAsyncDestructorsScope` to permit safe
  destruction. When reviewing code that introduces new GC or destructor paths, verify these scopes
  are correctly nested.

### Reviewing a `deserialize()`

A `JSG_SERIALIZABLE` type's `deserialize()` runs while V8 reads a value, where no JavaScript may
run (the rule is in `jsg/ser.h`, above `class Serializer`; `docs/jsg.md`, "Running No JavaScript
While Reading", has the details). A property lookup that may reach a prototype, an assignment,
and a conversion of an object (`toJSON`, `toString`, `valueOf`, `Symbol.toPrimitive`) can each
run JavaScript. An attempt to run JavaScript fails the read with `DataCloneError`. Decide whether
a `deserialize()` is safe by walking it statically:

1. **Find the roots.** Every `deserialize()` the change adds or touches, and anything else that
   runs during a read: `jsg/ser.c++`'s own readers (errors, classes implemented in JavaScript),
   the Rust deserializer in `src/rust/jsg/ser.c++`, and the RPC external handlers' `read()` and
   `claimPrebuilt*()` in `api/worker-rpc.c++`.
2. **Walk every call**, following each callee whose body you can read, until each path ends at a
   primitive that runs no JavaScript, or at an operation that can.
   - **Runs no JavaScript:**
     - raw reads (`readRawUint32()`, `readRawBytes()`, `readLengthDelimitedString()`, ...) and a
       nested `deserializer.readValue(js)`;
     - `getExternalHandler()`, and the handlers' `read()` and `claimPrebuilt*()`;
     - `tryCast<T>()` and type checks;
     - creating values (`js.obj()`, `js.str()`, `js.alloc<T>()` with a constructor that only
       stores what it is given) and `JsObject::createDataProperty()`;
     - `addRef()`, `JsRef`/`V8Ref`, `getHandle()`, `FeatureFlags::get(js)` and
       `IoContext::current()`;
     - `TypeHandler<T>::tryUnwrap()` where `T` is a `jsg::Ref<U>`, a `Js*` handle, or a
       `JSG_STRUCT`. A struct reads only own properties during a read. Its string and number
       fields still convert their values: the primitives that a serializer writes there convert
       without JavaScript, and a payload that holds an object there fails the read.
   - **Can run JavaScript, and so is a finding outside an `afterRead()` callback:**
     - `JsObject::get()`, `has()` without `HasOption::OWN`, `set()`, and any lookup that may reach
       a prototype;
     - converting an object to a string or number (`toString(js)` of a value not known to be a
       string, `TypeHandler<kj::String>`, numbers and their arrays and dictionaries);
     - JSON (`serializeJson()`, `jsg::deepClone()`, `V8Ref::deepClone()`,
       `CfProperty::deepClone()`: `JSON.stringify()` looks up `toJSON`);
     - `js.exceptionToKj()`, which reads the value's properties and serializes it;
     - calling a function (`jsg::Function`, `JsFunction::call()`), resolving a promise, and
       `Serializer::write()`;
     - a constructor or helper that does any of these, such as `Request::constructor()` or
       `AbortSignal::abort()`.
   - **Unknown:** a virtual call, a `kj::Function`, or a callee you cannot read. Say so in the
     review; don't assume it is safe.
3. **Check each deferral.**
   - The work runs in the lambda given to `deserializer.afterRead()`, not directly.
   - The object that `deserialize()` returns is consistent before its callback runs, since
     JavaScript that an earlier callback runs can reach it. For example, an aborted
     `AbortSignal` starts with the exception of a signal aborted without a reason.
   - The lambda captures owned values (`addRef()`, `JsRef`), never references to locals or to
     the deserializer, and it does not read from the deserializer.
   - It may use the value's instances of classes implemented in JavaScript, which are
     initialized before any callback runs.
   - Anything it calls may throw. The read then fails with that exception, and the remaining
     callbacks are not called.
   - It needs no `JSG_VISITABLE_LAMBDA`: the deserializer drops it once it has been called.
4. **Recognize the accepted patterns.**
   - JavaScript work done before the read: the RPC external handlers hydrate streams and sockets
     in `prepare()`, and `deserialize()` only claims the result.
   - `WrappedBinding::deserialize()` runs its wrapper module under an explicit
     `v8::Isolate::AllowJavascriptExecutionScope` once its own input has been read, because it
     returns what the module makes. Flag any other `AllowJavascriptExecutionScope` in a
     deserializer, and ask why `afterRead()` doesn't fit.
5. **Ask for a test.** A reader that does work that can run JavaScript needs a test that
   checks that the work waits until the value has been read, and that the value reads back
   intact. `api/tests/abortsignal-test.js` has one. Add a new serializable type to
   `api/tests/deserialize-pollution-test.js`.

### Runtime-Specific Safety Notes

- **tcmalloc**: workerd uses tcmalloc. Thread-local caches reduce contention but increase per-thread
  memory overhead. Focus optimization on reducing allocation count (especially in hot paths) rather
  than individual allocation sizes. Do not suggest switching to standard malloc.
- **Cap'n Proto zero-copy**: Cap'n Proto messages are zero-copy and use arena allocation. Do not
  suggest copying data out of Cap'n Proto messages "for safety" unless there is a concrete lifetime
  issue. Suggest using Cap'n Proto's traversal limits to prevent resource exhaustion when processing
  untrusted messages.
- **V8 GC awareness**: V8 may trigger garbage collection at any allocation point. Operations that
  create V8 objects (including string flattening, ArrayBuffer creation) can trigger GC. Be aware
  of this when analyzing code that interleaves V8 allocations with raw pointer access to V8-managed
  objects.
- **Destructors may throw**: workerd follows KJ convention of `noexcept(false)` destructors. Do not
  flag this as an issue unless there is a specific exception safety problem (e.g., double-exception
  during stack unwinding).
- **Cross-platform**: workerd runs on Linux in production but builds on macOS and Windows. Flag
  platform-specific system calls or assumptions (e.g., Linux-only epoll, /proc filesystem) that
  lack portable alternatives.
