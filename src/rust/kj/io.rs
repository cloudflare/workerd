#[cxx::bridge(namespace = "kj::rust")]
pub mod ffi {
    unsafe extern "C++" {
        include!("workerd/rust/kj/ffi.h");

        type AsyncInputStream;
        type AsyncIoStream;
        type AsyncOutputStream;
    }
}

pub type AsyncInputStream = ffi::AsyncInputStream;
pub type AsyncIoStream = ffi::AsyncIoStream;
pub type AsyncOutputStream = ffi::AsyncOutputStream;
