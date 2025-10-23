pub enum Type {
    INTERNAL,
}

#[cxx::bridge(namespace = "workerd::rust::jsg")]
pub mod ffi {
    extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type ModuleRegistry;
    }
}
