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
mod ffi {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type ModuleRegistry = jsg::v8::ffi::ModuleRegistry;
    }
    extern "Rust" {
        pub fn register_nodejs_modules(registry: Pin<&mut ModuleRegistry>);

        // The Rust implementation of `node-internal:url`. Registered separately
        // because it is gated by the C++ `NODEJS_URL_RUST` autogate; when the
        // gate is off the C++ `UrlUtil` registers that module instead (see
        // node.h). Kept as its own entry point to avoid a boolean parameter.
        pub fn register_nodejs_url_module(registry: Pin<&mut ModuleRegistry>);
    }
}

pub fn register_nodejs_modules(registry: Pin<&mut ffi::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry,
        "node-internal:dns",
        // SAFETY: isolate is valid and locked — called from C++ module registration.
        |isolate| unsafe {
            let mut lock = jsg::Lock::from_isolate_ptr(isolate);
            let dns_util = DnsUtil::new();
            dns_util.to_js(&mut lock).into_ffi()
        },
        jsg::modules::ModuleType::Internal,
    );
}

pub fn register_nodejs_url_module(registry: Pin<&mut ffi::ModuleRegistry>) {
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
mod tests {
    use jsg_test::Harness;

    use super::*;

    #[test]
    fn test_wrap_resource_equality() {
        let harness = Harness::new();
        harness.run_in_context(|lock, _ctx| {
            let dns_util = DnsUtil::new();

            let lhs = dns_util.clone().to_js(lock);
            let rhs = dns_util.to_js(lock);

            assert_eq!(lhs, rhs);
            Ok(())
        });
    }
}
