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

Like C++ JSG's `JSG_STRUCT`, converting to JavaScript follows the `fast_jsg_struct` flag: with
it, the fields are defined as data properties and no JavaScript runs; without it, they are set
one by one, so a setter on `Object.prototype` runs, and what it throws is passed on.

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

- **Instance methods** (`&self`) are placed on the prototype. JavaScript can call back into
  a resource while one of its methods runs, so methods never get `&mut self`; keep state
  they change in `Cell` or `RefCell` fields. Prefer `Cell`: a `RefCell` borrow held across a
  call that can run JavaScript makes a re-entrant call panic, and tracing skips a mutably borrowed
  `RefCell`, so while a `borrow_mut()` is held across a call that can allocate on the V8 heap,
  wrap the resource or create or drop a `jsg::Rc` to it, the values of the `TracedGlobal`s in it
  and the JS wrappers of the resources its `jsg::Rc`s point to can be collected (debug builds
  abort instead).
- **Static methods** (no receiver) are placed on the constructor.
- **`this: jsg::Rc<Self>`** as the first parameter, instead of `&self`, also makes an instance
  method. `this` is a `jsg::Rc` of the resource the method was called on, like `JSG_THIS` in
  C++ JSG. Use it when the method gives the resource to something that runs after the call,
  such as a callback or a promise continuation. See "Reaching the resource from a callback" in
  `src/rust/jsg/README.md`. A method that takes `this` has no Fast API callback.
  The macro matches both the name `this` and the type, so a JavaScript argument of type
  `jsg::Rc<Self>` with another name is still an ordinary argument. A `this` parameter that
  isn't first, or has another type, is a compile error.
- Return types of `Result<T, E>` automatically throw a JavaScript exception on `Err`, and so
  does an error from converting the return value with `jsg::ToJS`.
- The Rust `snake_case` name is converted to `camelCase` for JavaScript; override with
  `#[jsg_method(name = "jsName")]`.
- The first typed parameter may be `&mut Lock` / `&mut jsg::Lock` to receive the
  isolate lock directly — it is not exposed as a JavaScript argument. In a method that takes
  `this`, the lock comes right after `this`.
- To take or return handles, name the callback's scope:
  `fn f<'s>(&self, lock: &mut Lock<'s>, v: Local<'s, Value>) -> Local<'s, Value>`. The
  generated callback ties `'s` to its own scope.

```rust
#[jsg_resource]
impl DnsUtil {
    // Instance — obj.parseCaaRecord(…)
    #[jsg_method]
    pub fn parse_caa_record(&self, record: String) -> Result<CaaRecord, jsg::Error> { … }

    // Instance — obj.getName()
    #[jsg_method]
    pub fn get_name(&self) -> String { … }

    // Instance — obj.watch(…); `this` is a jsg::Rc of `obj`'s resource
    #[jsg_method]
    pub fn watch(this: jsg::Rc<Self>, lock: &mut Lock, watcher: jsg::Rc<Watcher>) {
        watcher.targets.borrow_mut().push(this);
    }

    // Static — DnsUtil.create(…)
    #[jsg_method]
    pub fn create(name: String) -> Result<jsg::Rc<Self>, jsg::Error> { … }
}
```

Eligible methods also get a V8 Fast API callback; see "Fast API" in `src/rust/jsg/README.md`.

---

## `#[jsg_resource]`

Generates JSG boilerplate for a resource type and its impl block.

