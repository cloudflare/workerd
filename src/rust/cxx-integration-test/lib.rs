//! Non-production crate to help test various aspects of rust/c++ integration.

use std::{
    io::{Error, ErrorKind},
    pin::Pin,
    time::Duration,
};

use tracing::{debug, error, info, trace, warn};

type Result<T> = std::io::Result<T>;

#[cxx::bridge(namespace = "workerd::rust::test")]
mod ffi {
    unsafe extern "C++" {
        // To use a C++ callback first define it as an opaque to Rust type.
        type TestCallback;

        // Then define a call function with a correct signature.
        // cxx can't call operator() but can call an ordinary function.
        // Use C++ preprocessor to alias operatorCALL to operator().
        #[cxx_name = "operatorCALL"]
        fn call(self: Pin<&mut TestCallback>, a: usize, b: usize) -> usize;

        // Include the header with the actual callback type definition.
        // This will be included into cxx generated files.
        include!("workerd/rust/cxx-integration-test/cxx-rust-integration-test.h");
    }

    // Structures defined without any extern specifier are visible both to Rust and c++.
    struct SharedStruct {
        a: i32,
        b: i32,
    }

    extern "Rust" {
        fn result_ok() -> Result<i32>;
        fn result_error() -> Result<i32>;

        fn log_every_level();

        fn call_callback(callback: Pin<&mut TestCallback>, a: usize, b: usize) -> usize;
    }

    extern "Rust" {
        // Shared structures can be passed with full ownership
        fn pass_shared_struct(s: SharedStruct) -> i32;
        fn return_shared_struct() -> SharedStruct;

        // References can be safely passed from c++
        fn pass_shared_struct_as_ref(s: &SharedStruct) -> i32;
        fn pass_shared_struct_as_mut_ref(s: &mut SharedStruct);

        // Structs can be passed as pointers, but functions need to be unsafe then
        unsafe fn pass_shared_struct_as_const_ptr(s: *const SharedStruct) -> i32;
        unsafe fn pass_shared_struct_as_mut_ptr(s: *mut SharedStruct);

        // Box<T> is supported
        fn pass_shared_struct_as_box(s: Box<SharedStruct>) -> i32;
        fn return_shared_struct_as_box() -> Box<SharedStruct>;
    }

    extern "Rust" {
        // rust-defined structures can be exposed to c++ as opaque type.
        type RustStruct;

        // rust needs to provide a way to access instances of the type.
        fn rust_struct_new_box(name: &str) -> Box<RustStruct>;

        // c++ can interact with opaque structures using conventional functions and methods
        fn get_name(self: &RustStruct) -> &str;

        // if there's only one type defined in extern block, then you can use self shorthand
        fn set_name(&mut self, name: &str);
    }

    extern "Rust" {
        fn get_string() -> String;
        fn get_str() -> &'static str;
    }

    // test async
    unsafe extern "C++" {
        type UsizeCallback;
        #[cxx_name = "operatorCALL"]
        fn call(self: Pin<&mut UsizeCallback>, x: usize);
    }
    extern "Rust" {
        fn async_immediate(callback: Pin<&'static mut UsizeCallback>);
        fn async_sleep(callback: Pin<&'static mut UsizeCallback>);
    }
}

#[allow(clippy::unnecessary_wraps)]
fn result_ok() -> Result<i32> {
    Ok(42)
}

fn result_error() -> Result<i32> {
    Err(Error::new(ErrorKind::Other, "test error"))
}

fn log_every_level() {
    trace!("rust_trace_message");
    debug!("rust_debug_message");
    info!("rust_info_message");
    warn!("rust_warn_message");
    error!("rust_error_message");
}

fn call_callback(callback: Pin<&mut ffi::TestCallback>, a: usize, b: usize) -> usize {
    callback.call(a, b)
}

#[allow(clippy::needless_pass_by_value)]
fn pass_shared_struct(s: ffi::SharedStruct) -> i32 {
    s.a + s.b
}

fn return_shared_struct() -> ffi::SharedStruct {
    ffi::SharedStruct { a: 13, b: 29 }
}

fn pass_shared_struct_as_ref(s: &ffi::SharedStruct) -> i32 {
    s.a + s.b
}

fn pass_shared_struct_as_mut_ref(s: &mut ffi::SharedStruct) {
    s.a += s.b;
    s.b = 0;
}

unsafe fn pass_shared_struct_as_const_ptr(s: *const ffi::SharedStruct) -> i32 {
    assert!(!s.is_null());
    (*s).a + (*s).b
}

unsafe fn pass_shared_struct_as_mut_ptr(s: *mut ffi::SharedStruct) {
    (*s).a = 0;
    (*s).b = 0;
}

#[allow(clippy::boxed_local)] // clippy is right, but we want to test it anyway
#[allow(clippy::needless_pass_by_value)]
fn pass_shared_struct_as_box(s: Box<ffi::SharedStruct>) -> i32 {
    s.a + s.b
}

fn return_shared_struct_as_box() -> Box<ffi::SharedStruct> {
    Box::new(ffi::SharedStruct { a: 1, b: 41 })
}

struct RustStruct {
    name: String,
}

fn rust_struct_new_box(name: &str) -> Box<RustStruct> {
    Box::new(RustStruct {
        name: name.to_owned(),
    })
}

impl RustStruct {
    fn get_name(&self) -> &str {
        &self.name
    }

    fn set_name(&mut self, name: &str) {
        name.clone_into(&mut self.name);
    }
}

fn get_string() -> String {
    "rust_string".to_owned()
}

fn get_str() -> &'static str {
    "rust_str"
}

unsafe impl Send for ffi::UsizeCallback {}
unsafe impl Sync for ffi::UsizeCallback {}

fn async_immediate(callback: Pin<&'static mut ffi::UsizeCallback>) {
    cxx_integration::tokio::spawn(async move {
        callback.call(42);
    });
}

fn async_sleep(callback: Pin<&'static mut ffi::UsizeCallback>) {
    cxx_integration::tokio::spawn(async move {
        tokio::time::sleep(Duration::from_millis(1)).await;
        callback.call(42);
    });
}

#[cfg(all(test, feature = "sanitizer_address"))]
mod tests {
    use nix::sys::signal::Signal;
    use safe_libc::expect_signal;

    #[test]
    fn asan_stack_buffer_overflow() {
        expect_signal!(Signal::SIGABRT, {
            let xs = [0, 1, 2, 3];
            let _y = unsafe { *xs.as_ptr().offset(4) };
        });
    }
}
