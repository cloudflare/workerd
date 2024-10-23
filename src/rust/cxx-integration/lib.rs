pub mod tokio;

#[cxx::bridge(namespace = "workerd::rust::cxx_integration")]
mod ffi {
    extern "Rust" {
        fn init();

        fn trigger_panic(msg: &str);
    }

    unsafe extern "C++" {
        include!("rust/cxx-integration/cxx-bridge.h");
    }
}

pub fn init() {
    tokio::init();
}

fn trigger_panic(msg: &str) {
    panic!("{}", msg)
}
