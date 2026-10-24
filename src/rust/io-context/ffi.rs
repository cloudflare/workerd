// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to the C++ `IoContext` calls in `ffi.h`.

#![allow(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]

#[cxx::bridge(namespace = "workerd::rust::io_context")]
pub mod bridge {
    #[namespace = "workerd"]
    unsafe extern "C++" {
        include!("workerd/io/io-context.h");

        type IoContext;
    }

    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = jsg::v8::ffi::Isolate;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    unsafe extern "C++" {
        include!("workerd/rust/io-context/ffi.h");

        /// Called through `jsg::v8::catching()`. The caller chooses the lifetime of the result.
        unsafe fn io_context_current<'a>(
            isolate: *mut Isolate,
            exception: &mut MaybeLocal,
        ) -> Result<Pin<&'a mut IoContext>>;
        fn io_context_try_current() -> *mut IoContext;
        fn io_context_now(context: Pin<&mut IoContext>) -> Result<i64>;
        fn io_context_write_logfwdr(
            context: Pin<&mut IoContext>,
            channel: u32,
            message: &[u8],
        ) -> Result<()>;
    }
}
