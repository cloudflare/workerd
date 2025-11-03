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
        let mut isolate = jsg::v8::Isolate::from_ffi(isolate);
        let dns_util = DnsUtil {};
        let dns_util_wrapper = DnsUtilWrapper {
            constructor: jsg::create_resource_constructor::<DnsUtil>(&mut isolate),
        };
        jsg::wrap_resource(&mut isolate, dns_util, &dns_util_wrapper).to_ffi()
    });
}
