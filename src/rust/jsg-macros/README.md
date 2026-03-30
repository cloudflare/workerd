# JSG Macros

Procedural macros for JSG (JavaScript Glue) Rust bindings. These macros reduce boilerplate when implementing the JSG type system.

## `#[jsg_struct]`

Generates the `jsg::Struct` and `jsg::Type` implementations for data structures. Only public fields are exposed to JavaScript. Automatically implements `class_name()` using the struct name, or a custom name if provided via the `name` parameter.

```rust
#[jsg_struct]
pub struct CaaRecord {
    pub critical: f64,
    pub field: String,
    pub value: String,
}

#[jsg_struct(name = "CustomName")]
pub struct MyRecord {
    pub value: String,
}
```

## `#[jsg_method]`

Generates FFI callback functions for JSG resource methods. The `name` parameter is optional and defaults to converting the method name from `snake_case` to `camelCase`.

The macro automatically detects whether a method is an instance method or a static method based on the presence of a receiver (`&self` or `&mut self`):

- **Instance methods** (with `&self`/`&mut self`) are placed on the prototype, called on instances (e.g., `obj.getName()`).
- **Static methods** (without a receiver) are placed on the constructor, called on the class itself (e.g., `MyClass.create()`).

Parameters and return values are handled via the `jsg::FromJS` and `jsg::ToJS` traits. Any type implementing these traits can be used as a parameter or return value:

- `Option<T>` - accepts `T` or `undefined`, rejects `null`
- `Nullable<T>` - accepts `T`, `null`, or `undefined`
- `NonCoercible<T>` - rejects values that would require JavaScript coercion

```rust
impl DnsUtil {
    // Instance method: called as obj.parseCaaRecord(...)
    #[jsg_method(name = "parseCaaRecord")]
    pub fn parse_caa_record(&self, record: String) -> Result<CaaRecord, DnsParserError> {
        // Errors are thrown as JavaScript exceptions
    }

    // Instance method: called as obj.getName()
    #[jsg_method]
    pub fn get_name(&self) -> String {
        self.name.clone()
    }

    // Instance method: void methods return undefined in JavaScript
    #[jsg_method]
    pub fn reset(&self) {
    }

    // Static method: called as DnsUtil.create(...)
    #[jsg_method]
    pub fn create(name: String) -> Result<String, jsg::Error> {
        Ok(name)
    }
}
```

## `#[jsg_resource]`

Generates boilerplate for JSG resources. Applied to both struct definitions and impl blocks. Automatically implements `jsg::Type::class_name()` using the struct name, or a custom name if provided via the `name` parameter.

```rust
#[jsg_resource]
pub struct DnsUtil {}

#[jsg_resource(name = "CustomUtil")]
pub struct MyUtil {
    pub value: u32,
}

#[jsg_resource]
impl DnsUtil {
    #[jsg_method]
    pub fn parse_caa_record(&self, record: String) -> Result<CaaRecord, DnsParserError> {
        // Instance method on the prototype
    }

    #[jsg_method]
    pub fn create(name: String) -> Result<String, jsg::Error> {
        // Static method on the constructor (no &self)
    }
}
```

On struct definitions, generates:
- `jsg::Type` implementation
- `jsg::GarbageCollected` implementation with automatic field tracing (see below)
- Wrapper struct and `ResourceTemplate` implementations

On impl blocks, scans for `#[jsg_method]` and `#[jsg_static_constant]` attributes and generates the `Resource` trait implementation. Methods with a receiver (`&self`/`&mut self`) are registered as instance methods; methods without a receiver are registered as static methods.

## `#[jsg_static_constant]`

Exposes a Rust `const` item as a read-only static constant on both the JavaScript constructor and prototype. This is the Rust equivalent of `JSG_STATIC_CONSTANT` in C++ JSG.

The constant name is used as-is for the JavaScript property name (no camelCase conversion), matching the convention that constants are `UPPER_SNAKE_CASE` in both Rust and JavaScript. Only numeric types are supported (`i8`..`i64`, `u8`..`u64`, `f32`, `f64`).

```rust
#[jsg_resource]
impl WebSocket {
    #[jsg_static_constant]
    pub const CONNECTING: i32 = 0;

    #[jsg_static_constant]
    pub const OPEN: i32 = 1;

    #[jsg_static_constant]
    pub const CLOSING: i32 = 2;

    #[jsg_static_constant]
    pub const CLOSED: i32 = 3;
}
// In JavaScript:
//   WebSocket.CONNECTING === 0
//   WebSocket.OPEN === 1
//   new WebSocket(...).CLOSING === 2  (also on instances via prototype)
```

