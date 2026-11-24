// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge through which C++ creates a [`WorkerLoader`](crate::WorkerLoader).

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and worker_loader_new() takes C++'s raw \
              isolate"
)]

use jsg::ToJS;
use jsg::v8;

#[cxx::bridge(namespace = "workerd::api")]
mod bridge {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = jsg::v8::ffi::Isolate;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    #[namespace = "workerd"]
    unsafe extern "C++" {
        include!("workerd/io/compatibility-date.h");

        type CompatibilityDateValidation = worker_loader_interop::CompatibilityDateValidation;
    }

    extern "Rust" {
        /// A new `WorkerLoader` on the loader channel `channel`.
        unsafe fn worker_loader_new(
            isolate: *mut Isolate,
            channel: u32,
            date_validation: CompatibilityDateValidation,
        ) -> MaybeLocal;
    }
}

/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open.
unsafe fn worker_loader_new(
    isolate: *mut bridge::Isolate,
    channel: u32,
    date_validation: bridge::CompatibilityDateValidation,
) -> bridge::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            let loader = jsg::Rc::new(lock, crate::WorkerLoader::new(channel, date_validation));
            loader.to_js(lock)
        })
    }
}
