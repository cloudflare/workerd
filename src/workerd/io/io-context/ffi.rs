// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to the C++ `IoContext` calls in `ffi.h`.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and current_id() reads through the \
              IoContext reference it returns"
)]

use std::pin::Pin;

#[cxx::bridge(namespace = "workerd::io_context")]
pub mod bridge {
    #[namespace = "workerd"]
    unsafe extern "C++" {
        include!("workerd/io/io-context.h");

        type IoContext;
    }

    unsafe extern "C++" {
        include!("workerd/io/io-context/ffi.h");

        /// The caller chooses the lifetime of the result; see `current_id()`.
        unsafe fn io_context_try_current<'a>() -> KjMaybe<Pin<&'a mut IoContext>>;
        fn io_context_id(context: &IoContext) -> u64;
        fn io_context_now(id: u64) -> Result<i64>;
        fn io_context_write_logfwdr(id: u64, channel: u32, message: &[u8]) -> Result<()>;
    }
}

/// The id of the current `IoContext`, or `None` outside a request.
pub fn current_id() -> Option<u64> {
    // SAFETY: the reference is used only to read the id, before this function returns, and the
    // current IoContext stays alive while code runs in it.
    let context: Option<Pin<&mut bridge::IoContext>> =
        unsafe { bridge::io_context_try_current() }.into();
    context.map(|context| bridge::io_context_id(&context))
}
