# KJ integration for the in-tree CXX fork

This directory contains the Rust and C++ support for passing KJ types across CXX bridges. It is
built as `//src/rust/cxx/kj-rs` and consumed directly by workerd's Rust crates.

It provides exception conversion, KJ ownership and refcount wrappers, `kj::Maybe` and `kj::Date`
interop, and adapters between Rust futures and `kj::Promise`. The code moved into workerd together
with the rest of the former `workerd-cxx` repository; it is no longer fetched as an external Bazel
dependency.

## Dropping a `KjOwn<T>`

`KjOwn<T>` mirrors `kj::Own<T>`'s two-word layout, and dropping it runs `kj::Own<T>::~Own()` in
C++. That destructor is typed: for a polymorphic `T` it hands the disposer
`dynamic_cast<void*>(ptr)`, the complete object's address, which differs from the `T*` when `T`
is a base at a nonzero offset of the object it points into (multiple inheritance). Rust cannot
compute that offset, so `KjOwn<T>` requires `T: kj_rs::OwnTarget`, and `OwnTarget::__drop`
calls a C++ function generated for that `T`:

```
void cxxbridge$kjrs$own$<namespace>$<Type>$drop(::kj::Own<T>* own) { own->~Own(); }
```

A bridge generates the `OwnTarget` implementation and this function for each `T` it declares
(`type T;`) and holds in a `KjOwn<T>`, next to its other per-type instantiations
(`UniquePtr<T>`, `KjMaybe<T>`). A bridge that only aliases `T` (`type T = other::ffi::T;`)
cannot implement the trait for it, so the declaring bridge must supply it: either by naming
`KjOwn<T>` itself or with an explicit `impl KjOwn<T> {}`. The primitive types a `KjOwn` may
hold are implemented in `own.rs` over one untyped drop in `own.c++`: a primitive is never
polymorphic, so its `kj::Own` passes the disposer the pointer unchanged. The generated C++ asserts that
`T` is complete where the bridge is compiled, since `~Own()` needs the definition.

Tests live in [`tests/`](tests/) and run as part of:

```sh
bazel test //src/rust/cxx/...
```
