#![feature(must_not_suspend)]
#![warn(must_not_suspend)]

use std::pin::Pin;

use crate::dns::DnsUtil;
use crate::dns::DnsUtilWrapper;

pub mod dns;

#[cxx::bridge(namespace = "workerd::rust::api")]
mod ffi {
    #[namespace = "v8"]
    unsafe extern "C++" {
        include!("v8.h");
        type Isolate;
    }

    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type ModuleRegistry = jsg::modules::ffi::ModuleRegistry;
    }
    extern "Rust" {
        pub fn register_nodejs_modules(registry: Pin<&mut ModuleRegistry>);
    }
}

pub fn register_nodejs_modules(registry: Pin<&mut ffi::ModuleRegistry>) {
    jsg::modules::add_builtin(registry, "node-internal:dns", |isolate| unsafe {
        let mut isolate = jsg::v8::Lock::from_isolate(isolate);
        let dns_util = DnsUtil { js: None };
        let dns_util = Box::into_raw(Box::new(dns_util));
        let dns_util_wrapper = DnsUtilWrapper {
            constructor: jsg::create_resource_constructor::<DnsUtil>(&mut isolate),
        };
        // let lhs = jsg::wrap_resource(&mut isolate, dns_util, &dns_util_wrapper);
        // let rhs = jsg::wrap_resource(&mut isolate, dns_util, &dns_util_wrapper);
        // assert_eq!(lhs, rhs);
        jsg::wrap_resource(&mut isolate, dns_util, &dns_util_wrapper).into_ffi()
    });
}
