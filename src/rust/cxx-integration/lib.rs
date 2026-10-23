#[cxx::bridge(namespace = "workerd::rust::cxx_integration")]
#[expect(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]
mod ffi {
    extern "Rust" {
        fn trigger_panic(msg: &str);
    }
}

#[expect(
    clippy::panic,
    reason = "intentional test hook exposed to C++ to exercise the panic -> kj::Exception conversion at the cxx bridge boundary"
)]
fn trigger_panic(msg: &str) {
    panic!("{}", msg)
}
