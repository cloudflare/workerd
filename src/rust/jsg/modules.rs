use std::pin::Pin;

use crate::v8::ffi;

pub enum Type {
    INTERNAL,
}

pub fn add_builtin(
    registry: Pin<&mut ffi::ModuleRegistry>,
    specifier: &str,
    callback: fn(*mut ffi::Isolate) -> ffi::Local,
) {
    unsafe {
        ffi::register_add_builtin_module(registry, specifier, callback);
    }
}