Per Web IDL, constants are `{writable: false, enumerable: true, configurable: false}`.

## `#[jsg_constructor]`

Marks a static method as the JavaScript constructor for a `#[jsg_resource]`. When JavaScript calls `new MyClass(args)`, V8 invokes this method, creates a `jsg::Rc<Self>`, and attaches it to the `this` object.

```rust
#[jsg_resource]
impl MyResource {
    #[jsg_constructor]
    fn constructor(name: String) -> Self {
        Self { name }
    }
}
// JS: let r = new MyResource("hello");
```

The method must be static (no `self` receiver) and must return `Self`. Only one `#[jsg_constructor]` is allowed per impl block. The first parameter may be `&mut Lock` if the constructor needs isolate access — it is not exposed as a JS argument.

If no `#[jsg_constructor]` is present, `new MyClass()` throws an `Illegal constructor` error.

## `#[jsg_property([placement,] [name = "..."] [, readonly])]`

Registers a method as a getter or setter on a `#[jsg_resource]` type. The `placement` argument
is optional and must be either `prototype` or `instance`; it defaults to `prototype`.

**`prototype`** — property lives on the prototype chain. Not directly enumerable (`Object.keys()`
is empty), but `"prop" in obj` is `true`. Can be overridden by subclasses. Equivalent to C++
`JSG_PROTOTYPE_PROPERTY` / `JSG_READONLY_PROTOTYPE_PROPERTY`.

**`instance`** — own property on every instance. `Object.keys()` includes it,
`hasOwnProperty()` returns `true`, not overridable by subclasses. Equivalent to C++
`JSG_INSTANCE_PROPERTY` / `JSG_READONLY_INSTANCE_PROPERTY`.

> **Prefer `prototype` in almost all cases.** Own-property accessors prevent minor-GC collection
> of the object and inhibit some V8 optimisations.

**`name = "..."`** — overrides the JS property name. Otherwise the Rust method name is converted
from `snake_case` to `camelCase` after stripping a leading `get_` or `set_` prefix.

**`readonly`** — asserts at compile time that no matching `set_*` method is also annotated with
this property. Omitting a setter already makes the property read-only; `readonly` adds an
explicit check.

**Naming** — methods **must** start with `get_` (getter) or `set_` (setter). A method without
either prefix is a compile error.  The prefix is stripped and the remainder is converted
`snake_case` → `camelCase`, so `get_foo_bar` / `set_foo_bar` both map to `"fooBar"`.

**Setter detection** — a method whose Rust name starts with `set_` is registered as the setter.
Omitting a setter (or using `readonly`) makes the property read-only. In
strict mode, assigning to a read-only property throws a `TypeError`.

**Compat flag** — when `spec_compliant_property_attributes` is enabled, getter `.length` is set
to `0`, setter `.length` to `1`, getter `.name` to `"get <name>"`, and setter `.name` to
`"set <name>"`, per Web IDL §3.7.6.

```rust
use std::cell::{Cell, RefCell};
use jsg_macros::{jsg_resource, jsg_property};

#[jsg_resource]
struct Counter { value: Cell<f64>, id: RefCell<String>, kind: String }

#[jsg_resource]
impl Counter {
    // Prototype — read/write (setter detected from `set_` prefix)
    #[jsg_property(prototype)]
    pub fn get_value(&self) -> jsg::Number { jsg::Number::new(self.value.get()) }

    #[jsg_property(prototype)]
    pub fn set_value(&self, v: jsg::Number) { self.value.set(v.value()); }

    // Prototype — read-only (explicit `readonly`)
    #[jsg_property(prototype, readonly)]
    pub fn get_label(&self) -> String { "counter".into() }

    // Prototype — explicit JS name override
    #[jsg_property(prototype, name = "myProp")]
    pub fn get_something(&self) -> String { "x".into() }

    // Instance — read/write own property
    #[jsg_property(instance)]
    pub fn get_id(&self) -> String { self.id.borrow().clone() }

    #[jsg_property(instance)]
    pub fn set_id(&self, v: String) { *self.id.borrow_mut() = v; }

    // Instance — read-only with name override
    #[jsg_property(instance, name = "tokenKind", readonly)]
    pub fn get_kind(&self) -> String { self.kind.clone() }
}
// JS: obj.value = 7; obj.value === 7         // prototype rw
//     obj.label                               // "counter" (read-only)
//     obj.myProp                              // "x"
//     Object.keys(obj)                        // ["id", "tokenKind"]  (instance props)
//     obj.hasOwnProperty("id")                // true
//     obj.kind                                // TypeError: read-only
```


