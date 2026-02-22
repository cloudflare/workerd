---
name: kj-style
description: KJ/workerd C++ style guidelines for code review. Covers naming, type usage, memory management, error handling, inheritance, constness, and formatting conventions. Load this skill when reviewing or writing C++ code in the workerd codebase.
---

## KJ Style Guide — Code Review Reference

Full guide: https://github.com/capnproto/capnproto/blob/v2/kjdoc/style-guide.md
Full API tour: https://github.com/capnproto/capnproto/blob/v2/kjdoc/tour.md

The KJ style guide is self-described as suggestions, not rules. Consistency matters, but
pragmatic exceptions are fine.

---

### KJ Types over STL

This is the most common review issue. The project uses KJ types instead of the C++ standard library:

| Instead of              | Use                                                 |
| ----------------------- | --------------------------------------------------- |
| `std::string`           | `kj::String` (owned) / `kj::StringPtr` (borrowed)   |
| `std::vector`           | `kj::Array<T>` (fixed) / `kj::Vector<T>` (growable) |
| `std::unique_ptr`       | `kj::Own<T>`                                        |
| `std::shared_ptr`       | `kj::Rc<T>` / `kj::Arc<T>` (thread-safe)            |
| `std::optional`         | `kj::Maybe<T>`                                      |
| `std::function`         | `kj::Function<T>`                                   |
| `std::variant`          | `kj::OneOf<T...>`                                   |
| `std::span` / array ref | `kj::ArrayPtr<T>`                                   |
| `std::exception`        | `kj::Exception`                                     |
| `std::promise`/`future` | `kj::Promise<T>` / `kj::ForkedPromise<T>`           |
| `T*` (nullable)         | `kj::Maybe<T&>`                                     |

Avoid including `<string>`, `<vector>`, `<memory>`, `<optional>`, `<functional>`, `<variant>`,
or other std headers from header files. Source-file-only use of std is acceptable when KJ has no equivalent.

Never use `FILE*`, `iostream`, or C stdio. Use `kj::str()` for formatting, `KJ_DBG()` for debug
printing, and `kj/io.h` for I/O.

`KJ_DBG` statements must never be left in committed code.

### Memory Management

**Never write `new` or `delete`**. Use:

- `kj::heap<T>(args...)` → returns `kj::Own<T>`
- `kj::heapArray<T>(size)` → returns `kj::Array<T>`
- `kj::heapArrayBuilder<T>(size)` → build arrays element-by-element

Only heap-allocate when you need moveability. Prefer stack or member variables.

If a type would need copy constructors that allocate, provide an explicit `clone()` method
and delete the copy constructor instead.

### Ownership Model

- Every object has exactly **one owner** (another object or a stack frame)
- `kj::Own<T>` = owned pointer (transfers ownership when moved)
- Raw C++ pointers and references = **not owned**, borrowing only
- An object can **never own itself**, even transitively (no reference cycles)
- Default lifetime rules for raw pointer/reference parameters:
  - Constructor param → valid for lifetime of the constructed object
  - Function/method param → valid until function returns (or returned promise completes)
  - Method return → valid until the object it was called on is destroyed

**Reference counting** (`kj::Refcounted`/`kj::addRef`) is allowed. If used:

- Avoid cycles (memory leaks)
- Prefer non-atomic refcounting; atomic refcounting (`kj::AtomicRefcounted`) is extremely slow
- `kj::AtomicRefcounted` is only appropriate for objects that whose refcounting needs to occur across threads.
- Prefer using `kj::Rc<T>`/`kj::rc<T>(...)` and `kj::Arc<T>`/`kj::arc<T>(...)` instead of using `kj::refcounted<T>`, `kj::atomicRefcounted<T>`, and `kj::addRef()` directly.

### Error Handling

**Never use `throw` directly**. Use KJ assertion macros from `kj/debug.h`:

- `KJ_ASSERT(cond, msg, values...)` — checks invariants (bug in this code)
- `KJ_REQUIRE(cond, msg, values...)` — checks preconditions (bug in caller)
- `KJ_FAIL_ASSERT(msg, values...)` — unconditional failure
- `KJ_SYSCALL(expr, msg, values...)` — wraps C syscalls, checks return values
- `KJ_UNREACHABLE` — marks unreachable code

