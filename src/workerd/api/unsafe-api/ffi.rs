// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge through which C++ registers the unsafe modules and creates an `unsafeEval`
//! binding, and the C++ they call.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and new_unsafe_eval() takes C++'s raw \
              isolate"
)]

use std::pin::Pin;

use jsg::ToJS;
use jsg::v8;

#[cxx::bridge(namespace = "workerd::api::unsafe_api")]
pub mod bridge {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = jsg::v8::ffi::Isolate;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
        type ModuleRegistry = jsg::v8::ffi::ModuleRegistry;
    }

    unsafe extern "C++" {
        include!("workerd/api/unsafe-api/ffi.h");

        /// Whether the `TEST_WORKERD` autogate is on.
        fn is_test_autogate_enabled() -> bool;

        /// Runs a Fuzzilli program. See ffi.h.
        ///
        /// # Safety
        /// `isolate` must be locked by this thread, with a context entered and no exception
        /// pending.
        unsafe fn reprl_execute(isolate: *mut Isolate, script: &[u8]) -> i32;

        /// Resets the coverage edge guards of a build for Fuzzilli.
        fn reprl_reset_edge_guards();
    }

    extern "Rust" {
        /// Registers `internal:unsafe-eval`.
        fn register_unsafe_modules(registry: Pin<&mut ModuleRegistry>);
        /// Registers `workerd:unsafe` and `workerd:unsafe-eval`.
        fn register_unsafe_module(registry: Pin<&mut ModuleRegistry>);
        /// Registers `workerd:stdin`.
        fn register_stdin_module(registry: Pin<&mut ModuleRegistry>);
    }

    extern "Rust" {
        /// A new `UnsafeEval`, for an `unsafeEval` binding.
        ///
        /// # Safety
        /// C++ calls this with the isolate locked, a context entered and a `HandleScope` open.
        unsafe fn new_unsafe_eval(isolate: *mut Isolate) -> MaybeLocal;
    }
}

/// The result of [`bridge::reprl_execute`] for a program that failed to compile, as
/// `REPRL_COMPILE_FAILED` in ffi.h.
pub const REPRL_COMPILE_FAILED: i32 = -1;

fn register_unsafe_modules(registry: Pin<&mut bridge::ModuleRegistry>) {
    crate::register_unsafe_modules(registry);
}

fn register_unsafe_module(registry: Pin<&mut bridge::ModuleRegistry>) {
    crate::register_unsafe_module(registry);
}

fn register_stdin_module(registry: Pin<&mut bridge::ModuleRegistry>) {
    crate::register_stdin_module(registry);
}

/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open.
unsafe fn new_unsafe_eval(isolate: *mut bridge::Isolate) -> bridge::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe { v8::run_ffi_callback(isolate, |lock| crate::UnsafeEval::new(lock).to_js(lock)) }
}
