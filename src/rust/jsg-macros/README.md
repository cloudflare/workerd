# JSG Macros

Procedural macros for JSG (JavaScript Glue) Rust bindings. These macros reduce boilerplate when implementing the JSG type system.

## `#[jsg::r#struct]`

Automatically generates the `jsg::Struct` implementation for data structures. Only public fields are exposed to JavaScript as properties on the resulting object.

```rust
#[jsg::r#struct]
pub struct CaaRecord {
    pub critical: u8,
    pub field: String,
    pub value: String,
}

impl jsg::Type for CaaRecord {}
```

This macro generates code equivalent to manually implementing:

```rust
impl jsg::Struct for CaaRecord {
    fn wrap<'a, 'b>(&self, lock: &'a mut jsg::Lock) -> jsg::v8::Local<'b, jsg::v8::Value>
    where
        'b: 'a,
    {
        // conversion implementation
    }
}
```

## `#[jsg::method]`

Generates FFI callback functions for JSG resource methods. Creates a `{method_name}_callback` extern "C" function that handles marshalling between JavaScript and Rust.

```rust
impl DnsUtil {
    #[jsg::method(name = "parseCaaRecord")]
    pub fn parse_caa_record(&self, record: &str) -> Result<CaaRecord, DnsParserError> {
        // implementation
    }
}
```

The generated callback must be registered in the resource's `members()` implementation:

```rust
impl Resource for DnsUtil {
    fn members() -> Vec<Member> {
        vec![Member::Method {
            name: "parseCaaRecord",
            callback: Self::parse_caa_record_callback,
        }]
    }
    // ...
}
```

### Supported Parameter Types

Currently, the `#[jsg::method]` macro supports:
- `&str` - Automatically unwrapped from JavaScript strings

Additional type support will be added as needed.
