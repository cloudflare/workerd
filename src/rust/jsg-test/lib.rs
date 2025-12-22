use std::pin::Pin;

use jsg::v8;
use kj_rs::KjOwn;

#[cfg(test)]
mod tests;

#[cxx::bridge(namespace = "workerd::rust::jsg_test")]
mod ffi {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type Isolate = jsg::v8::ffi::Isolate;
        type Local = jsg::v8::ffi::Local;
    }

    #[derive(Debug, PartialEq, Eq)]
    struct EvalResult {
        success: bool,
        result_type: String,
        result_value: String,
    }

    unsafe extern "C++" {
        include!("workerd/rust/jsg-test/ffi.h");

        type TestHarness;
        type EvalContext;

        pub unsafe fn create_test_harness() -> KjOwn<TestHarness>;
        pub unsafe fn run_in_context(
            self: &TestHarness,
            callback: unsafe fn(*mut Isolate, Pin<&mut EvalContext>),
        );

        #[cxx_name = "eval"]
        pub fn eval_safe(self: &EvalContext, code: &str) -> EvalResult;

        #[cxx_name = "set_global"]
        pub fn set_global_safe(self: &EvalContext, name: &str, value: Local);
    }
}

pub struct Harness(KjOwn<ffi::TestHarness>);

pub use ffi::EvalContext;
pub use ffi::EvalResult;

impl Harness {
    pub fn new() -> Self {
        Self(unsafe { ffi::create_test_harness() })
    }

    pub fn run_in_context(&self, callback: fn(*mut v8::ffi::Isolate, Pin<&mut EvalContext>)) {
        unsafe { self.0.run_in_context(callback) }
    }
}

impl Default for Harness {
    fn default() -> Self {
        Self::new()
    }
}
