#![feature(must_not_suspend)]
#![warn(must_not_suspend)]

use std::pin::Pin;

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

        fn register_add_builtin_module(
            registry: Pin<&mut ModuleRegistry>,
            specifier: &str,
            callback: unsafe fn(*mut Isolate) -> u64,
        );
    }
    extern "Rust" {
        pub fn register_nodejs_modules(registry: Pin<&mut ModuleRegistry>);
    }
}

pub fn register_nodejs_modules(registry: Pin<&mut ffi::ModuleRegistry>) {
    ffi::register_add_builtin_module(registry, "node-internal:dns", |isolate| unsafe {
        let isolate_ptr = isolate as *mut jsg::ffi::Isolate;
        jsg::instantiate_resource::<dns::DnsUtil>(isolate_ptr).into_raw()
    });
}
