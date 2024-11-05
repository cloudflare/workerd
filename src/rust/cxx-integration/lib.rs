pub mod tokio;

#[cxx::bridge(namespace = "workerd::rust::cxx_integration")]
mod ffi {
    extern "Rust" {
        fn init();

        fn trigger_panic(msg: &str);
    }

    unsafe extern "C++" {
        include!("workerd/rust/cxx-integration/cxx-bridge.h");
    }
}

pub fn init() {
    init_tokio(None);
}

/// Initialize tokio runtime.
/// Should not be called directly but as a part of a downstream cxx-integration init.
pub fn init_tokio(worker_threads: Option<usize>) {
    tokio::init(worker_threads);
}

fn trigger_panic(msg: &str) {
    panic!("{}", msg)
}
