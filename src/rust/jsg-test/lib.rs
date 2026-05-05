// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::pin::Pin;

use jsg::FromJS;
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

    #[derive(Debug)]
    struct EvalResult {
        success: bool,
        value: KjMaybe<Local>,
    }

    enum GcType {
        /// Full (major) GC — collects both young and old generations, plus cppgc heap.
        Full = 0,
        /// Minor (scavenge) GC — collects only the young generation.
        Minor = 1,
    }

    unsafe extern "C++" {
        include!("workerd/rust/jsg-test/ffi.h");
        type TestHarness;
        type EvalContext;

        pub unsafe fn create_test_harness() -> KjOwn<TestHarness>;
        pub unsafe fn run_in_context(
            self: &TestHarness,
            data: usize, /* callback */
            callback: unsafe fn(usize /* callback */, *mut Isolate, Pin<&mut EvalContext>),
        );

        pub unsafe fn eval(self: &EvalContext, code: &str) -> EvalResult;
        pub unsafe fn set_global(self: &EvalContext, name: &str, value: Local);

        /// Triggers garbage collection for testing purposes.
        /// Note: For GC to actually collect objects, they must not be reachable from the
        /// current HandleScope.
        #[expect(clippy::allow_attributes)] // Only used in tests, but #[expect(dead_code)] fails during test builds
        #[allow(dead_code)]
        pub unsafe fn request_gc(isolate: *mut Isolate, gc_type: GcType);

        /// Creates a V8 object with the C++ `WORKERD_WRAPPABLE_TAG` in its internal fields.
        /// Used to test that Rust unwrap correctly rejects non-Rust wrappable objects.
        #[expect(clippy::allow_attributes)]
        #[allow(dead_code)]
        pub unsafe fn create_cpp_tagged_object(isolate: *mut Isolate) -> Local;

    }
}

pub struct Harness(KjOwn<ffi::TestHarness>);

pub struct EvalContext<'a> {
    inner: &'a ffi::EvalContext,
    isolate: v8::IsolatePtr,
}

#[derive(Debug)]
pub enum EvalError<'a> {
    UncoercibleResult {
        value: v8::Local<'a, v8::Value>,
        message: String,
    },
    Exception(v8::Local<'a, v8::Value>),
    EvalFailed,
}

impl EvalError<'_> {
    /// Extracts a `jsg::Error` from an `EvalError::Exception` variant.
    ///
    /// # Panics
    ///
    /// Panics if `self` is not `EvalError::Exception`, or if the value cannot be converted to a
    /// `jsg::Error`.
    pub fn unwrap_jsg_err(&self, lock: &mut jsg::Lock) -> jsg::Error {
        match self {
            EvalError::Exception(value) => jsg::Error::from_js(lock, value.clone())
                .expect("Failed to convert exception to jsg::Error"),
            _ => panic!("Unexpected error"),
        }
    }
}
impl EvalContext<'_> {
    pub fn eval<T>(&self, lock: &mut jsg::Lock, code: &str) -> Result<T, EvalError<'_>>
    where
        T: jsg::FromJS<ResultType = T>,
    {
        // SAFETY: self.inner is a valid EvalContext from C++; code is a valid str.
        let result = unsafe { self.inner.eval(code) };
        let opt_local: Option<v8::ffi::Local> = result.value.into();

        if result.success {
            match opt_local {
                Some(local) => {
                    // SAFETY: self.isolate is valid and local is from a successful eval result.
                    let local = unsafe { v8::Local::from_ffi(self.isolate, local) };
                    match T::from_js(lock, local.clone()) {
                        Err(e) => Err(EvalError::UncoercibleResult {
                            value: local,
                            message: e.to_string(),
                        }),
                        Ok(value) => Ok(value),
                    }
                }
                None => unreachable!(),
            }
        } else {
            match opt_local {
                Some(local) => {
                    // SAFETY: self.isolate is valid and local is from an eval exception.
                    let value = unsafe { v8::Local::from_ffi(self.isolate, local) };
                    Err(EvalError::Exception(value))
                }
                None => Err(EvalError::EvalFailed),
            }
        }
    }

    /// Evaluates JavaScript code and returns the raw `Local<Value>` without conversion.
    ///
    /// Useful for obtaining handles (e.g. functions) that aren't `FromJS` types.
    pub fn eval_raw(&self, code: &str) -> Result<v8::Local<'_, v8::Value>, EvalError<'_>> {
        // SAFETY: self.inner is a valid EvalContext from C++; code is a valid str.
        let result = unsafe { self.inner.eval(code) };
        let opt_local: Option<v8::ffi::Local> = result.value.into();

        if result.success {
            match opt_local {
                // SAFETY: self.isolate is valid and local is from a successful eval result.
                Some(local) => Ok(unsafe { v8::Local::from_ffi(self.isolate, local) }),
                None => unreachable!(),
            }
        } else {
            match opt_local {
                Some(local) => {
                    // SAFETY: self.isolate is valid and local is from an eval exception.
                    let value = unsafe { v8::Local::from_ffi(self.isolate, local) };
                    Err(EvalError::Exception(value))
                }
                None => Err(EvalError::EvalFailed),
            }
        }
    }

    pub fn set_global(&self, name: &str, value: v8::Local<v8::Value>) {
        // SAFETY: self.inner is a valid EvalContext and value is a valid Local handle.
        unsafe { self.inner.set_global(name, value.into_ffi()) }
    }
}

