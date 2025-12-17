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

Resources exposed to JavaScript are managed by V8's cppgc (Oilpan) garbage collector with full support for cycle collection.

### Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        cppgc Heap                               │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │ RustResource (C++ GarbageCollected object)               │  │
│  │  ├─ data[2]: fat pointer to dyn GarbageCollected         │  │
│  │  └─ [AdditionalBytes]: Instance<R>                       │  │
│  │       ├─ resource: R (user's Rust struct)                │  │
│  │       ├─ wrapper: Option<TracedReference<Object>>        │  │
│  │       └─ strong_refcount: Cell<u32>                      │  │
│  └──────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
         ▲                              ▲
         │                              │
    ┌────┴────┐                   ┌─────┴─────┐
    │ Handle  │                   │  Member   │
    │(Persist)│                   │ (traced)  │
    └────┬────┘                   └─────┬─────┘
         │                              │
         └──────────┬───────────────────┘
                    │
              ┌─────┴─────┐
              │  Ref<R>   │  ← dynamically switches between Handle/Member
              │ (storage) │
              └───────────┘
```

### RustResource: The Bridge Between C++ and Rust

cppgc can only trace C++ classes that inherit from `cppgc::GarbageCollected`. `RustResource` acts as a bridge: cppgc sees a normal C++ GC object, but the actual data is a Rust object stored in cppgc's `AdditionalBytes` region.

```cpp
// C++ side (ffi.h)
class RustResource: public cppgc::GarbageCollected<RustResource> {
public:
    ~RustResource();                    // Calls Rust drop
    void Trace(cppgc::Visitor*) const;  // Calls Rust trace
    uintptr_t data[2];                  // Fat pointer: [data_ptr, vtable_ptr]
};
```

The `data[2]` field stores a Rust fat pointer (`dyn GarbageCollected`) enabling:
- **Tracing**: When cppgc traces the object, it invokes `GarbageCollected::trace()` through the vtable
- **Destruction**: When cppgc collects the object, the destructor calls Rust's `drop()`

### Instance<R>: The Rust Wrapper

Each resource is wrapped in `Instance<R>` which lives in the `AdditionalBytes` region:

```rust
struct Instance<R: Resource> {
    resource: R,                                    // User's data
    wrapper: Option<TracedReference<v8::Object>>,   // JS wrapper (if wrapped)
    strong_refcount: Cell<u32>,                     // Active Ref<R> count
}
```

### How Allocation Works

```rust
let resource = MyResource::alloc(&mut lock, MyResource { ... });
```

1. `cppgc::MakeGarbageCollected` allocates a `RustResource` with extra bytes for `Instance<R>`
2. The Rust object is written into the `AdditionalBytes` region
3. A fat pointer to `dyn GarbageCollected` is stored in `RustResource::data[2]`
4. A `Ref<R>` with a `Handle` (strong mode) is returned

### How GC Tracing Works

When cppgc runs a GC cycle:

1. **C++ Trace**: `RustResource::Trace()` is called by cppgc
2. **FFI Bridge**: Calls `cppgc_invoke_trace()` which reads the fat pointer from `data[2]`
3. **Rust Trace**: Invokes `GarbageCollected::trace()` on the Rust object
4. **Visit Children**: The trace method visits all `Ref<T>`, `TracedReference<T>` fields

### Dynamic Mode Switching in Ref<R>

`Ref<R>` uses `RefStorage` to hold either a `Handle` (strong) or `Member` (traced):

```rust
enum RefStorage {
    Strong(Handle),   // cppgc::Persistent - prevents collection
    Traced(Member),   // cppgc::Member - traced, enables cycles
}
```

The switch happens during GC visitation based on the parent's state:
- **Switch to Traced**: When parent has a JS wrapper (reachable from JS)
- **Switch to Strong**: When parent loses JS wrapper but still has Rust refs

This enables cycle collection: when circular `Ref<R>` references use `Member` internally, cppgc can detect and collect the entire cycle when unreachable.

### How it works

1. **Allocation**: Resources are allocated on the cppgc heap via `Resource::alloc()`, returning a `Ref<R>` handle.

2. **Dynamic mode switching**: `Ref<R>` dynamically switches between two modes:
   - **Strong mode** (`cppgc::Persistent`): Used when held by Rust code outside the GC heap. Prevents collection.
   - **Traced mode** (`cppgc::Member`): Used when owned by a parent resource that has a JavaScript wrapper. Enables cycle collection.

3. **Wrapping**: When a resource is wrapped for JavaScript via `wrap()`, the wrapper is stored as a `TracedReference`. Child `Ref<R>` fields automatically switch to traced mode during GC visitation.

4. **Cycle collection**: Because traced `Ref<R>` fields use `cppgc::Member` internally, circular references between resources are properly collected when no JavaScript references remain.

5. **Tracing**: The `#[jsg_resource]` macro generates `GarbageCollected::trace()` implementations that visit all `Ref<T>`, `TracedReference<T>`, and `RefCell<Option<Ref<T>>>` fields.

### Resource Reference Types

| Type | Description |
|------|-------------|
| `Ref<R>` | Smart pointer to a resource. Dynamically switches between strong and traced modes. Derefs to `&R`. |
| `WeakRef<R>` | Weak reference that doesn't prevent collection. Use `upgrade()` to get `Option<Ref<R>>`. |

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
pub struct ParentResource {
    name: String,
    // Child resources - automatically traced
    child: Ref<ChildResource>,
    optional_child: Option<Ref<ChildResource>>,
    // JavaScript callbacks - automatically traced
    callback: Option<TracedReference<v8::Object>>,
}

// Cyclic references using RefCell
#[jsg_resource]
pub struct Node {
    name: String,
    next: RefCell<Option<Ref<Node>>>,
}

// Usage: create a cycle that will be properly collected
let node_a = Node::alloc(&mut lock, Node { name: "A".into(), next: RefCell::new(None) });
let node_b = Node::alloc(&mut lock, Node { name: "B".into(), next: RefCell::new(None) });
*node_a.next.borrow_mut() = Some(node_b.clone());
*node_b.next.borrow_mut() = Some(node_a.clone());
// When wrapped and later unreferenced from JS, both nodes will be collected
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
