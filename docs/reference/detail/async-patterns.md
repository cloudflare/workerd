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
