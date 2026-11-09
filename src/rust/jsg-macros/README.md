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

Generates `jsg::Struct`, `jsg::Type`, `jsg::TypeInfo`, `jsg::ToJS`, and `jsg::FromJS` for a plain data
struct. Only `pub` fields are projected into the JavaScript object. Use
`#[jsg_struct(name = "MyName")]` to override the JavaScript class name.

Like C++ JSG's `JSG_STRUCT`, converting to JavaScript follows the `fast_jsg_struct` flag: with
it, the fields are defined as data properties, an absent optional field as `undefined`, and no
JavaScript runs; without it, they are set one by one, an absent optional field is omitted, and a
setter on `Object.prototype` runs, and what it throws is passed on.

Optional fields can use `Option<T>` (`None` is absent) or `jsg::NullOrUndefined<T>`
(`Undefined` is absent). Prefer `NullOrUndefined<T>` to `Option<jsg::Nullable<T>>`
when a field must distinguish `null` from `undefined`; its `Null` state always
produces a property whose value is `null`. An absent input property becomes `None`
or `Undefined`, respectively. If all public fields are optional, a `null` or
`undefined` input is accepted as an empty dictionary.

A field that holds a JavaScript value, as a C++ `jsg::JsRef<jsg::JsValue>` field does, is a
`jsg::v8::TracedGlobal<M>`. It converts as a `Local<'s, M>` does: `TracedGlobal<Value>` takes any
value, an absent property included, and `TracedGlobal<Object>` only an object. The struct then
holds GC references, and a resource that keeps it traces the value. Once GC has visited it there,
the handle is weak and relies on that trace, so a resource that sets or takes the struct after
construction keeps it in a `jsg::Cell` or `jsg::RefCell`, which make the handle strong again
before the struct can leave; a `std::cell::Cell` or `RefCell` of it doesn't compile. See "Cells"
in `src/rust/jsg/README.md`.

`ts_root` marks a TypeScript generation root; it can be combined with `name`.
RTTI describes the public fields using their types' `TypeInfo` implementations.

The struct implements `jsg::HoldsNoGcReferences`, which lets it into a `std::cell::Cell`,
`RefCell` or `std::rc::Rc`, when every field's type does. A recursive struct, or two that
contain each other, can't derive it, since the trait solver reports the cycle as an overflow, so
`custom_holds_no_gc_references` on one struct in the cycle keeps the generated `Traced` impl and
leaves the marker to its author, who implements it only if no struct in the cycle holds a GC
reference.

```rust
#[jsg_struct]
pub struct CaaRecord {
    pub critical: u8,
    pub tag: String,
    pub value: String,
}

#[jsg_struct]
pub struct ProduceResult {
    pub value: jsg::v8::TracedGlobal<jsg::v8::Value>,
    pub expiration: Option<f64>,
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
  they change in `Cell` or `RefCell` fields; state that holds a `jsg::Rc` or a `TracedGlobal`
  goes in a `jsg::Cell` or `jsg::RefCell`, whose `take`, `replace` and `borrow_mut` take the lock
  and make the contents strong (see "Cells" in `src/rust/jsg/README.md`). Prefer `Cell`: a
  `RefCell` borrow held across a call that can run JavaScript makes a re-entrant call panic.
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
- The return value converts with `jsg::ToJS::to_js_with_creator`, with the resource's wrapper
  as the creator for an instance method and `None` for a static one, as C++ JSG passes
  `creator` to `wrap()`: a returned `jsg::Promise` keeps the resource alive until it settles.
- A method, property or call handler that returns a promise (`jsg::Promise<T>` or
  `v8::Local<v8::Promise>`, alone or in a `Result`) rejects it with what it throws, an argument
  conversion error included, when the isolate captures throws as rejections (the
  `capture_async_api_throws` compatibility flag), as C++ JSG does; otherwise it throws. A
  termination still propagates.
- The Rust `snake_case` name is converted to `camelCase` for JavaScript; override with
  `#[jsg_method(name = "jsName")]`.
