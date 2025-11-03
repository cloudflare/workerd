use std::pin::Pin;

pub enum Type {
    INTERNAL,
}

#[cxx::bridge(namespace = "workerd::rust::jsg")]
pub mod ffi {
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type ModuleRegistry;
        type ModuleCallback;
        type Isolate = crate::v8::ffi::Isolate;

        pub unsafe fn register_add_builtin_module(
            registry: Pin<&mut ModuleRegistry>,
            specifier: &str,
            callback: unsafe fn(*mut Isolate) -> usize,
        );
    }
}

pub fn add_builtin(
    registry: Pin<&mut ffi::ModuleRegistry>,
    specifier: &str,
    callback: fn(*mut ffi::Isolate) -> usize,
) {
    unsafe {
        ffi::register_add_builtin_module(registry, specifier, callback);
    }
}
