# JSG Macros

Procedural macros for JSG (JavaScript Glue) Rust bindings. These macros reduce boilerplate when implementing the JSG type system.

## `#[jsg_struct]`

Generates the `jsg::Struct` and `jsg::Type` implementations for data structures. Only public fields are exposed to JavaScript. Automatically implements `class_name()` using the struct name, or a custom name if provided via the `name` parameter.

```rust
#[jsg_struct]
pub struct CaaRecord {
    pub critical: u8,
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

```rust
impl DnsUtil {
    #[jsg_method(name = "parseCaaRecord")]
    pub fn parse_caa_record(&self, record: &str) -> Result<CaaRecord, DnsParserError> {
        // implementation
    }

    #[jsg_method]
    pub fn parse_naptr_record(&self, record: &str) -> Result<NaptrRecord, DnsParserError> {
        // Exposed as "parseNaptrRecord" in JavaScript
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
    pub fn parse_caa_record(&self, record: &str) -> Result<CaaRecord, DnsParserError> {
        // implementation
    }
}
```

On struct definitions, generates:
- `jsg::Type` implementation
- `jsg::GarbageCollected` implementation (default, no-op trace)
- Wrapper struct and `ResourceTemplate` implementations

On impl blocks, scans for `#[jsg_method]` attributes and generates the `Resource` trait implementation.

### Garbage Collection

Resources are automatically integrated with V8's cppgc garbage collector. The macro automatically generates a `GarbageCollected` implementation that traces fields requiring GC integration:

- `Ref<T>` fields - traces the underlying resource
- `TracedReference<T>` fields - traces the JavaScript handle
- `Option<Ref<T>>` and `Option<TracedReference<T>>` - conditionally traces

```rust
#[jsg_resource]
pub struct MyResource {
    // Automatically traced
    js_callback: Option<TracedReference<Object>>,
    child_resource: Option<Ref<ChildResource>>,

    // Not traced (plain data)
    name: String,
}
```

For complex cases or custom tracing logic, you can manually implement `GarbageCollected`:

```rust
#[jsg_resource]
pub struct CustomResource {
    data: String,
}

impl jsg::GarbageCollected for CustomResource {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        // Custom tracing logic
    }
}
```
