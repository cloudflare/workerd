use std::pin::Pin;

use jsg::Resource;

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
        |isolate| unsafe {
            let mut lock = jsg::Lock::from_isolate_ptr(isolate);
            let dns_util = DnsUtil::alloc(&mut lock, DnsUtil { _unused: 0 });
            DnsUtil::wrap(dns_util, &mut lock).into_ffi()
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
            let dns_util = DnsUtil::alloc(lock, DnsUtil { _unused: 0 });

            let lhs = DnsUtil::wrap(dns_util.clone(), lock);
            let rhs = DnsUtil::wrap(dns_util, lock);

            assert_eq!(lhs, rhs);
            Ok(())
        });
    }
}
