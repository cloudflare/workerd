// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::pin::Pin;

use jsg::ToJS;

use crate::dns::DnsUtil;

pub mod dns;

#[cxx::bridge(namespace = "workerd::rust::api")]
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
