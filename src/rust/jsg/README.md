# JSG (JavaScript Glue) Rust Bindings

Rust bindings for the JSG (JavaScript Glue) layer, enabling Rust code to integrate with workerd's JavaScript runtime.

## Core Types

### `Isolate`

A safe wrapper around V8's `v8::Isolate*` pointer. This type ensures the pointer is always non-null and provides methods to interact with the V8 isolate.

```rust
// Creating from a raw pointer (unsafe - pointer must be valid and non-null)
let isolate = unsafe { v8::Isolate::from_raw(raw_ptr) };

// Getting the raw pointer for FFI calls
let ptr = isolate.as_ptr();

// Checking if the isolate is locked by the current thread
if isolate.is_locked() {
    // Safe to perform V8 operations
}
```

### `Lock`

Provides access to V8 operations within an isolate lock. Passed to resource methods and callbacks to perform V8 operations.

```rust
impl Lock {
    // Create from an Isolate
    pub fn from_isolate(isolate: v8::Isolate) -> Self;

    // Get the underlying isolate
    pub fn isolate(&self) -> v8::Isolate;

    // Create a new JavaScript object
    pub fn new_object<'a>(&mut self) -> v8::Local<'a, v8::Object>;
}
```

### `Realm`

Per-isolate state for Rust resources exposed to JavaScript. Stores cached function templates and tracks resource instances.

## Garbage Collection with cppgc (Oilpan)

Resources exposed to JavaScript are managed by V8's cppgc (Oilpan) garbage collector. This integration follows the pattern used by Deno and other V8 embedders.

### How it works

1. **Wrapping**: When a Rust resource is wrapped for JavaScript via `wrap()`, it's allocated on the cppgc heap as a `RustResource` object. A `cppgc::Persistent` handle keeps it alive while Rust holds `Ref<R>` references.

2. **Reference counting**: `Ref<R>` handles track Rust-side references. While any `Ref<R>` exists, the cppgc persistent keeps the resource alive.

3. **Release**: When the last `Ref<R>` is dropped, the persistent handle is released, allowing cppgc to garbage collect the resource.

4. **Tracing**: Resources can implement `GarbageCollected::trace()` to trace nested JavaScript handles, ensuring proper GC integration.

### cppgc Types

The `v8::cppgc` module provides Rust wrappers for cppgc's reference types:

| Type | Description | Needs Tracing |
|------|-------------|---------------|
| `Handle` | Strong off-heap → on-heap reference (`cppgc::Persistent`) | No |
| `WeakHandle` | Weak off-heap → on-heap reference (`cppgc::WeakPersistent`) | No |
| `Member` | Strong on-heap → on-heap reference (`cppgc::Member`) | Yes |
| `WeakMember` | Weak on-heap → on-heap reference (`cppgc::WeakMember`) | Yes |

### Example

```rust
#[jsg_resource]
pub struct MyResource {
    data: String,
    // TracedReference fields are automatically traced by the macro
    callback: Option<TracedReference<v8::Object>>,
}

// For custom tracing logic, manually implement GarbageCollected:
impl jsg::GarbageCollected for MyResource {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        // Trace any nested TracedReference handles here
        if let Some(ref callback) = self.callback {
            visitor.trace(callback);
        }
    }
}
```

## V8 Handle Types

### `Local<'a, T>`

A stack-allocated handle to a V8 value. The lifetime `'a` is tied to the `HandleScope` that created it.

```rust
// Creating locals
let str_value = "hello".to_local(&mut lock);
let num_value = 42u32.to_local(&mut lock);

// Converting to global
let global = local.to_global(&mut lock);
```

### `Global<T>`

A persistent handle that outlives `HandleScope`s. Must be explicitly managed.

### `TracedReference<T>`

A reference traced by the garbage collector. Used for storing V8 handles in cppgc-managed objects.

**Note**: `TracedReference` intentionally does NOT implement `Drop`. V8 manages the lifecycle of traced handles automatically during GC. Calling `reset()` during cppgc finalization would cause use-after-free.

## FFI Functions with Raw Pointers

Functions exposed to C++ via FFI that receive raw pointers must be marked as `unsafe fn`. The `unsafe` keyword indicates to callers that the function deals with raw pointers and requires careful handling.

```rust
pub unsafe fn realm_create(isolate: *mut v8::ffi::Isolate) -> Box<Realm> {
    // implementation
}
```

For more information on unsafe Rust and raw pointers, see the [Rust Book: Unsafe Superpowers](https://doc.rust-lang.org/book/ch20-01-unsafe-rust.html#unsafe-superpowers).