## `#[jsg_inspect_property]`

Registers a method as an inspect property on a `#[jsg_resource]` type. Equivalent to C++ `JSG_INSPECT_PROPERTY`.

The getter is registered under a unique symbol. It is **invisible** to normal property access (string key lookup, `Object.keys()`, `getOwnPropertyNames()`), and is surfaced by `node:util`'s `inspect()` and `console.log()`. Inspect properties are always read-only — annotating a `set_*` method is a compile error.

**Naming** — `name = "..."` sets the symbol description used by `inspect()`. Otherwise the Rust method name is converted from `snake_case` to `camelCase` (no prefix stripping, since there is no setter concept).

```rust
use jsg_macros::{jsg_resource, jsg_inspect_property};

#[jsg_resource]
struct ReadableStream { state: String }

#[jsg_resource]
impl ReadableStream {
    // Shown as "[state]: 'readable'" in util.inspect() output
    #[jsg_inspect_property]
    pub fn state(&self) -> String { self.state.clone() }

    // Explicit symbol description
    #[jsg_inspect_property(name = "streamState")]
    pub fn get_debug_state(&self) -> String { format!("state={}", self.state) }
}
// JS: typeof stream.state           // "undefined" (invisible to string key lookup)
//     Object.keys(stream)           // []
//     // util.inspect(stream) shows the property via its symbol
```

## `#[jsg_oneof]`

Generates `jsg::Type` and `jsg::FromJS` implementations for union types. Use this to accept parameters that can be one of several JavaScript types.

Each enum variant should be a single-field tuple variant where the field type implements `jsg::Type` and `jsg::FromJS` (e.g., `String`, `f64`, `bool`).

```rust
use jsg_macros::jsg_oneof;

#[jsg_oneof]
#[derive(Debug, Clone)]
enum StringOrNumber {
    String(String),
    Number(f64),
}

impl MyResource {
    #[jsg_method]
    pub fn process(&self, value: StringOrNumber) -> Result<String, jsg::Error> {
        match value {
            StringOrNumber::String(s) => Ok(format!("string: {}", s)),
            StringOrNumber::Number(n) => Ok(format!("number: {}", n)),
        }
    }
}
```

The macro generates type-checking code that matches JavaScript values to enum variants without coercion. If no variant matches, a `TypeError` is thrown listing all expected types.

### Garbage Collection

Resources are automatically integrated with V8's garbage collector through the C++ `Wrappable` base class. The macro generates a `GarbageCollected` implementation that traces fields based on their type:

| Field type | Behaviour |
|---|---|
| `jsg::Rc<T>` | Strong GC edge — keeps target alive |
| `jsg::Weak<T>` | Not traced — does not keep target alive |
| `jsg::v8::Global<T>` | Dual strong/traced — enables back-reference cycle collection |
| Anything else | Not traced — plain data, ignored by tracer |

`Option<F>` and `jsg::Nullable<F>` wrappers are supported for all traced field types and are traced only when `Some`. Any traced field type may also be wrapped in `Cell<F>` (or `std::cell::Cell<F>`) for interior mutability — required when the field is set after construction, since `GarbageCollected::trace` receives `&self`.

#### `jsg::v8::Global<T>` and cycle collection

`jsg::v8::Global<T>` fields use the same strong↔traced dual-mode as C++ `jsg::V8Ref<T>`. While the parent resource has strong Rust `Rc` refs the handle stays strong; once all `Rc`s are dropped, `visit_global` downgrades the handle to a `v8::TracedReference` that cppgc can follow — allowing back-reference cycles (e.g. a resource holding a callback that captures its own JS wrapper) to be collected by the next full GC.

```rust
use std::cell::Cell;

#[jsg_resource]
pub struct MyResource {
    // Strong GC edge — keeps child alive
    child: jsg::Rc<ChildResource>,

    // Conditionally traced
    maybe_child: Option<jsg::Rc<ChildResource>>,

    // Weak — does not keep target alive
    observer: jsg::Weak<ChildResource>,

    // JS value — traced with dual-mode switching; Cell needed because
    // the callback is set after construction (trace takes &self)
    callback: Cell<Option<jsg::v8::Global<jsg::v8::Value>>>,

    // Plain data — not traced
    name: String,
}
```

For complex cases or custom tracing logic, you can manually implement `GarbageCollected` without using the `jsg_resource` macro:

```rust
pub struct CustomResource {
    data: String,
}

impl jsg::GarbageCollected for CustomResource {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        // Custom tracing logic
    }
}
```
