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

Parameters and return values are handled via the `jsg::Wrappable` trait. Any type implementing `Wrappable` can be used as a parameter or return value:

- `Option<T>` - accepts `T` or `undefined`, rejects `null`
- `Nullable<T>` - accepts `T`, `null`, or `undefined`
- `NonCoercible<T>` - rejects values that would require JavaScript coercion

```rust
impl DnsUtil {
    #[jsg_method(name = "parseCaaRecord")]
    pub fn parse_caa_record(&self, record: String) -> Result<CaaRecord, DnsParserError> {
        // Errors are thrown as JavaScript exceptions
    }

    #[jsg_method]
    pub fn get_name(&self) -> String {
        self.name.clone()
    }

    #[jsg_method]
    pub fn reset(&self) {
        // Void methods return undefined in JavaScript
    }
}
```

## `#[jsg_resource]`

Generates boilerplate for JSG resources. Applied to both struct definitions and impl blocks. Automatically implements `jsg::Type::class_name()` using the struct name, or a custom name if provided via the `name` parameter.

```rust
#[jsg_resource]
pub struct DnsUtil {
    pub _unused: u8,
}

#[jsg_resource(name = "CustomUtil")]
pub struct MyUtil {
    pub value: u32,
}

#[jsg_resource]
impl DnsUtil {
    #[jsg_method]
    pub fn parse_caa_record(&self, record: String) -> Result<CaaRecord, DnsParserError> {
        // implementation
    }
}
```

On struct definitions, generates:
- `jsg::Type` implementation
- `jsg::GarbageCollected` implementation (default, no-op trace)
- Wrapper struct and `ResourceTemplate` implementations

On impl blocks, scans for `#[jsg_method]` attributes and generates the `Resource` trait implementation.

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

Resources are automatically integrated with V8's cppgc garbage collector. The macro automatically generates a `GarbageCollected` implementation that traces fields requiring GC integration:

- `Ref<T>` fields - traces the underlying resource
- `TracedReference<T>` fields - traces the JavaScript handle
- `Option<Ref<T>>` and `Option<TracedReference<T>>` - conditionally traces
- `RefCell<Option<Ref<T>>>` - supports cyclic references through interior mutability

```rust
#[jsg_resource]
pub struct MyResource {
    // Automatically traced
    js_callback: Option<TracedReference<Object>>,
    child_resource: Option<Ref<ChildResource>>,

    // Not traced (plain data)
    name: String,
}

// Cyclic references using RefCell
#[jsg_resource]
pub struct Node {
    name: String,
    next: RefCell<Option<Ref<Node>>>,
}
```

For complex cases or custom tracing logic, you can manually implement `GarbageCollected` without using the jsg_resource macro:

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
