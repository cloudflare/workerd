#[cxx::bridge(namespace = "workerd::rust::jsg::autogate")]
mod bridge {
    unsafe extern "C++" {
        include!("workerd/rust/jsg/autogate/ffi.h");

        pub fn is_enabled(key: String) -> bool;
    }
}

pub use bridge::is_enabled;
