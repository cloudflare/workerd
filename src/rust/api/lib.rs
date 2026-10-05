// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::pin::Pin;

use jsg::ToJS;

use crate::dns::DnsUtil;
use crate::url::UrlUtil;

pub mod dns;
pub mod url;

#[cxx::bridge(namespace = "workerd::rust::api")]
#[expect(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]
mod ffi {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type ModuleRegistry = jsg::v8::ffi::ModuleRegistry;
    }
    extern "Rust" {
        pub fn register_nodejs_modules(registry: Pin<&mut ModuleRegistry>);
    }
}

#[expect(
    unsafe_code,
    reason = "builds a `Lock` from the isolate pointer C++ hands to each module callback"
)]
pub fn register_nodejs_modules(mut registry: Pin<&mut ffi::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry.as_mut(),
        "node-internal:dns",
        // SAFETY: isolate is valid and locked — called from C++ module registration.
        |isolate| unsafe {
            let mut lock = jsg::Lock::from_isolate_ptr(isolate);
            let dns_util = DnsUtil::new();
            dns_util.to_js(&mut lock).into_ffi()
        },
        jsg::modules::ModuleType::Internal,
    );
    jsg::modules::add_builtin(
        registry,
        "node-internal:url",
        // SAFETY: isolate is valid and locked — called from C++ module registration.
        |isolate| unsafe {
            let mut lock = jsg::Lock::from_isolate_ptr(isolate);
            let url_util = UrlUtil::new();
            url_util.to_js(&mut lock).into_ffi()
        },
        jsg::modules::ModuleType::Internal,
    );
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
