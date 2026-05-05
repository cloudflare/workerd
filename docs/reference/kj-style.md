This document provides a style guide for C++ code in the workerd project, based on the KJ style used in the Cap'n Proto project. It covers naming conventions, type usage, memory management, error handling, inheritance, constness, formatting, and other idioms. The guide emphasizes consistency and readability while allowing pragmatic exceptions when justified.

- Full guide: https://github.com/capnproto/capnproto/blob/v2/kjdoc/style-guide.md
- Full API tour: https://github.com/capnproto/capnproto/blob/v2/kjdoc/tour.md

The KJ style guide is self-described as suggestions, not rules. Consistency matters, but
pragmatic exceptions are fine.

These guidelines are split across multiple files for context efficiency.

**You MUST read the relevant reference files before writing or reviewing code that touches
their subject matter. Do not rely on memory or general knowledge — the reference files contain
project-specific patterns and idioms that override general C++ conventions. Skipping a relevant
reference file WILL lead to incorrect suggestions.**

- `detail/review-checklist.md` **must** be used when performing ANY code review of workerd C++ code
- `detail/api-patterns.md` **must** be used when code uses or should use `kj::Maybe`, `kj::OneOf`,
  `kj::str()`, `KJ_DEFER`, `KJ_SYSCALL`, `kj::downcast`, or KJ iteration helpers
- `detail/async-patterns.md` **must** be used when code involves `kj::Promise`, `.then()`,
  `.attach()`, `.eagerlyEvaluate()`, coroutines, `kj::MutexGuarded`, or `kj::TaskSet`
- `detail/type-design.md` **must** be used when designing new classes, reviewing class hierarchies,
  analyzing constness or thread safety semantics, or deciding value-type vs resource-type

When in doubt about whether a reference file is relevant, load it.

### KJ Types over STL

The project uses KJ types instead of the C++ standard library:

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


- **Always** avoid including `<string>`, `<vector>`, `<memory>`, `<optional>`, `<functional>`,
  `<variant>`, or other std headers from header files. Source-file-only use of `std::` is acceptable
  when KJ has no equivalent or when use is required when interacting with a dependency.
- **Always** use KJ types when a KJ equivalent exists, even if it requires more effort.
- **Never** use `FILE*`, `iostream`, or C stdio.
- **Always** use `kj::str()` for formatting, `KJ_DBG()` for debug printing, and `kj/io.h` for I/O.
- **Never** leave `KJ_DBG` statements in committed code.

### Memory Management

- **Never write `new` or `delete`**. Use:
  - `kj::heap<T>(args...)` → returns `kj::Own<T>`
  - `kj::heapArray<T>(size)` → returns `kj::Array<T>`
  - `kj::heapArrayBuilder<T>(size)` → build arrays element-by-element
  - `kj::rc<T>(args...)` → returns `kj::Rc<T>`
  - `kj::arc<T>(args...)` → returns `kj::Arc<T>`

- **Always** use heap alocations when you need moveability.
- **Always** prefer stack or member variables when possible and moving is not needed.
- **Always** prefer an expicit `clone()` method over copy constructors or copy assignment

### Ownership Model

- Every object has exactly **one owner** (another object or a stack frame)
- `kj::Own<T>` is an owned pointer that transfers ownership when moved
- Raw C++ pointers and references = **not owned**, borrowing only
- An object can **never own itself**, even transitively (no reference cycles)
- Default lifetime rules for raw pointer/reference parameters:
  - Constructor param → valid for lifetime of the constructed object
  - Function/method param → valid until function returns (or returned promise completes)
  - Method return → valid until the object it was called on is destroyed

**Reference counting** is allowed. If used:

- **Always** avoid cycles
- **Always** prefer non-atomic refcounting:
  - Atomic refcounting (`kj::AtomicRefcounted`) is extremely slow
  - `kj::AtomicRefcounted` is only appropriate for objects that whose refcounting needs to occur
    across threads.