These macros automatically capture file/line, stringify operands, and generate stack traces.

**Never declare anything `noexcept`**. Bugs can happen anywhere; aborting is never correct.

**Explicit destructors must be `noexcept(false)`**. Use `kj::UnwindDetector` or recovery blocks
in destructors to handle the unwinding-during-unwind problem.

**Exceptions are for fault tolerance**, not control flow. They represent things that "should never
happen" — bugs, network failures, resource exhaustion. If callers need to catch an exception
to operate correctly, the interface is wrong. Offer alternatives like `openIfExists()` returning
`kj::Maybe`.

### RAII

All cleanup must happen in destructors. Use `KJ_DEFER(code)` for scope-exit cleanup.
A destructor should perform **no more than one cleanup action** — use multiple members with
their own destructors so that if one throws, the others still run.

### Naming Conventions

| Kind                          | Style                                                            |
| ----------------------------- | ---------------------------------------------------------------- |
| Types (classes, structs)      | `TitleCase`                                                      |
| Variables, functions, methods | `camelCase`                                                      |
| Constants, enumerants         | `CAPITAL_WITH_UNDERSCORES`                                       |
| Macros                        | `CAPITAL_WITH_UNDERSCORES` with project prefix (`KJ_`, `CAPNP_`) |
| Namespaces                    | `oneword` (keep short); private namespace: `_`                   |
| Files                         | `module-name.c++`, `module-name.h`, `module-name-test.c++`       |

### Inheritance

- A class is either an **interface** (no data members, only pure virtual methods) or an
  **implementation** (no non-final virtual methods). Never mix — this causes fragile base class problems.
- Interfaces should **NOT** declare a destructor.
- Multiple inheritance is allowed and encouraged (typically inheriting multiple interfaces).
- Implementation inheritance is acceptable for composition without extra heap allocations.

### Constness and Thread Safety

- Treat constness as **transitive** — a const pointer to a struct means its contained pointers
  are also effectively const.
- **`const` methods must be thread-safe** for concurrent calls. Non-const methods require
  exclusive access. `kj::MutexGuarded<T>` enforces this: `.lockShared()` returns `const T&`,
  `.lockExclusive()` returns `T&`.
- Copyable classes with pointer members: declare copy constructor as `T(T& other)` (not
  `T(const T& other)`) to prevent escalating transitively-const references. Or inherit
  `kj::DisallowConstCopy`.
- Only hold locks for the minimum duration necessary to access or modify the guarded data.

#### Correctly holding locks

When using `kj::MutexGuarded<T>`, ensure the lock is correctly held for the duration of any access to
the guarded data.

For instance, do this:

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

### Two Kinds of Types

**Value types** (data):

- Copyable/movable, compared by value, can be serialized
- No virtual methods; use templates for polymorphism
- Always have move constructors

**Resource types** (objects with identity):

- Not copyable, not movable — use `kj::Own<T>` on the heap for ownership transfer
- Use `KJ_DISALLOW_COPY_AND_MOVE` to prevent accidental copying/moving
- Compared by identity, not value
- May use inheritance and virtual methods

### Lambda Capture Rules

- **Never** use `[=]` (capture-all-by-value) — makes lifetime analysis impossible during review
- **May** use `[&]` (capture-all-by-reference) but **only** when the lambda will not outlive the
  current stack frame. Using `[&]` signals "this lambda doesn't escape."
- For escaping lambdas, capture each variable explicitly.
- Use `kj::coCapture(...)` to capture variables in a way that extends their lifetime for the duration of the promise.

### No Singletons

Never use mutable globals or global registries. The `main()` function or high-level code should
explicitly construct components and wire dependencies via constructor parameters.

### Lazy Input Validation

Validate data at time-of-use, not upfront. Upfront validation creates false confidence downstream,
gets out of sync with usage code, and wastes cycles on unused fields.

### Text Encoding