- The first typed parameter may be `&mut Lock` / `&mut jsg::Lock` to receive the
  isolate lock directly — it is not exposed as a JavaScript argument. In a method that takes
  `this`, the lock comes right after `this`.
- `Option<T>`, `jsg::NullOrUndefined<T>`, and `jsg::Lenient<T>` arguments may be omitted.
  Prefer `NullOrUndefined<T>` to `Option<jsg::Nullable<T>>` for three distinct
  `Some(T)`, `Null`, and `Undefined` states; omission produces `Undefined`.
  A `jsg::Nullable<T>` argument remains required, although explicitly passing
  `undefined` converts to `Null`. See [nullish value conversions](../jsg/README.md#nullablet-and-nullorundefinedt).
- To take or return handles, name the callback's scope:
  `fn f<'s>(&self, lock: &mut Lock<'s>, v: Local<'s, Value>) -> Local<'s, Value>`. The
  generated callback ties `'s` to its own scope.
- A last parameter of type `jsg::Arguments<T>` takes the remaining arguments, each converted
  to `T`, like C++ JSG's `jsg::Arguments<T>`; it is empty when there are none. It must be the
  last parameter, of a method or a `#[jsg_constructor]`, and TypeScript declares it as
  `...name: any[]`. See [Rest parameters](../jsg/README.md#rest-parameters).
- When `spec_compliant_property_attributes` is enabled, the function's `.length` is the
  Web IDL count of leading required arguments: `this`, the lock and a `jsg::Arguments` are not
  counted, and counting stops at the first type whose `FromJS::is_optional()` returns true (such
  as `Option<T>` or `jsg::Lenient<T>`). Otherwise `.length` is `0`.

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
        watcher.targets.borrow_mut(lock).push(this); // targets: jsg::RefCell<Vec<jsg::Rc<Self>>>
    }

    // Static — DnsUtil.create(…)
    #[jsg_method]
    pub fn create(lock: &mut Lock, name: String) -> Result<jsg::Rc<Self>, jsg::Error> { … }
}
```

Eligible methods also get a V8 Fast API callback; see "Fast API" in `src/rust/jsg/README.md`.

---

## `#[jsg_resource]`

Generates JSG boilerplate for a resource type and its impl block.

