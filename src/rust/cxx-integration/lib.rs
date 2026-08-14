// Production code must not panic; test code is exempt via clippy.toml allow-*-in-tests.
#![deny(clippy::expect_used, clippy::panic, clippy::unreachable)]
#![deny(clippy::todo, clippy::unimplemented)]

#[cxx::bridge(namespace = "workerd::rust::cxx_integration")]
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
