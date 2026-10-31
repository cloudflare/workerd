// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Interop between Rust JSG resources and C++ JSG resources.
//!
//! - [`Ref<T>`] is a Rust reference to a C++ JSG resource: a resource field, a method parameter
//!   or a return value, such as a lazy property returning the C++ object of the current request.
//!   [`CppResource`] declares a C++ class to Rust.
//! - Rust calls the methods of a C++ resource through per-method shims that take `&T` and run in
//!   `callCpp()` (`jsg-cpp.h`).
//! - C++ refers to a Rust resource with a `RustRef<Tag>` (`jsg-cpp.h`).
//!
//! The C++ side, `jsg-cpp.h` and `ffi.c++`, is the one part of Rust JSG that may use C++ JSG;
//! see `src/rust/jsg/README.md` §Safety rules, rule 3.

mod ffi;

pub use ffi::CppRef;
pub use ffi::CppResource;
pub use ffi::Ref;
pub use ffi::bridge;
pub use ffi::call_cpp;
pub use ffi::call_cpp_for_ref;
