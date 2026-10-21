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

pub fn register_nodejs_modules(mut registry: Pin<&mut ffi::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry.as_mut(),
        "node-internal:dns",
        |lock| DnsUtil::new().to_js(lock),
        jsg::modules::ModuleType::Internal,
    );
    jsg::modules::add_builtin(
        registry,
        "node-internal:url",
        |lock| UrlUtil::new().to_js(lock),
        jsg::modules::ModuleType::Internal,
    );
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
