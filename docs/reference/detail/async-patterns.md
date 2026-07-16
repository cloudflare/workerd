Promise lifetime management, background tasks, cancellation, and mutex usage in workerd.

---

### Promise Patterns

**`.attach()` for lifetime management** — objects must outlive the promise that uses them:

```cpp
// Correct: stream stays alive until readAllText() completes
return stream->readAllText().attach(kj::mv(stream));

// Wrong: stream destroyed immediately, promise has dangling reference
auto promise = stream->readAllText();
return promise;  // stream is gone
```

**`.eagerlyEvaluate()` for background tasks** — without it, continuations are lazy
and may never run:

```cpp
promise = doWork().then([]() {
  KJ_LOG(INFO, "done");  // won't run unless someone .wait()s or .then()s
}).eagerlyEvaluate([](kj::Exception&& e) {
  KJ_LOG(ERROR, e);      // error handler is required
});
```

When using coroutines, `eagerlyEvaluate()` is implied and not needed to be called explicitly.

Use `kj::TaskSet` to manage many background tasks with a shared error handler.

**Cancellation** — destroying a `kj::Promise` cancels it immediately. No continuations
run, only destructors. Use `.attach(kj::defer(...))` for cleanup that must happen
on both completion and cancellation.

**`kj::evalNow()`** — wraps synchronous code to catch exceptions as rejected promises:

```cpp
return kj::evalNow([&]() {
  // Any throw here becomes a rejected promise, not a synchronous exception
  return doSomethingThatMightThrow();
});
```

---

### Continuation Captures

`workerd-unsafe-continuation-capture` flags lambdas passed to async sinks
(`kj/jsg::Promise::then/catch_`, `IoContext::run/addTask/awaitIo/addFunctor`,
`kj::evalLater`, ...) that capture bare references, `[this]`, or non-owning views.
Use one of these patterns:

| Site | Capture |
|---|---|
| JSG resource | `[self = JSG_THIS]` |
| `kj::Refcounted` | `[self = addRefToThis()]` |
| IoContext, in JS-lock scope (`Worker::Lock&`/`jsg::Lock&` available) | `auto& context = IoContext::current();` inside the lambda (but see caveat below) |
| IoContext, outside JS-lock scope | `[weakRef = context.getWeakRef()]` + `KJ_ASSERT_NONNULL(weakRef)` (alive by invariant) or `KJ_IF_SOME(ctx, weakRef) { ... }` (may be gone) |
| Chain feeds opaque wrapper (`oomCanceler.wrap`, `gate.lockWhile`, `.fork()`) | IILE coroutine — pass `this`/`&context` as a coroutine parameter, not a lambda capture |
| Chain is `co_await`ed / `.wait()`ed / joined from a local container | already safe; no change |

Do **not** strong-ref an IoContext (`kj::addRef(context)`) to satisfy the check —
chains owned transitively by the IoContext form a refcount cycle.

**`IoContext::current()` caveat.** Re-deriving the IoContext from inside the
lambda is safe only when the continuation is guaranteed to run under the same
IoContext that scheduled it. `jsg::Promise::then()` does **not** make that
guarantee in general: if the application returns a foreign promise that
resolves from a different context, the continuation can run with a different
IoContext active, and `IoContext::current()` will return the wrong one (or
throw if no context is active). Each use needs to be evaluated for this
possibility. When it can happen, capture the originating context's
`getWeakRef()` instead (or use `IoContext::addFunctor`) so the continuation
either runs under the correct context or fails loudly.

**IoContext WeakRc.** `context.getWeakRef()` returns a `kj::WeakRc<IoContext>`,
a non-owning handle to an `IoContext`. The `WeakRc` itself is safe to hold past
the IoContext's destruction. Upgrade it with `KJ_IF_SOME(ctx, weakRef) { ... }`
(or `KJ_ASSERT_NONNULL`/`JSG_REQUIRE_NONNULL`), which binds `ctx` to a transient
strong `kj::Rc<IoContext>` while the block runs and yields `kj::none` once the
context is gone. Test liveness alone with `weakRef != nullptr`, or get a bare
`Maybe<IoContext&>` via `weakRef.tryGet()`; make additional weak handles with
`weakRef.addRef()`. The upgraded `Rc` is only safe while it stays local to the
continuation — do not capture or store it, or you reintroduce the refcount cycle
warned about above. Use the WeakRc pattern any time a continuation may outlive
its originating context, or when the continuation runs outside a JS-lock scope
where `IoContext::current()` may not be the right context (or may not exist at
all).

For invariants the analyzer fundamentally can't see (capnp `thisCap()`,
fiber-blocked stacks, constructor-time `*this`, `CantOutliveIncomingRequest`-style
structural guarantees), add
`// NOLINTNEXTLINE(workerd-unsafe-continuation-capture)` with a one-line
justification.

---

### Mutex Patterns

`kj::MutexGuarded<T>` ties locking to access — you can't touch the data without a lock:

```cpp
// Exclusive access for modification
{
  auto lock = guarded.lockExclusive();
  lock->modify();
  // lock is released at end of scope
}

// Multiple readers ok
{
  auto shared = guarded.lockShared();
  shared->read();
  // shared lock is released at end of scope
}
```

`.wait(cond)` on a lock replaces condition variables:

```cpp
auto lock = guarded.lockExclusive();
lock.wait([](const T& val) { return val.ready; });  // releases/reacquires automatically
```

#### Correctly Holding Locks

When using `kj::MutexGuarded<T>`, ensure the lock is correctly held for the duration of any
access to the guarded data.

Do this:

```cpp
auto lock = mutexGuarded.lockExclusive();
KJ_IF_SOME(value, lock->maybeValue) {
  // Access value safely while lock is held.
}
```

Not this:

```cpp
KJ_IF_SOME(value, mutexGuarded.lockExclusive()->maybeValue) {
  // Unsafe! The lock is released before we access maybeValue.
}
```
