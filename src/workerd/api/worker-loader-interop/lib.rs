// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Interop with C++ `IoChannelFactory::loadIsolate()` for the Rust Worker Loader
//! (`api/worker-loader-api`): a [`SourceBuilder`] builds a C++ `DynamicWorkerSource` from
//! buffers that Rust made and hands over without copying, [`load`] and [`get`] load it, and a
//! [`WorkerStubHandle`] holds the `WorkerStubChannel` they return, from which it gets entrypoints
//! as `Fetcher`s and Durable Object classes as `DurableObjectClass`es.
//!
//! The C++ side, `ffi.h` and `ffi.c++`, is C++ API code, which lives here with the API rather
//! than in Rust JSG.

mod ffi;

pub use ffi::CodeCallback;
pub use ffi::CompatibilityDateValidation;
pub use ffi::Limits;
pub use ffi::ModuleKind;
pub use ffi::SourceBuilder;
pub use ffi::WorkerStubHandle;
pub use ffi::get;
pub use ffi::load;
