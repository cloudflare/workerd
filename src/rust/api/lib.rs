#![feature(must_not_suspend)]
#![warn(must_not_suspend)]

use std::pin::Pin;

pub mod dns;

#[cxx::bridge(namespace = "workerd::rust::api")]
mod ffi {
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
    todo!("register_nodejs_modules")
}