All text is UTF-8. Do not assume text is _valid_ UTF-8 — be tolerant of malformed sequences
(lazy validation principle). ASCII-range bytes in UTF-8 are always single-byte, so parsers for
machine-readable formats can safely operate byte-by-byte.

### Formatting

- **2-space indents**, never tabs, max 100 chars/line
- Continuation lines: 4-space indent (or align with opening delimiter)
- Space after keywords: `if (foo)`, `for (...)`, `while (...)`
- No space after function names: `foo(bar)`
- Opening brace at end of statement; closing brace on its own line
- Always use braces for blocks, unless the entire statement fits on one line
- `public:`/`private:`/`protected:` reverse-indented by one stop from class body
- Namespace contents are NOT indented
- Strip trailing whitespace

### Comments

Workerd follows these convention over KJ's own comment style:

- **Always use `//` line comments**, never `/* */` block comments
- Doc comments go **before** the declaration following common C++ conventions.
- Don't state the obvious — comments should add information not evident from the code
- TODO format: `// TODO(type): description` where type is: `now`, `soon`, `someday`, `perf`,
  `security`, `cleanup`, `port`, `test`
- `TODO(now)` comments must be addressed before merging.
- `TODO({project})` type comments where `{project}` is the name of an active project are acceptable
  for work that is carried out over multiple pull requests.

### Idiomatic KJ API Patterns

**Unwrapping `kj::Maybe`** — always use `KJ_IF_SOME`, never dereference directly:

```cpp
// Correct:
KJ_IF_SOME(value, maybeValue) {
  use(value);  // value is a reference to the contained T
}

// Correct:
auto& value = KJ_ASSERT_NONNULL(maybeValue);  // asserts if maybeValue is none, otherwise gives T&
auto& value = KJ_REQUIRE_NONNULL(maybeValue);  // same but for preconditions
auto& value = JSG_REQUIRE_NONNULL(maybeValue, ErrorType, "message");  // same but with JavaScript exception type/message

// Wrong
auto& value = *maybeValue;      // doesn't exist
auto& value = maybeValue.value(); // not how KJ works
```

`maybe.map(fn)` and `maybe.orDefault(val)` are useful for simple transforms/fallbacks.

When moving a value out of a `kj::Maybe`, use `kj::mv()` and remember to set the `kj::Maybe` to `kj::none` to avoid dangling references:

```cpp
KJ_IF_SOME(value, maybeValue) {
  auto movedValue = kj::mv(value);  // move out of the Maybe
  maybeValue = kj::none;            // set Maybe to none to avoid dangling reference
  use(movedValue);
}
```

**Unwrapping `kj::OneOf`** — always use `KJ_SWITCH_ONEOF` / `KJ_CASE_ONEOF`:

```cpp
KJ_SWITCH_ONEOF(variant) {
  KJ_CASE_ONEOF(s, kj::String) { handleString(s); }
  KJ_CASE_ONEOF(i, int) { handleInt(i); }
}
```

**Building strings** — use `kj::str()`, never `std::to_string` or `+` concatenation:

```cpp
kj::String msg = kj::str("count: ", count, ", name: ", name);
```

Use `kj::hex(n)` for hex output. Extend with `KJ_STRINGIFY(MyType)` or a `.toString()` method.
Use `"literal"_kj` suffix for `kj::StringPtr` literals (can be `constexpr`).

**Scope-exit cleanup** — use `KJ_DEFER` or `kj::defer()`:

```cpp
KJ_DEFER(close(fd));                          // block scope
auto cleanup = kj::defer([&]() { close(fd); }); // movable, for non-block lifetimes
```

Also: `KJ_ON_SCOPE_SUCCESS(...)` and `KJ_ON_SCOPE_FAILURE(...)` for conditional cleanup.

**Syscall error checking** — use `KJ_SYSCALL`, never manual errno checks:

```cpp
int n;
KJ_SYSCALL(n = read(fd, buf, size));          // throws on error, retries EINTR
KJ_SYSCALL_HANDLE_ERRORS(fd = open(path, O_RDONLY)) {
  case ENOENT: return nullptr;                // handle specific errors
  default: KJ_FAIL_SYSCALL("open()", error);
}
```

