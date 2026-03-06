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
- `jsg::GarbageCollected` implementation (default, no-op trace)
- Wrapper struct and `ResourceTemplate` implementations

On impl blocks, scans for `#[jsg_method]` attributes and generates the `Resource` trait implementation. Methods with a receiver (`&self`/`&mut self`) are registered as instance methods; methods without a receiver are registered as static methods.

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

Resources are automatically integrated with V8's garbage collector through the C++ `Wrappable` base class. The macro automatically generates a `GarbageCollected` implementation that traces fields requiring GC integration:

- `Ref<T>` fields - traces the underlying resource
- `WeakRef<T>` fields - weak reference (no-op trace, does not keep the target alive)
- `Option<Ref<T>>` / `Nullable<Ref<T>>` - conditionally traces

```rust
#[jsg_resource]
pub struct MyResource {
    // Automatically traced (strong - keeps child alive)
    child_resource: Option<Ref<ChildResource>>,

    // Nullable variant (traces when Nullable::Some)
    nullable_child: Nullable<Ref<ChildResource>>,

    // Weak reference (does not keep target alive)
    observer: WeakRef<ChildResource>,

    // Not traced (plain data)
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
