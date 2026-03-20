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

Parameters and return values are handled via the `jsg::Wrappable` trait. Any type implementing `Wrappable` can be used as a parameter or return value:

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
    pub fn parse_caa_record(&self, record: String) -> Result<CaaRecord, DnsParserError> {
        // Instance method on the prototype
    }

    #[jsg_method]
    pub fn create(name: String) -> Result<String, jsg::Error> {
        // Static method on the constructor (no &self)
    }
}
```

On struct definitions, generates `jsg::Type`, wrapper struct, and `ResourceTemplate` implementations. On impl blocks, scans for `#[jsg_method]` and `#[jsg_static_constant]` attributes and generates the `Resource` trait implementation. Methods with a receiver (`&self`/`&mut self`) are registered as instance methods; methods without a receiver are registered as static methods.

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
