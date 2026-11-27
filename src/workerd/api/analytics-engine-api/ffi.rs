// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge through which C++ creates an [`AnalyticsEngine`](crate::AnalyticsEngine).

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and analytics_engine_new() takes C++'s \
              raw isolate"
)]

use jsg::ToJS;
use jsg::v8;

use crate::Binding;
use crate::encode::Header;

#[cxx::bridge(namespace = "workerd::api")]
mod bridge {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = jsg::v8::ffi::Isolate;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    extern "Rust" {
        /// A new `AnalyticsEngine` that writes to logfwdr channel `channel`, for `dataset`.
        unsafe fn analytics_engine_new(
            isolate: *mut Isolate,
            channel: u32,
            dataset: &str,
            schema_version: i64,
            account_id: i64,
        ) -> MaybeLocal;
    }
}

/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open.
unsafe fn analytics_engine_new(
    isolate: *mut bridge::Isolate,
    channel: u32,
    dataset: &str,
    schema_version: i64,
    account_id: i64,
) -> bridge::MaybeLocal {
    let binding = Binding {
        channel,
        header: Header {
            account_id,
            dataset: dataset.to_owned(),
            schema_version,
        },
    };
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            jsg::Rc::new(lock, crate::AnalyticsEngine::new(binding)).to_js(lock)
        })
    }
}
