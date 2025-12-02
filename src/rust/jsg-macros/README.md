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

**Important:** Resource structs must include a `_state: jsg::ResourceState` field for internal JSG state management.

```rust
#[jsg_resource]
pub struct DnsUtil {
    pub _state: jsg::ResourceState,
}

#[jsg_resource(name = "CustomUtil")]
pub struct MyUtil {
    pub _state: jsg::ResourceState,
}

#[jsg_resource]
impl DnsUtil {
    #[jsg_method]
    pub fn parse_caa_record(&self, record: &str) -> Result<CaaRecord, DnsParserError> {
        // implementation
    }
}
```

On struct definitions, generates `jsg::Type`, wrapper struct, and `ResourceTemplate` implementations. On impl blocks, scans for `#[jsg_method]` attributes and generates the `Resource` trait implementation.