**On a struct** — emits `jsg::Type`, `jsg::TypeInfo`, `jsg::ToJS`, `jsg::FromJS`, and
`jsg::GarbageCollected`. The `trace()` body is synthesised automatically for every
field whose type is or contains a traceable JSG handle (see [Garbage Collection](#garbage-collection) below).
Use `#[jsg_resource(name = "JSName")]` to override the JavaScript class name.
The optional `ts_root` argument marks a TypeScript generation root.

**On an impl block** — emits `jsg::Resource::members(registry, flags)`, registering every
`#[jsg_method]`, `#[jsg_constructor]`, `#[jsg_property]`, `#[jsg_inspect_property]`,
`#[jsg_wildcard_property]`, `#[jsg_dispose]`, `#[jsg_async_dispose]`, `#[jsg_callable]`, and `#[jsg_static_constant]` item. It also generates member factories used by both automatic
and custom registration.
Use `#[jsg_resource(nested_types(Foo, Name = Bar))]` to expose other resource types'
constructors on the prototype (equivalent to `JSG_NESTED_TYPE(Foo)` and
`JSG_NESTED_TYPE_NAMED(Bar, Name)` in C++ JSG).
Each factory also describes its member for RTTI, which describes a resource type with the
members its `Resource::members` registers, in registration order. Methods, properties,
constructors, and constants are described; inspect and wildcard properties and disposers are not. Parameter
names are retained, injected `this` and lock parameters are excluded, and fallible return types
describe their success value. See [RTTI](../jsg/README.md#rtti-and-typescript-declarations).

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
factories without an automatic `Resource` implementation. Implement
`Resource::members(registry, flags)` to choose which members to expose, their order, and property
placement using ordinary Rust, adding each to `registry`. The flags belong to the realm
constructing the template, as in C++ `JSG_RESOURCE_TYPE(T, flags)`. Templates are cached per
resource type within that realm; registration runs when a template is created, not on each
property access.

RTTI export runs the same function with its own registry and the flags of the export, so the
TypeScript declarations follow the same choices, like the `Registry` parameter of C++
`registerMembers`. Template creation never runs the type descriptions, and RTTI export never
builds callbacks. A `jsg::Member` built by hand carries no type description, so it is added as
`jsg::undescribed(member)`, which RTTI omits from the TypeScript declarations.

```rust
use jsg::{MemberRegistry, PropertyPlacement, Resource};
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
    fn members(registry: &mut impl MemberRegistry, flags: compatibility_flags::Reader<'_>) {
        let placement = if flags.get_jsg_property_on_prototype_template() {
            PropertyPlacement::Prototype
        } else {
            PropertyPlacement::Instance
        };
        registry.add(Self::constructor_member());
        registry.add(Self::get_label_member(placement));
        if flags.get_cache_option_enabled() && flags.get_jsg_property_on_prototype_template() {
            registry.add(Self::get_cache_member(PropertyPlacement::Prototype));
        }
    }
}
```

Factories are private associated functions returning `impl jsg::MemberDefinition`. Their names derive from
the **Rust declaration**, independently of any JavaScript `name = "..."` override:

| Annotated declaration | Factory |
| --- | --- |
| Method or constructor `foo` | `Self::foo_member()` |
| Constant `MAX_SIZE` | `Self::max_size_member()` (constant name lowercased) |
| Property getter `get_value` with an optional paired setter | `Self::get_value_member(placement)` |
| Inspect getter `debug_info` | `Self::debug_info_member()` |
| Wildcard property `get_var` | `Self::get_var_member()` |
| Disposer `close` (`#[jsg_dispose]`) | `Self::close_dispose_member()` |
| Async disposer `close` (`#[jsg_async_dispose]`) | `Self::close_async_dispose_member()` |
| Callable `call` | `Self::call_callable_member()` |

An ordinary property factory accepts `jsg::PropertyPlacement::{Prototype, Instance, LazyInstance}`
and selects the matching V8 getter and setter callbacks. The attribute's `prototype` / `instance` /
`lazy_instance` argument only supplies placement for **automatic** registration; custom registration passes it explicitly.
Getter/setter pairing, `readonly` checks, name overrides, argument conversion, and exception
handling apply in both modes. Getter and setter annotations must agree on placement and JS name.
Inspect properties remain symbol-keyed and do not take a placement argument.
`LazyInstance` keeps the getter's `readonly` (see `lazy_instance` below); asking for it for a
property with a setter panics when the template is created.
Properties are registered in the order they are added to `registry`. V8 enumerates instance native
data properties in reverse registration order, as it does for C++ JSG.
`nested_types(...)` cannot be combined with `custom_members`; a custom `members` registers nested
types itself, as `jsg::nested_type::<Foo>(name)`.

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
`new` (`MyClass()`, `MyClass.call(obj)`) throws a `TypeError`, as in C++ JSG. Parameter
conversion and `.length` follow the [`#[jsg_method]`](#jsg_method) rules.

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
struct Counter { value: Cell<u32>, id: RefCell<String>, kind: String }

#[jsg_resource]
impl Counter {
    // Prototype — read/write (setter detected from `set_` prefix)
    #[jsg_property(prototype)]
    pub fn get_value(&self) -> u32 { self.value.get() }

    #[jsg_property(prototype)]
    pub fn set_value(&self, v: u32) { self.value.set(v); }

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

## `#[jsg_dispose]` and `#[jsg_async_dispose]`

Registers a method as `Symbol.dispose` (for `using`) or `Symbol.asyncDispose` (for
`await using`) of the prototype of a `#[jsg_resource]` type. Equivalent to C++ `JSG_DISPOSE` and
`JSG_ASYNC_DISPOSE`. The method takes `&self` or `this: jsg::Rc<Self>` and is called like a
`#[jsg_method]`; with `#[jsg_method]` on the same function it is also exposed under its own name.
An async disposer should return `jsg::Promise<'s, T>` or `jsg::v8::Local<'s, jsg::v8::Promise>`,
optionally in a `Result`. The attributes take no arguments, and a disposer cannot also be a
property, inspect property or wildcard property. A type has at most one disposer of each kind,
sync and async, and one method cannot be both; with automatic registration a second of a kind is
a compile error, and a custom `members` list that includes two of a kind panics when the template
is created.

A disposer must be idempotent and safe to re-enter. JavaScript can call it directly any number of
times, and from inside itself when it runs JavaScript: mark the resource disposed before calling
out, and hold no `RefCell` borrow across the call. See
[Explicit resource management](../jsg/README.md#explicit-resource-management) for how to write one.

```rust
use jsg_macros::{jsg_dispose, jsg_method, jsg_resource};

#[jsg_resource]
struct Connection { closed: std::cell::Cell<bool> }

#[jsg_resource]
impl Connection {
    #[jsg_method]
    #[jsg_dispose]
    pub fn close(&self) { self.closed.set(true); }
}
// JS: { using conn = connect(); /* ... */ }  // conn.close() runs here
```

## `#[jsg_callable]`

Makes the instances of a `#[jsg_resource]` type callable as functions, running the method.
Equivalent to C++ `JSG_CALLABLE`.

The method takes `&self`, or `this: jsg::Rc<Self>` in its place, and otherwise has the shape of
a `#[jsg_method]`. The attribute takes no arguments. A type is callable at most one way; with
automatic registration a second `#[jsg_callable]` is a compile error, and a custom `members` list
that includes two fails at runtime. The macro generates `<name>_callable_callback()` and, through
`#[jsg_resource]`, the factory `<name>_callable_member()`, which differ from a `#[jsg_method]`'s,
so a method may also be a `#[jsg_method]`. See [Callable resources](../jsg/README.md#callable-resources)
for how they behave.

```rust
use jsg_macros::{jsg_callable, jsg_resource};

#[jsg_resource]
struct Greeter { greeting: String }

#[jsg_resource]
impl Greeter {
    #[jsg_callable]
    pub fn greet(&self, name: String) -> String { format!("{}, {name}", self.greeting) }
}
// JS: greeter('world')   // "Hello, world"
//     typeof greeter     // "function"
```

## `#[jsg_oneof]`

Generates `jsg::Type`, `jsg::FromJS`, `jsg::Traced` and `jsg::TypeInfo` for a union enum —
the Rust equivalent of `kj::OneOf<…>` — and `jsg::ToJS` when every variant implements it.
Each variant must be a single-field tuple whose inner type converts from JavaScript and is
`jsg::Type` and `jsg::TypeInfo`; RTTI describes the union as its variants' types. A union that holds handles has one lifetime parameter,
their scope, and no `jsg::Traced` that can be used; a union has no other generic
parameters. Like a `#[jsg_struct]`, a union implements `jsg::HoldsNoGcReferences` when every
variant does, and `custom_holds_no_gc_references` leaves it to the author. A value converts as it
would to the equivalent `kj::OneOf`: variants are
tried in passes that leave coercion for last, in declaration order within a pass. If no
variant converts the value, a `TypeError` is thrown naming the expected types, such as
`Expected string or number but got symbol`. Like `kj::OneOf`, a union that breaks Web
IDL's rules for unions, such as one with two string types, doesn't compile. See
[Union Types](../jsg/README.md#union-types).

```rust
#[jsg_oneof]
#[derive(Debug, Clone)]
enum StringOrNumber {
    String(String),
    Number(f64),
}

#[jsg_resource]
impl MyResource {
    #[jsg_method]
    pub fn process(&self, value: StringOrNumber) -> String {
        match value {
            StringOrNumber::String(s) => format!("string: {s}"),
            StringOrNumber::Number(n) => format!("number: {n}"),
        }
    }
}
```

---

## Garbage Collection

`#[jsg_resource]` on a struct synthesises:

- `impl jsg::Traced for MyType { fn trace(&self, visitor) { ... } }`
- `impl jsg::GarbageCollected for MyType { fn memory_name(&self) -> ... }`, which with
  `#[jsg_resource(memory_info)]` or `#[jsg_resource(custom_memory_info)]` also reports the
  fields to heap snapshots (see [Heap Snapshots](#heap-snapshots))
- with `#[jsg_resource(memory_info)]`, `impl jsg::VisitForMemoryInfo for MyType`, which
  reports every named field not marked `#[jsg_memory_info(skip)]`

The generated `Traced::trace` body simply calls `Traced::trace` on **every named
field**:

```rust
jsg::Traced::trace(&self.field_a, visitor);
jsg::Traced::trace(&self.field_b, visitor);
```

This means tracing behavior is now fully trait-driven. Types with no GC edges
use no-op `Traced` impls (`jsg::jsg_traced!`); containers/wrappers recurse to inner values.
`Traced::trace` has no default, so a hand-written impl must spell out its visits.

### Supported field shapes

| Field type | Trace behaviour |
|---|---|
| `jsg::Rc<T>` | Strong GC edge — `visitor.visit_rc` |
| `jsg::v8::TracedGlobal<T>` | Dual strong/traced — `visitor.visit_traced_global` (enables cycle collection) |
| `jsg::Weak<T>` | **Not traced** — does not keep the target alive |
| `jsg::v8::Global<T>` | **Not allowed** — no `Traced` impl; use `jsg::v8::TracedGlobal<T>` |
| `Option<T>` / `jsg::Nullable<T>` / `jsg::NullOrUndefined<T>` | Delegates to `T` when `Some` |
| `jsg::Lenient<T>` | Traces `T` when `Some`, or the original JS value when `Unconvertable` |
| `Vec<T>`, `jsg::Dict<V>`, `HashMap<K,V>`, `BTreeMap<K,V>`, `HashSet<T>`, `BTreeSet<T>` | Delegates recursively to contained values |
| `std::cell::Cell<T>` / `std::cell::RefCell<T>` | Only for `T: jsg::HoldsNoGcReferences`: a `jsg::Rc` or a `TracedGlobal` inside is a compile error |
| `jsg::Cell<T>` / `jsg::RefCell<T>` | Delegates to `T`, for any `T: Traced` (a `Cell` through an `as_ptr()` read, safe because visits change nothing other than through the visitor and `replace` moves the value out before detaching it; a `RefCell` through `try_borrow()`, skipping a mutably borrowed cell, whose contents are strong); `take`, `replace` and `borrow_mut` make strong what leaves the cell (see "Cells" in `src/rust/jsg/README.md`) |
| Plain data / primitives | No-op `Traced` |
| `#[jsg_struct]` types | Delegates to each field, like a resource; implements `jsg::HoldsNoGcReferences` when every field does |
| Any other `T: Traced` | Uses `T`'s implementation |

A cell is required whenever a traced field needs to be mutated after construction,
because `Traced::trace` receives `&self`. A field that holds a `jsg::Rc` or a
`TracedGlobal` uses the `jsg::` cells.

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
use std::collections::HashMap;

#[jsg_resource]
pub struct EventRouter {
    // Strong edges — all children kept alive through GC.
    handlers: HashMap<String, jsg::Rc<Handler>>,

    // Conditionally traced.
    fallback: Option<jsg::Rc<Handler>>,

    // Interior-mutable callback set after construction; dual-mode TracedGlobal enables
    // cycle collection if the callback closes over this resource's own JS wrapper.
    on_error: jsg::Cell<Option<jsg::v8::TracedGlobal<jsg::v8::Value>>>,

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

## Heap Snapshots

Every resource is a node in a V8 heap snapshot, as a C++ resource is: it is named after the
struct (`workerd / MyType`), sized as the struct plus its C++ `Wrappable`, and linked to its
JavaScript wrapper. To report its fields as well, as C++ `visitForMemoryInfo` does, add
`memory_info` to the struct's `#[jsg_resource]`. It generates `jsg::VisitForMemoryInfo` from the
named fields, as `Traced` is generated: each field is reported under its name, so a field added
later is reported too, and a field whose type has no `jsg::MemoryInfo` does not compile.
`#[jsg_memory_info(skip)]` leaves a field out. Without `memory_info` a resource reports no fields.

```rust
#[jsg_resource(memory_info)]
pub struct EventRouter {
    handlers: HashMap<String, jsg::Rc<Handler>>,
    on_error: jsg::RefCell<Option<jsg::v8::TracedGlobal<jsg::v8::Value>>>,
    name: String,
    #[jsg_memory_info(skip)]
    scratch: SomeTypeWithoutMemoryInfo,
}
```

To write the impl yourself, for example to report a field under another name or a value that is
not a field, use `custom_memory_info` instead and implement `jsg::VisitForMemoryInfo`; without
the impl the resource does not compile:

```rust
#[jsg_resource(custom_memory_info)]
pub struct EventRouter {
    handlers: HashMap<String, jsg::Rc<Handler>>,
}

impl jsg::VisitForMemoryInfo for EventRouter {
    fn visit_for_memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
        tracker.track_field(c"handlers", &self.handlers);
    }
}
```

`tracker.track_field` takes any `jsg::MemoryInfo`, the counterpart of C++
`MemoryTracker::trackField`'s overloads: strings and collections are nodes of the memory they
allocate, a `jsg::Rc` is an edge to the resource's node, a `TracedGlobal` an edge into the V8 heap,
and a helper type that implements `jsg::MemoryRetainer` (C++ `JSG_MEMORY_INFO`) is a node of its
own. Edge and node names are `c"..."` literals, since V8 reads them after the tracker's callback
returns. See `src/rust/jsg/memory.rs` for the full list.

A helper type registers its `MemoryRetainer` impl with `jsg::jsg_memory_retainer!`, which
implements `jsg::MemoryInfo` for it, so that `track_field` takes it:

```rust
struct Cache {
    entries: HashMap<String, String>,
}

impl jsg::MemoryRetainer for Cache {
    fn memory_name(&self) -> &'static CStr {
        c"Cache"
    }

    fn memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
        tracker.track_field(c"entries", &self.entries);
    }
}

jsg::jsg_memory_retainer!(Cache);
```

A retainer held as a field of a resource without `custom_trace` is traced too, so it also needs a
`Traced` impl: `jsg::jsg_traced!(Cache);` for one that holds no GC references.

A retainer's node counts its own bytes, so a collection of retainers counts only its spare
capacity. A retainer reached twice is one node, and so is an allocation that several
`std::rc::Rc`s or `std::sync::Arc`s share, which also ends a cycle through them. Nodes nest at most
`jsg::MemoryTracker::MAX_DEPTH` deep: a node that would be that deep is a leaf with nothing inside
it, or an edge to a resource's JavaScript wrapper, which bounds any long chain, and which is not
deduplicated, so that a shorter path to the same value still reports it in full. A retainer type
is `'static`.

The reporting code (`visit_for_memory_info`, `memory_info` and any `MemoryInfo` impl) follows the
rules in `jsg::MemoryInfo`'s docs:

- It must not panic. It runs in a callback from C++, where a panic aborts the process.
- It must not write into anything it can reach. A `jsg::Cell<T>` is reported in place, and the
  `set`, `replace` and `take` of any `jsg::Cell` on the thread panic while it is; a `RefCell` is
  borrowed while its value is reported, so a `borrow_mut` panics; either aborts the process.
  `MemoryInfo for std::cell::Cell<T>` exists only for `T: Copy` and reports nothing, as plain
  data does; hold a retainer in a `jsg::Cell` instead.
- It must not call into JavaScript.

`memory_info` or `custom_memory_info` (not both) can be combined with `name`, `custom_trace` and
`ts_root`. Any other flag on the struct, or one given twice, is a compile error, so a misspelled
`memory_info` is not silently off.
