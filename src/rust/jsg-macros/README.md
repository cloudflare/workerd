# JSG Macros

Procedural macros for the JSG (JavaScript Glue) Rust bindings. These macros eliminate
boilerplate when implementing the JSG type system for Rust-backed JavaScript APIs.

## Crate layout

| File          | Contents                                                                       |
|---------------|--------------------------------------------------------------------------------|
| `lib.rs`      | Public macro entry points — thin dispatchers only                              |
| `resource.rs` | Code generation for `#[jsg_resource]` on structs and impl blocks               |
| `trace.rs`    | GC trace code generation — field classification and `trace()` body emission    |
| `utils.rs`    | Shared helpers: `extract_named_fields`, `snake_to_camel`, `is_lock_ref`, etc.  |

---

## `#[jsg_struct]`

Generates `jsg::Struct`, `jsg::Type`, `jsg::ToJS`, and `jsg::FromJS` for a plain data
struct. Only `pub` fields are projected into the JavaScript object. Use
`#[jsg_struct(name = "MyName")]` to override the JavaScript class name.

```rust
#[jsg_struct]
pub struct CaaRecord {
    pub critical: f64,
    pub tag: String,
    pub value: String,
}

#[jsg_struct(name = "CustomName")]
pub struct MyRecord {
    pub value: String,
}
```

---

## `#[jsg_method]`

Generates a V8 `FunctionCallback` for a method on a `#[jsg_resource]` type.

- **Instance methods** (`&self` / `&mut self`) are placed on the prototype.
- **Static methods** (no receiver) are placed on the constructor.
- Return types of `Result<T, E>` automatically throw a JavaScript exception on `Err`.
- The Rust `snake_case` name is converted to `camelCase` for JavaScript; override with
  `#[jsg_method(name = "jsName")]`.
- The first typed parameter may be `&mut Lock` / `&mut jsg::Lock` to receive the
  isolate lock directly — it is not exposed as a JavaScript argument.

```rust
#[jsg_resource]
impl DnsUtil {
    // Instance — obj.parseCaaRecord(…)
    #[jsg_method]
    pub fn parse_caa_record(&self, record: String) -> Result<CaaRecord, jsg::Error> { … }

    // Instance — obj.getName()
    #[jsg_method]
    pub fn get_name(&self) -> String { … }

    // Static — DnsUtil.create(…)
    #[jsg_method]
    pub fn create(name: String) -> Result<jsg::Rc<Self>, jsg::Error> { … }
}
```

---

## `#[jsg_resource]`

Generates JSG boilerplate for a resource type and its impl block.

