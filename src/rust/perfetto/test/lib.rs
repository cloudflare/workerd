//! Checks that Rust trace points (`//src/rust/perfetto`) and C++ trace points are recorded in the
//! same Perfetto session, with matching flow, track and counter track IDs.

#[cxx::bridge(namespace = "workerd::rust::perfetto_test")]
#[cfg_attr(not(test), expect(dead_code, reason = "only used by the tests"))]
mod ffi {
    unsafe extern "C++" {
        include!("workerd/rust/perfetto/test/test-helper.h");

        fn perfetto_in_build() -> bool;
        fn start_trace(categories: &str) -> Result<()>;
        fn emit_cpp_events(address: usize);
        fn stop_trace() -> Result<Vec<u8>>;
    }
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
