# KJ integration for the in-tree CXX fork

This directory contains the Rust and C++ support for passing KJ types across CXX bridges. It is
built as `//src/rust/cxx/kj-rs` and consumed directly by workerd's Rust crates.

It provides exception conversion, KJ ownership and refcount wrappers, `kj::Maybe` and `kj::Date`
interop, and adapters between Rust futures and `kj::Promise`. The code moved into workerd together
with the rest of the former `workerd-cxx` repository; it is no longer fetched as an external Bazel
dependency.

Tests live in [`tests/`](tests/) and run as part of:

```sh
bazel test //src/rust/cxx/...
```