impl Harness {
    pub fn new() -> Self {
        // SAFETY: create_test_harness initializes the V8 platform and returns a valid harness.
        Self(unsafe { ffi::create_test_harness() })
    }

    /// Runs a callback within a V8 context.
    ///
    /// The callback is passed through C++ via a data pointer since CXX doesn't support
    /// closures directly. The monomorphized trampoline function receives the pointer
    /// and reconstructs the closure.
    ///
    /// The callback returns `Result<(), jsg::Error>` to allow use of the `?` operator.
    /// If an error is returned, the test will panic.
    pub fn run_in_context<F>(&self, callback: F)
    where
        F: FnOnce(&mut jsg::Lock, &mut EvalContext) -> Result<(), jsg::Error>,
    {
        #[expect(clippy::needless_pass_by_value)]
        fn trampoline<F>(
            data: usize,
            isolate: *mut v8::ffi::Isolate,
            context: Pin<&mut ffi::EvalContext>,
        ) where
            F: FnOnce(&mut jsg::Lock, &mut EvalContext) -> Result<(), jsg::Error>,
        {
            // SAFETY: data was cast from &raw mut Option<F> in run_in_context below.
            let cb = unsafe { &mut *(data as *mut Option<F>) };
            if let Some(callback) = cb.take() {
                // SAFETY: isolate is a valid pointer provided by the C++ test harness.
                let isolate_ptr = unsafe { v8::IsolatePtr::from_ffi(isolate) };
                let mut eval_context = EvalContext {
                    inner: &context,
                    isolate: isolate_ptr,
                };
                // SAFETY: isolate is a valid pointer provided by the C++ test harness.
                let mut lock = unsafe { jsg::Lock::from_isolate_ptr(isolate) };
                if let Err(e) = callback(&mut lock, &mut eval_context) {
                    panic!("Test failed: {}: {}", e.name, e.message);
                }
            }
        }

        let mut callback = Some(callback);
        // SAFETY: callback pointer is valid for the duration of run_in_context.
        unsafe {
            self.0
                .run_in_context(&raw mut callback as usize, trampoline::<F>);
        }
    }

    pub fn request_gc(lock: &mut jsg::Lock) {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe { ffi::request_gc(lock.isolate().as_ffi(), ffi::GcType::Full) };
    }

    pub fn request_minor_gc(lock: &mut jsg::Lock) {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe { ffi::request_gc(lock.isolate().as_ffi(), ffi::GcType::Minor) };
    }

    /// Creates a V8 object tagged with the C++ `WORKERD_WRAPPABLE_TAG`.
    /// Used to test that Rust unwrap rejects non-Rust wrappable objects.
    pub fn create_cpp_tagged_object<'a>(lock: &mut jsg::Lock) -> v8::Local<'a, v8::Value> {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe {
            let local = ffi::create_cpp_tagged_object(lock.isolate().as_ffi());
            v8::Local::from_ffi(lock.isolate(), local)
        }
    }
}

impl Default for Harness {
    fn default() -> Self {
        Self::new()
    }
}
