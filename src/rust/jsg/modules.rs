use std::pin::Pin;

use crate::v8::ffi;

pub enum Type {
    INTERNAL,
}

/// Registers a builtin module with the given specifier.
///
/// The callback is invoked when JavaScript imports the module, receiving the isolate pointer
/// and returning a V8 Local handle to the module's exports object.
pub fn add_builtin(
    registry: Pin<&mut ffi::ModuleRegistry>,
    specifier: &str,
    callback: fn(*mut ffi::Isolate) -> ffi::Local,
) {
    unsafe {
        ffi::register_add_builtin_module(registry, specifier, callback);
    }
}
