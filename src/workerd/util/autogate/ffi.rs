// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to the C++ shims in `ffi.h`.

#![allow(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]

#[cxx::bridge(namespace = "workerd::autogate_bridge")]
pub mod bridge {
    unsafe extern "C++" {
        include!("workerd/util/autogate/ffi.h");

        #[nothrow]
        fn autogate_count() -> usize;
        #[nothrow]
        fn autogate_is_enabled(index: usize) -> bool;
        #[nothrow]
        fn autogate_name(index: usize) -> &'static str;
    }
}
