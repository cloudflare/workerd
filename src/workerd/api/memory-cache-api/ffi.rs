// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge through which C++ creates a [`MemoryCache`](crate::MemoryCache), and the C++ it
//! calls.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and memory_cache_new() takes C++'s raw \
              isolate"
)]

pub use bridge::is_predictable_mode_for_test;
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

    #[namespace = "workerd::rust::memory_cache"]
    unsafe extern "C++" {
        include!("workerd/rust/memory-cache/ffi/lib.rs.h");
    }

    #[namespace = "workerd"]
    unsafe extern "C++" {
        include!("workerd/util/thread-scopes.h");

        /// Whether the process runs in predictable mode, for tests.
        #[cxx_name = "isPredictableModeForTest"]
        fn is_predictable_mode_for_test() -> bool;
    }

    #[namespace = "workerd::rust::memory_cache"]
    extern "Rust" {
        type Binding = memory_cache_ffi::Binding;
    }

    extern "Rust" {
        /// A new `MemoryCache` for `binding`.
        unsafe fn memory_cache_new(isolate: *mut Isolate, binding: Box<Binding>) -> MaybeLocal;
    }
}

/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open.
unsafe fn memory_cache_new(
    isolate: *mut bridge::Isolate,
    binding: Box<memory_cache_ffi::Binding>,
) -> bridge::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            let cache = jsg::Rc::new(crate::MemoryCache::new(binding.into_inner()));
            cache.to_js(lock)
        })
    }
}
