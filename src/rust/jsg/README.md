# JSG (JavaScript Glue) Rust Bindings

Rust bindings for the JSG (JavaScript Glue) layer, enabling Rust code to integrate with workerd's JavaScript runtime.

## FFI Functions with Raw Pointers

Functions exposed to C++ via FFI that receive raw pointers must be marked as `unsafe fn`. The `unsafe` keyword indicates to callers that the function deals with raw pointers and requires careful handling.

```rust
pub unsafe fn realm_create(isolate: *mut v8::ffi::Isolate) -> Box<Realm> {
    // implementation
}
```

For more information on unsafe Rust and raw pointers, see the [Rust Book: Unsafe Superpowers](https://doc.rust-lang.org/book/ch20-01-unsafe-rust.html#unsafe-superpowers).
