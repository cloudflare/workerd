use std::pin::Pin;

use jsg::ResourceState;
use jsg::ResourceTemplate;

use crate::dns::DnsUtil;
use crate::dns::DnsUtilTemplate;

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
            let mut lock = jsg::Lock::from_raw_isolate(isolate);
            let dns_util = jsg::Ref::new(DnsUtil {
                _state: ResourceState::default(),
            });
            let mut dns_util_template = DnsUtilTemplate::new(&mut lock);

            jsg::wrap_resource(&mut lock, dns_util, &mut dns_util_template).into_ffi()
        },
        jsg::modules::ModuleType::INTERNAL,
    );
}

#[cfg(test)]
mod tests {
    use jsg::ResourceTemplate;
    use jsg_test::Harness;

    use super::*;

    #[test]
    fn test_wrap_resource_equality() {
        let harness = Harness::new();
        harness.run_in_context(|isolate| unsafe {
            let mut lock = jsg::Lock::from_raw_isolate(isolate);
            let dns_util = jsg::Ref::new(DnsUtil {
                _state: ResourceState::default(),
            });
            let mut dns_util_template = DnsUtilTemplate::new(&mut lock);

            let lhs = jsg::wrap_resource(&mut lock, dns_util.clone(), &mut dns_util_template);
            let rhs = jsg::wrap_resource(&mut lock, dns_util, &mut dns_util_template);

            assert_eq!(lhs, rhs);
        });
    }
}