**Downcasting** — use `kj::downcast<T>` (debug-checked), not `static_cast` or `dynamic_cast`:

```cpp
auto& derived = kj::downcast<DerivedType>(baseRef);  // asserts in debug builds
```

Use `kj::dynamicDowncastIfAvailable<T>` only for optimizations (returns null without RTTI).

**Iteration helpers**:

```cpp
for (auto i: kj::zeroTo(n)) { ... }        // 0..n-1
for (auto i: kj::range(a, b)) { ... }      // a..b-1
for (auto i: kj::indices(array)) { ... }    // 0..array.size()-1
```

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

### Other Rules

- **No global constructors**: Don't declare static/global variables with dynamic constructors.
  Global `constexpr` constants are fine.
- **No `dynamic_cast` for polymorphism**: Don't use long if/else chains casting to derived types.
  Extend the base interface instead. `dynamic_cast` is OK for optimizations or diagnostics
  (test: if it always returned null, would the code still be correct?).
- **No function/method pointers**: Use templates over functors, or `kj::Function<T>`.
- **Prefer references over pointers**: Unambiguously non-null.

---

### Code Review Checklist

When reviewing workerd C++ code, check for:

1. **STL leaking in**: `std::string`, `std::vector`, `std::optional`, `std::unique_ptr`, etc.
2. **Raw `new`/`delete`**: Should be `kj::heap<T>()` or similar
3. **`throw` statements**: Should use `KJ_ASSERT`/`KJ_REQUIRE`/`KJ_FAIL_ASSERT`
4. **`noexcept` declarations**: Should not be present
5. **Destructor missing `noexcept(false)`**: Required on all explicit destructors
6. **`[=]` lambda captures**: Never allowed
7. **Nullable raw pointers (`T*`)**: Should be `kj::Maybe<T&>`
8. **Mixed interface/implementation classes**: No data members in interfaces, no non-final virtuals in implementations
9. **Singletons or mutable globals**: Not allowed
10. **Missing `.attach()` on promises**: Objects must stay alive for the promise duration
11. **Background promise without `.eagerlyEvaluate()`**: Lazy continuations may never execute
12. **Manual errno checks**: Should use `KJ_SYSCALL` / `KJ_SYSCALL_HANDLE_ERRORS`
13. **`static_cast` for downcasting**: Should be `kj::downcast<T>` (debug-checked)
14. **`std::to_string` or `+` string concatenation**: Should be `kj::str()`
15. **`dynamic_cast` for dispatch**: Extend the interface instead
16. **`/* */` block comments**: Use `//` line comments
17. **Naming**: TitleCase types, camelCase functions/variables, CAPS constants
18. **Missing braces**: Required unless entire statement is on one line
19. **`bool` function parameter**: Prefer `enum class` or `WD_STRONG_BOOL` for clarity at call sites. E.g., `void connect(bool secure)` should be `void connect(SecureMode mode)`.
20. **Missing `[[nodiscard]]`**: Functions returning error codes, `kj::Maybe`, or success booleans that callers must check should be `[[nodiscard]]`.
21. **Promise chain where coroutine would be clearer**: Nested `.then()` chains with complex error handling that would be more readable as a coroutine with `co_await`. But avoid suggesting sweeping rewrites.
22. **Missing `constexpr` / `consteval`**: Compile-time evaluable functions or constants not marked accordingly.
23. **Reinvented utility**: Custom code duplicating functionality already in `src/workerd/util/` (e.g., custom ring buffer, small set, state machine, weak reference pattern). Check the util directory before suggesting a new abstraction.
24. **Missing `override`**: Virtual method overrides missing the `override` specifier.
25. **Direct `new`/`delete` (via `new` expression)**: Should use `kj::heap<T>()`, `kj::heapArray<T>()`, or other KJ memory utilities.
26. **Explicit `throw` statement**: Should use `KJ_ASSERT`, `KJ_REQUIRE`, `KJ_FAIL_ASSERT`, or `KJ_EXCEPTION` instead of bare `throw`.
