#[cxx::bridge(namespace = "edgeworker::rust::rjs")]
mod ffi {
    extern "Rust" {}
}

#[no_mangle]
pub extern "C" fn init_rjs(
    isolate: *mut v8::Isolate,
    context: *mut v8::Context,
    obj: *mut v8::Object,
) {
    // Do nothing
}
