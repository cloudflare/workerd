// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to the C++ `IoContext` calls in `ffi.h`.

#![expect(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]

#[cxx::bridge(namespace = "workerd::io_context")]
pub mod bridge {
    unsafe extern "C++" {
        include!("workerd/io/io-context/ffi.h");

        fn io_context_no_request_message() -> &'static str;
        fn io_context_try_current_id() -> KjMaybe<u64>;
        fn io_context_now(id: u64) -> Result<i64>;
        fn io_context_write_logfwdr(id: u64, channel: u32, message: &[u8]) -> Result<()>;
    }
}
