use std::pin::Pin;

pub use ffi::ModuleType;

use crate::v8::ffi;

/// Registers a builtin module with the given specifier and module type.
///
/// The callback is invoked when JavaScript imports the module, receiving the isolate pointer
/// and returning a V8 Local handle to the module's exports object.
pub fn add_builtin(
    registry: Pin<&mut ffi::ModuleRegistry>,
    specifier: &str,
    callback: fn(*mut ffi::Isolate) -> ffi::Local,
    module_type: ModuleType,
) {
    unsafe {
        ffi::register_add_builtin_module(registry, specifier, callback, module_type);
    }
}