**On a struct** — emits `jsg::Type`, `jsg::ToJS`, `jsg::FromJS`, and
`jsg::GarbageCollected`. The `trace()` body is synthesised automatically for every
field whose type is or contains a traceable JSG handle (see [Garbage Collection](#garbage-collection) below).
Use `#[jsg_resource(name = "JSName")]` to override the JavaScript class name.

**On an impl block** — emits `jsg::Resource::members(flags)`, registering every
`#[jsg_method]`, `#[jsg_constructor]`, `#[jsg_property]`, `#[jsg_inspect_property]`,
`#[jsg_wildcard_property]`, and `#[jsg_static_constant]` item. It also generates member factories used by both automatic
and custom registration.
Use `#[jsg_resource(nested_types(Foo, Name = Bar))]` to expose other resource types'
constructors on the prototype (equivalent to `JSG_NESTED_TYPE(Foo)` and
`JSG_NESTED_TYPE_NAMED(Bar, Name)` in C++ JSG).

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

### Custom member registration

Use `#[jsg_resource(custom_members)]` on the impl block to generate the bindings and member
factories without an automatic `Resource` implementation. Implement `Resource::members(flags)`
to choose which members to expose, their order, and property placement using ordinary Rust.
The flags belong to the realm constructing the template, as in C++ `JSG_RESOURCE_TYPE(T, flags)`.
Templates are cached per resource type within that realm; registration runs when a template is
created, not on each property access.

```rust
use jsg::{Member, PropertyPlacement, Resource};
use jsg::feature_flags::compatibility_flags;
use jsg_macros::{jsg_constructor, jsg_property, jsg_resource};

#[jsg_resource]
struct Example {}

#[jsg_resource(custom_members)]
impl Example {
    #[jsg_constructor]
    fn constructor() -> Self { Self {} }

    #[jsg_property(readonly)]
    fn get_label(&self) -> String { "example".to_owned() }

    #[jsg_property(readonly)]
    fn get_cache(&self) -> String { "no-store".to_owned() }
}

impl Resource for Example {
    fn members(flags: compatibility_flags::Reader<'_>) -> Vec<Member> {
        let placement = if flags.get_jsg_property_on_prototype_template() {
            PropertyPlacement::Prototype
        } else {
            PropertyPlacement::Instance
        };
        let mut members = vec![
            Self::constructor_member(),
            Self::get_label_member(placement),
        ];
        if flags.get_cache_option_enabled() && flags.get_jsg_property_on_prototype_template() {
            members.push(Self::get_cache_member(PropertyPlacement::Prototype));
        }
        members
    }
}
```

Factories are private associated functions returning `jsg::Member`. Their names derive from
the **Rust declaration**, independently of any JavaScript `name = "..."` override:

| Annotated declaration | Factory |
| --- | --- |
| Method or constructor `foo` | `Self::foo_member()` |
| Constant `MAX_SIZE` | `Self::max_size_member()` (constant name lowercased) |
| Property getter `get_value` with an optional paired setter | `Self::get_value_member(placement)` |
| Inspect getter `debug_info` | `Self::debug_info_member()` |
| Wildcard property `get_var` | `Self::get_var_member()` |

An ordinary property factory accepts `jsg::PropertyPlacement::{Prototype, Instance, LazyInstance}`
and selects the matching V8 getter and setter callbacks. The attribute's `prototype` / `instance` /
`lazy_instance` argument only supplies placement for **automatic** registration; custom registration passes it explicitly.
Getter/setter pairing, `readonly` checks, name overrides, argument conversion, and exception
handling apply in both modes. Getter and setter annotations must agree on placement and JS name.
Inspect properties remain symbol-keyed and do not take a placement argument.
`LazyInstance` keeps the getter's `readonly` (see `lazy_instance` below); asking for it for a
property with a setter panics when the template is created.
Property registration follows the returned member list's order. V8 enumerates instance native
data properties in reverse registration order, as it does for C++ JSG.
`nested_types(...)` cannot be combined with `custom_members`; a custom `members` lists nested
types itself, as `jsg::Member::NestedType { name, nested_type: jsg::NestedType::of::<Foo>() }`.

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

Marks a **static** method (no `self` receiver, returns `Self` or `Result<Self, E>`) as the
JavaScript constructor. An `Err` is converted with `Into<jsg::Error>` and thrown, as for
`#[jsg_method]`. Only one per impl block is allowed. Without it, `new MyClass()` throws
`Illegal constructor`. An optional first parameter of `&mut Lock` is passed the
isolate lock and is not counted as a JavaScript argument. Calling the constructor without
`new` (`MyClass()`, `MyClass.call(obj)`) throws a `TypeError`, as in C++ JSG.

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
is optional and must be one of `prototype`, `instance` or `lazy_instance`; it defaults to
`prototype`.

**`prototype`** — property lives on the prototype chain. Not directly enumerable (`Object.keys()`
is empty), but `"prop" in obj` is `true`. Can be overridden by subclasses. Equivalent to C++
`JSG_PROTOTYPE_PROPERTY` / `JSG_READONLY_PROTOTYPE_PROPERTY`.

**`instance`** — own data property on every instance, computed by the getter on each read (V8
`SetNativeDataProperty`). Its descriptor has `value` and `writable`, not `get`/`set`;
`Object.keys()` includes it, `hasOwnProperty()` returns `true`, not overridable by subclasses.
Equivalent to C++ `JSG_INSTANCE_PROPERTY` / `JSG_READONLY_INSTANCE_PROPERTY`.

**`lazy_instance`** — own data property on every instance whose getter runs on first access (V8
`SetLazyDataProperty`); V8 then stores the result as the property's value, so later reads return
that same value without calling the getter. A getter that throws leaves the property lazy. The
property is enumerable, configurable and writable; assigning to it before the first read stores
the assigned value without running the getter. It cannot have a setter, and `readonly` makes it
non-writable. Equivalent to C++ `JSG_LAZY_INSTANCE_PROPERTY` /
`JSG_LAZY_READONLY_INSTANCE_PROPERTY`.

> **Prefer `prototype` in almost all cases.** Own properties prevent minor-GC collection
> of the object and inhibit some V8 optimisations.

**`name = "..."`** — overrides the JS property name. Otherwise the Rust method name is converted
from `snake_case` to `camelCase` after stripping a leading `get_` or `set_` prefix.

**`readonly`** — asserts at compile time that no matching `set_*` method is also annotated with
this property. Omitting a setter already makes a `prototype` or `instance` property read-only;
`readonly` adds an explicit check. A `lazy_instance` property never has a setter, and `readonly`
is what makes its stored value read-only.

**Naming** — methods **must** start with `get_` (getter) or `set_` (setter). A method without
either prefix is a compile error.  The prefix is stripped and the remainder is converted
`snake_case` → `camelCase`, so `get_foo_bar` / `set_foo_bar` both map to `"fooBar"`.

**Setter detection** — a method whose Rust name starts with `set_` is registered as the setter.
Omitting a setter (or using `readonly`) makes a `prototype` or `instance` property read-only. A
`lazy_instance` property has no setter and is writable unless marked `readonly`. In strict mode,
assigning to a read-only property throws a `TypeError`.

**Receiver** — a getter or setter takes `&self`, or `this: jsg::Rc<Self>` as its first
parameter, like a `#[jsg_method]`. So do `#[jsg_inspect_property]` getters.

**Compat flag** — when `spec_compliant_property_attributes` is enabled, a `prototype` getter's
`.length` is set to `0`, its setter's `.length` to `1`, the getter's `.name` to `"get <name>"`,
and the setter's `.name` to `"set <name>"`, per Web IDL §3.7.6. `instance` properties have no
getter or setter functions, nor do `lazy_instance` properties.

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

    // Lazy instance — computed on first read, then stored on the instance
    #[jsg_property(lazy_instance, readonly)]
    pub fn get_tags(&self) -> Vec<String> { vec![self.kind.clone()] }
}
// JS: obj.value = 7; obj.value === 7         // prototype rw
//     obj.label                               // "counter" (read-only)
//     obj.myProp                              // "x"
//     Object.keys(obj)                        // ["tags", "tokenKind", "id"]  (own props)
//     obj.tags === obj.tags                   // true (getter ran once)
//     obj.hasOwnProperty("id")                // true
//     "use strict"; obj.tokenKind = "x"       // TypeError: read-only
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

## `#[jsg_wildcard_property]`

Registers a method as the wildcard property of a `#[jsg_resource]` type. Equivalent to C++
`JSG_WILDCARD_PROPERTY`. Reading a string-named property that neither the object nor its
prototypes have calls the method with the name; `None` means there is no such property.

The method takes `&self`, optionally `lock: &mut jsg::Lock`, and the name as a `String` or
`&str`, and returns `Option<T>` or `Result<Option<T>, E>` with `T: ToJS`. The attribute takes
no arguments. A type has at most one; with automatic registration a second is a compile error,
and a custom `members` list that includes two fails at runtime. `name in obj` and assignment
call the method as well, so it must not have side effects. See [Wildcard properties](../jsg/README.md#wildcard-properties) for how they
behave.

```rust
use jsg_macros::{jsg_resource, jsg_wildcard_property};

#[jsg_resource]
struct Env { vars: std::collections::HashMap<String, String> }

#[jsg_resource]
impl Env {
    #[jsg_wildcard_property]
    pub fn get_var(&self, name: &str) -> Option<String> { self.vars.get(name).cloned() }
}
// JS: env.HOME        // "/home/user"
//     env.MISSING     // undefined
//     'HOME' in env   // true
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
| `jsg::v8::TracedGlobal<T>` | Dual strong/traced — `visitor.visit_traced_global` (enables cycle collection) |
| `jsg::Weak<T>` | **Not traced** — does not keep the target alive |
| `jsg::v8::Global<T>` | **Not allowed** — no `Traced` impl; use `jsg::v8::TracedGlobal<T>` |
| `Option<T>` / `jsg::Nullable<T>` | Delegates to `T` when present |
| `Vec<T>`, `jsg::Dict<V>`, `HashMap<K,V>`, `BTreeMap<K,V>`, `HashSet<T>`, `BTreeSet<T>` | Delegates recursively to contained values |
| `Cell<T>` / `std::cell::Cell<T>` | Delegates via `as_ptr()` read (safe under single-threaded, non-reentrant GC tracing) |
| `RefCell<T>` | Delegates via `try_borrow()`; skipped while mutably borrowed (debug builds abort if `T` can hold GC references) |
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
    on_message: Option<jsg::v8::TracedGlobal<jsg::v8::Value>>,
    on_error:   Option<jsg::v8::TracedGlobal<jsg::v8::Value>>,
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

    // Interior-mutable callback set after construction; dual-mode TracedGlobal enables
    // cycle collection if the callback closes over this resource's own JS wrapper.
    on_error: Cell<Option<jsg::v8::TracedGlobal<jsg::v8::Value>>>,

    // Weak — does not keep target alive.
    parent: jsg::Weak<EventRouter>,

    // Plain data — no-op Traced.
    name: String,
}
```

### `jsg::v8::TracedGlobal<T>` and cycle collection

`jsg::v8::TracedGlobal<T>` uses the same strong↔traced dual-mode as C++ `jsg::V8Ref<T>`.
While the parent resource holds at least one strong Rust `Rc`, the V8 handle stays
strong. Once all `Rc`s are dropped and only the JS wrapper keeps the resource alive,
`visit_traced_global` downgrades the handle to a `v8::TracedReference` that cppgc can
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
