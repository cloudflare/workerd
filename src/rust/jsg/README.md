# JSG (JavaScript Glue) Rust Bindings

Rust bindings for the JSG (JavaScript Glue) layer, enabling Rust code to integrate with workerd's JavaScript runtime.

## Core Types

### `Lock`

Provides access to V8 operations within an isolate lock. Passed to resource methods and callbacks.

### `Ref<R>`

Strong reference to a Rust resource managed by GC. Derefs to `&R`. Cloning and dropping a `Ref` tracks strong references for the garbage collector.

### `WeakRef<R>`

Weak reference that doesn't prevent GC collection. Use `upgrade()` to get a `Ref<R>` if the resource is still alive, or `get()` for a temporary `&R`.

### `Realm`

Per-isolate state for Rust resources exposed to JavaScript. Stores cached function templates.

## Resources

Rust resources integrate with V8's garbage collector through the existing C++ `Wrappable` infrastructure — the same GC system that C++ `jsg::Ref<T>` and `jsg::Object` use.

```rust
use jsg_macros::{jsg_resource, jsg_method};

#[jsg_resource]
struct MyResource {
    name: String,
    child: jsg::Ref<OtherResource>,           // traced by GC
    maybe_child: Option<jsg::Ref<OtherResource>>, // conditionally traced
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
// Allocate on the KJ heap
let resource = MyResource::alloc(&lock, MyResource { ... });

// Wrap as a JS object (uses cached FunctionTemplate)
let js_obj = MyResource::wrap(resource.clone(), &mut lock);

// Unwrap from JS back to Rust
let r: &mut MyResource = MyResource::unwrap(&mut lock, js_val)
    .expect("not a Rust-wrapped resource");
```

### GC Behavior

- **No JS wrapper**: Dropping the last `Ref` immediately destroys the resource (no GC needed).
- **With JS wrapper**: Dropping all `Ref`s makes the wrapper eligible for V8 GC. When collected, the resource is destroyed.
- **Tracing**: `Ref<T>` fields are automatically traced during GC cycles. `WeakRef<T>` fields are not traced (they don't keep targets alive).
- **Circular references** through `Ref<T>` are **not** collected, matching C++ `jsg::Ref<T>` behavior.

## V8 Handle Types

### `Local<'a, T>`

A stack-allocated handle to a V8 value. The lifetime `'a` is tied to the `HandleScope` that created it.

```rust
let str_value = "hello".to_local(&mut lock);
let num_value = 42u32.to_local(&mut lock);
let global = local.to_global(&mut lock);
```

### `Global<T>`

A persistent handle that outlives `HandleScope`s. Must be explicitly managed.

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

This is similar to `kj::OneOf<>` in C++ JSG.

## FFI Functions with Raw Pointers

Functions exposed to C++ via FFI that receive raw pointers must be marked `unsafe fn`:

```rust
pub unsafe fn realm_create(isolate: *mut v8::ffi::Isolate) -> Box<Realm> {
    // ...
}
```