- **Always** prefer using the newer `kj::Rc<T>`/`kj::rc<T>(...)` and `kj::Arc<T>`/`kj::arc<T>(...)`
  instead of `kj::refcounted<T>`, `kj::atomicRefcounted<T>`, and `kj::addRef()` directly.

### Error Handling

- **Never use `throw` directly**. Use KJ assertion macros from `kj/debug.h`:
  - `KJ_ASSERT(cond, msg, values...)` — checks invariants (bug in this code)
  - `KJ_REQUIRE(cond, msg, values...)` — checks preconditions (bug in caller)
  - `KJ_FAIL_ASSERT(msg, values...)` — unconditional failure
  - `KJ_SYSCALL(expr, msg, values...)` — wraps C syscalls, checks return values
  - `KJ_UNREACHABLE` — marks unreachable code
  - `kj::throwFatalError(msg, values...)` — throws an exception with a stack trace

  These macros automatically capture file/line, stringify operands, and generate stack traces.

- **Never declare anything `noexcept`**. Bugs can happen anywhere; aborting is never correct.
- **Always** ensure that explicit destructors use `noexcept(false)`**. Use `kj::UnwindDetector` or
  recovery blocks in destructors to handle the unwinding-during-unwind problem.

**Exceptions are for fault tolerance**, not control flow. They represent things that "should never
happen" — bugs, network failures, resource exhaustion. If callers need to catch an exception
to operate correctly, the interface is wrong. Offer alternatives like `openIfExists()` returning
`kj::Maybe`.

### RAII

**Always** prefer RAII for resource management.

Cleanup should happen in destructors. A destructor should perform no more than one cleanup action.

Use `KJ_DEFER(code)` for scope-exit cleanup.

### Naming Conventions

| Kind                          | Style                                                            |
| ----------------------------- | ---------------------------------------------------------------- |
| Types (classes, structs)      | `TitleCase`                                                      |
| Variables, functions, methods | `camelCase`                                                      |
| Constants, enumerants         | `CAPITAL_WITH_UNDERSCORES`                                       |
| Macros                        | `CAPITAL_WITH_UNDERSCORES` with project prefix (`KJ_`, `CAPNP_`) |
| Namespaces                    | `oneword` (keep short); private namespace: `_`                   |
| Files                         | `module-name.c++`, `module-name.h`, `module-name-test.c++`       |

### Lambda Capture Rules

- **Never** use `[=]` (capture-all-by-value) — makes lifetime analysis impossible during review
- **May** use `[&]` (capture-all-by-reference) but **only** when the lambda will not outlive the
  current stack frame. Using `[&]` signals "this lambda doesn't escape."
- For escaping lambdas, capture each variable explicitly.
- Use `kj::coCapture(...)` to capture variables in a way that extends their lifetime for the duration of the promise.

### No Singletons

**Never** use mutable globals or global registries. The `main()` function or high-level code should
explicitly construct components and wire dependencies via constructor parameters.

### Lazy Input Validation

**Always** validate data at time-of-use, not upfront. Upfront validation creates false confidence
downstream, gets out of sync with usage code, and wastes cycles on unused fields.

### Text Encoding

All text is UTF-8.

**Never** assume text is _valid_ UTF-8. Be tolerant of malformed sequences.

### Formatting

- **2-space indents**, never tabs
- Max 100 chars/line
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

- **Always** use `//` line comments. **Never** `/* */` block comments
- **Always** put doc comments **before** the declaration following common C++ conventions.
- **Never** state the obvious. Comments should add information not evident from the code
- TODO format: `// TODO(type): description` where type is: `now`, `soon`, `someday`, `perf`,
  `security`, `cleanup`, `port`, `test`
- `TODO(now)` comments **must** be addressed before merging.
- **May* use `TODO({project})` type comments where `{project}` is the name of an active project
  for work that is carried out over multiple pull requests.
