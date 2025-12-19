# JSG (JavaScript Glue) Rust Bindings

Rust bindings for the JSG (JavaScript Glue) layer, enabling Rust code to integrate with workerd's JavaScript runtime.

## `NonCoercible<T>`

JavaScript automatically coerces types in certain contexts. For instance, when a JavaScript API expects a string, calling it with the value `null` will result in the null being coerced into the string value "null".

`NonCoercible<T>` can be used to disable automatic type coercion in APIs. For instance, `NonCoercible<String>` can be used to accept a value only if the input is already a string. If the input is the value `null`, then an error is thrown rather than silently coercing to "null".

### Supported Types

Any type implementing the `jsg::Type` trait can be used with `NonCoercible<T>`. Built-in implementations include:

- `NonCoercible<String>` - only accepts JavaScript strings
- `NonCoercible<bool>` - only accepts JavaScript booleans
- `NonCoercible<f64>` - only accepts JavaScript numbers

### Usage

```rust
use jsg::NonCoercible;

// This function will only accept actual strings, not values that can be coerced to strings
#[jsg_method]
pub fn process_string(&self, param: NonCoercible<String>) -> Result<(), Error> {
    let s: &String = param.value();
    // or use Deref: let s: &str = &*param;
    // ...
}
```

### Important Notes

Using `NonCoercible<T>` runs counter to Web IDL and general JavaScript API conventions. In nearly all cases, APIs should allow coercion to occur and should deal with the coerced input accordingly to avoid being a source of user confusion. Only use `NonCoercible` if you have a good reason to disable coercion.

## FFI Functions with Raw Pointers

Functions exposed to C++ via FFI that receive raw pointers must be marked as `unsafe fn`. The `unsafe` keyword indicates to callers that the function deals with raw pointers and requires careful handling.

```rust
pub unsafe fn realm_create(isolate: *mut v8::ffi::Isolate) -> Box<Realm> {
    // implementation
}
```

For more information on unsafe Rust and raw pointers, see the [Rust Book: Unsafe Superpowers](https://doc.rust-lang.org/book/ch20-01-unsafe-rust.html#unsafe-superpowers).