**On a struct** — emits `jsg::Type`, `jsg::ToJS`, `jsg::FromJS`, and
`jsg::GarbageCollected`. The `trace()` body is synthesised automatically for every
field whose type is or contains a traceable JSG handle (see [Garbage Collection](#garbage-collection) below).
Use `#[jsg_resource(name = "JSName")]` to override the JavaScript class name.

**On an impl block** — emits `jsg::Resource::members()`, registering every
`#[jsg_method]`, `#[jsg_constructor]`, and `#[jsg_static_constant]` item.

```rust
#[jsg_resource]
pub struct DnsUtil {
    cache: HashMap<String, jsg::Rc<CacheEntry>>, // traced automatically
    name: String,                                // plain data, ignored by tracer
}

#[jsg_resource]
impl DnsUtil {
    #[jsg_method]
    pub fn lookup(&self, host: String) -> Result<String, jsg::Error> { … }

    #[jsg_method]
    pub fn create() -> Self { … }
}
```

---

## `#[jsg_static_constant]`

Exposes a Rust `const` as a read-only JavaScript property on both the constructor
and its prototype (equivalent to `JSG_STATIC_CONSTANT` in C++ JSG). The name is
used as-is (no camelCase). Only numeric types are supported.

```rust
#[jsg_resource]
impl WebSocket {
    #[jsg_static_constant]
    pub const CONNECTING: i32 = 0;
    #[jsg_static_constant]
    pub const OPEN: i32 = 1;
}
// JS: WebSocket.CONNECTING === 0 / instance.OPEN === 1
```

---

## `#[jsg_constructor]`

Marks a **static** method (no `self` receiver, returns `Self`) as the JavaScript
constructor. Only one per impl block is allowed. Without it, `new MyClass()` throws
`Illegal constructor`. An optional first parameter of `&mut Lock` is passed the
isolate lock and is not counted as a JavaScript argument.

```rust
#[jsg_resource]
impl Greeting {
    #[jsg_constructor]
    fn constructor(message: String) -> Self {
        Self { message }
    }
}
// JS: let g = new Greeting("hello");
```

---

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

Generates `jsg::Type` and `jsg::FromJS` for a union enum — the Rust equivalent of
`kj::OneOf<…>`. Each variant must be a single-field tuple whose inner type implements
`jsg::Type` + `jsg::FromJS`. Variants are tried in declaration order using
exact-type matching; if none matches, a `TypeError` is thrown listing all expected
types.

```rust
#[jsg_oneof]
#[derive(Debug, Clone)]
enum StringOrNumber {
    String(String),
    Number(jsg::Number),
}

#[jsg_resource]
impl MyResource {
    #[jsg_method]
    pub fn process(&self, value: StringOrNumber) -> String {
        match value {
            StringOrNumber::String(s) => format!("string: {s}"),
            StringOrNumber::Number(n) => format!("number: {}", n.value()),
        }
    }
}
```

---

## Garbage Collection

`#[jsg_resource]` on a struct synthesises:

- `impl jsg::Traced for MyType { fn trace(&self, visitor) { ... } }`
- `impl jsg::GarbageCollected for MyType { fn memory_name(&self) -> ... }`

The generated `Traced::trace` body simply calls `Traced::trace` on **every named
field**:

```rust
jsg::Traced::trace(&self.field_a, visitor);
jsg::Traced::trace(&self.field_b, visitor);
```

This means tracing behavior is now fully trait-driven. Types with no GC edges
use no-op `Traced` impls; containers/wrappers recurse to inner values.

### Supported field shapes

| Field type | Trace behaviour |
|---|---|
| `jsg::Rc<T>` | Strong GC edge — `visitor.visit_rc` |
| `jsg::v8::Global<T>` | Dual strong/traced — `visitor.visit_global` (enables cycle collection) |
| `jsg::Weak<T>` | **Not traced** — does not keep the target alive |
| `Option<T>` / `jsg::Nullable<T>` | Delegates to `T` when present |
| `Vec<T>`, `HashMap<K,V>`, `BTreeMap<K,V>`, `HashSet<T>`, `BTreeSet<T>` | Delegates recursively to contained values |
| `Cell<T>` / `std::cell::Cell<T>` | Delegates via `as_ptr()` read (safe under single-threaded, non-reentrant GC tracing) |
| Plain data / primitives / `#[jsg_struct]` types | No-op `Traced` |
| Any other `T: Traced` | Uses `T`'s implementation |

The `Cell<…>` variants are required whenever a traced field needs to be mutated
after construction, because `Traced::trace` receives `&self`.

### Manual tracing with `custom_trace`

For cases where default field-by-field `Traced` behavior is not enough, use
`#[jsg_resource(custom_trace)]` to suppress the generated `Traced` impl and
write your own.

The macro still generates:

- `jsg::GarbageCollected` (with `memory_name`)
- `jsg::Type`
- `jsg::ToJS`
- `jsg::FromJS`

Example:

```rust
struct EventHandlers {
    on_message: Option<jsg::v8::Global<jsg::v8::Value>>,
    on_error:   Option<jsg::v8::Global<jsg::v8::Value>>,
}

impl jsg::Traced for EventHandlers {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        self.on_message.trace(visitor);
        self.on_error.trace(visitor);
    }
}

#[jsg_resource]
pub struct MySocket {
    handlers: EventHandlers,
    name: String,
}
```

```rust
use std::cell::Cell;
use std::collections::HashMap;

#[jsg_resource]
pub struct EventRouter {
    // Strong edges — all children kept alive through GC.
    handlers: HashMap<String, jsg::Rc<Handler>>,

    // Conditionally traced.
    fallback: Option<jsg::Rc<Handler>>,

    // Interior-mutable callback set after construction; dual-mode Global enables
    // cycle collection if the callback closes over this resource's own JS wrapper.
    on_error: Cell<Option<jsg::v8::Global<jsg::v8::Value>>>,

    // Weak — does not keep target alive.
    parent: jsg::Weak<EventRouter>,

    // Plain data — no-op Traced.
    name: String,
}
```

### `jsg::v8::Global<T>` and cycle collection

`jsg::v8::Global<T>` uses the same strong↔traced dual-mode as C++ `jsg::V8Ref<T>`.
While the parent resource holds at least one strong Rust `Rc`, the V8 handle stays
strong. Once all `Rc`s are dropped and only the JS wrapper keeps the resource alive,
`visit_global` downgrades the handle to a `v8::TracedReference` that cppgc can
follow — allowing back-reference cycles (e.g. a resource that stores a callback
which closes over its own JS wrapper) to be detected and collected on the next full GC.

### Custom tracing with `custom_trace`

Use `#[jsg_resource(custom_trace)]` to suppress the generated `Traced` impl and
write your own. The macro still generates `jsg::GarbageCollected` (`memory_name`),
`jsg::Type`, `jsg::ToJS`, and `jsg::FromJS`.

```rust
#[jsg_resource(custom_trace)]
pub struct DynamicResource {
    slots: Vec<Option<jsg::Rc<Handler>>>,
}

impl jsg::Traced for DynamicResource {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        for slot in &self.slots {
            if let Some(ref h) = slot {
                visitor.visit_rc(h);
            }
        }
    }
}
```

`custom_trace` can be combined with `name`: `#[jsg_resource(name = "MyName", custom_trace)]`.
