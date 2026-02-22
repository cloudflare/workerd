---
name: workerd-safety-review
description: Memory safety, thread safety, concurrency, and critical detection patterns for workerd code review. Covers V8/KJ boundary hazards, lifetime management, cross-thread safety, and coroutine pitfalls. Load this skill when reviewing code that touches io/, jsg/, async patterns, or V8 integration.
---

## Safety Analysis — Memory, Thread Safety & Concurrency

Load this skill when analyzing code for memory safety, thread safety, or concurrency correctness. This covers the most critical classes of bugs in workerd.

---

### Memory Safety

- Identify potential memory leaks, use-after-free, and dangling pointers or references
- Review ownership semantics and lifetime management
- Analyze smart pointer usage (`kj::Own`, `kj::Rc`, `kj::Maybe`)
- Check for proper RAII, CRTP patterns
- Look for potential buffer overflows and bounds checking issues
- Identify raw pointer usage that could be safer with owning types
- Review destructor correctness and cleanup order
- Analyze lambda captures for safety
- Consider patterns where weakrefs (see `util/weak-refs.h`) or other techniques would be safer

### Thread Safety & Concurrency

- Identify data races and race conditions
- Review lock ordering and deadlock potential
- Analyze shared state access patterns
- Check for proper synchronization primitives usage
- Review promise/async patterns for correctness
- Identify thread-unsafe code in concurrent contexts
- Analyze KJ event loop interactions
- Ensure that code does not attempt to use isolate locks across suspension points in coroutines
- Ensure that RAII objects and other types that capture raw pointers or references are not unsafely
  used across suspension points
- When reviewing V8 integration, pay particular attention to GC interactions and cleanup order
- kj I/O objects should never be held by a V8-heap object without use of `IoOwn` or `IoPtr`
  (see `io/io-own.h`) to ensure proper lifetime and thread-safety.
- Watch carefully for places where `kj::Refcounted` is used when `kj::AtomicRefcounted` is required
  for thread safety.

---

### Critical Detection Patterns

Concrete patterns to watch for. When you encounter these, flag them at the indicated severity.

Beyond these specific patterns, also watch for non-obvious complexity at V8/KJ boundaries: re-entrancy bugs where a C++ callback unexpectedly re-enters JavaScript, subtle interactions between KJ event loop scheduling and V8 GC timing, and cases where destruction order depends on runtime conditions.

**CRITICAL / HIGH:**

- **V8 callback throwing C++ exception**: A V8 callback (JSG method, property getter/setter) that can throw a C++ exception without using `liftKj` (see `jsg/util.h`). V8 callbacks must catch C++ exceptions and convert them to JS exceptions.
- **V8 heap object holding kj I/O object directly**: A `jsg::Object` subclass storing `kj::Own<T>`, `kj::Rc<T>`, `kj::Arc<T>` for an I/O-layer object without wrapping in `IoOwn` or `IoPtr` (see `io/io-own.h`). Causes lifetime and thread-safety bugs.
- **`kj::Refcounted` in cross-thread context**: A class using `kj::Refcounted` whose instances can be accessed from both the I/O thread and the JS isolate thread. Needs `kj::AtomicRefcounted`.
- **Isolate lock held across `co_await`**: Holding a `jsg::Lock`, V8 `HandleScope`, or similar V8 scope object across a coroutine suspension point. This is undefined behavior.
- **RAII object with raw pointer/reference across `co_await`**: Any RAII type or variable capturing a raw pointer or reference used across a coroutine suspension point without `kj::coCapture` to ensure correct lifetime.
- **Reference cycle between `jsg::Object` subclasses**: Two or more `jsg::Object` subclasses holding strong references to each other without GC tracing via `JSG_TRACE`. Causes memory leaks invisible to V8's GC.
- **`jsg::Object` destructor accessing another `jsg::Object`**: V8 GC destroys objects in non-deterministic order. A destructor that dereferences another GC-managed object may use-after-free.

**MEDIUM (safety-related):**

- **Broad capture in async lambda**: Lambda passed to `.then()` or stored for deferred execution using `[&]` or `[this]` when only specific members are needed. Prefer explicit captures and carefully consider captured variable lifetimes.
- **Implicit GC trigger in sensitive context**: V8 object allocations (e.g., `ArrayBuffer` backing store creation, string flattening, `v8::Object::New()`) inside hot loops or time-sensitive callbacks may trigger GC unexpectedly.

---

### Runtime-Specific Safety Notes

- **V8 GC awareness**: V8 may GC at any allocation point. Operations that create V8 objects (including string flattening, ArrayBuffer creation) can trigger GC. Be aware of this when analyzing code that interleaves V8 allocations with raw pointer access to V8-managed objects.
- **Destructors may throw**: workerd follows KJ convention of `noexcept(false)` destructors. Do not flag this as an issue unless there is a specific exception safety problem (e.g., double-exception during stack unwinding).
- **Cross-platform**: workerd runs on Linux in production but builds on macOS and Windows. Flag platform-specific system calls or assumptions (e.g., Linux-only epoll, /proc filesystem) that lack portable alternatives.
