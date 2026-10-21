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

        pub unsafe fn eval(
            self: &EvalContext,
            code: &str,
            resource_name: KjMaybe<&str>,
        ) -> EvalResult;
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
}

#[derive(Debug)]
pub enum EvalError<'s> {
    UncoercibleResult {
        value: v8::Local<'s, v8::Value>,
        message: String,
    },
    Exception(v8::Local<'s, v8::Value>),
    EvalFailed,
}

impl<'s> EvalError<'s> {
    /// Extracts a `jsg::Error` from an `EvalError::Exception` variant.
    ///
    /// # Panics
    ///
    /// Panics if `self` is not `EvalError::Exception`, or if the value cannot be converted to a
    /// `jsg::Error`.
    pub fn unwrap_jsg_err(&self, lock: &mut jsg::Lock<'s>) -> jsg::Error {
        match self {
            EvalError::Exception(value) => jsg::Error::from_js(lock, *value)
                .expect("Failed to convert exception to jsg::Error"),
            _ => panic!("Unexpected error"),
        }
    }
}

impl EvalContext<'_> {
    /// Evaluates JavaScript code and converts the completion value to `T`.
    pub fn eval<'s, T>(&self, lock: &mut jsg::Lock<'s>, code: &str) -> Result<T, EvalError<'s>>
    where
        T: jsg::FromJS<'s, ResultType = T>,
    {
        let local = self.eval_raw(lock, code)?;
        T::from_js(lock, local).map_err(|e| EvalError::UncoercibleResult {
            value: local,
            message: e.to_string(),
        })
    }

    /// Evaluates JavaScript code and returns the raw `Local<Value>` without conversion.
    ///
    /// Useful for obtaining handles (e.g. functions) that aren't `FromJS` types.
    pub fn eval_raw<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        code: &str,
    ) -> Result<v8::Local<'s, v8::Value>, EvalError<'s>> {
        // SAFETY: self.inner is a valid EvalContext from C++; code is a valid str.
        let result = unsafe { self.inner.eval(code, None.into()) };
        Self::raw_result(lock, result)
    }

    /// Like [`EvalContext::eval_raw`], but compiles the code with a `ScriptOrigin` whose resource
    /// name is `resource_name`.
    pub fn eval_raw_named<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        code: &str,
        resource_name: &str,
    ) -> Result<v8::Local<'s, v8::Value>, EvalError<'s>> {
        // SAFETY: self.inner is a valid EvalContext from C++; code and resource_name are valid strs.
        let result = unsafe { self.inner.eval(code, Some(resource_name).into()) };
        Self::raw_result(lock, result)
    }

    fn raw_result<'s>(
        lock: &jsg::Lock<'s>,
        result: ffi::EvalResult,
    ) -> Result<v8::Local<'s, v8::Value>, EvalError<'s>> {
        let opt_local: Option<v8::ffi::Local> = result.value.into();
        let isolate = lock.isolate();

        if result.success {
            match opt_local {
                // SAFETY: the C++ harness evaluates in `lock`'s HandleScope, so the completion
                // value lives in scope `'s`.
                Some(local) => Ok(unsafe { v8::Local::from_ffi(isolate, local) }),
                None => unreachable!(),
            }
        } else {
            match opt_local {
                Some(local) => {
                    // SAFETY: as above, for the exception value.
                    let value = unsafe { v8::Local::from_ffi(isolate, local) };
                    Err(EvalError::Exception(value))
                }
                None => Err(EvalError::EvalFailed),
            }
        }
    }

    /// Converts `value` to JavaScript and stores it as the global `name`.
    pub fn set_global<'s>(&self, lock: &mut jsg::Lock<'s>, name: &str, value: impl jsg::ToJS<'s>) {
        let value = value.to_js(lock);
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
    /// The callback receives a [`jsg::Lock`] for the harness's top-level `HandleScope`;
    /// handles it creates cannot outlive the callback.
    ///
    /// The callback returns `Result<(), jsg::Error>` to allow use of the `?` operator.
    /// If an error is returned, the test will panic. A panic in the callback is caught before
    /// it reaches C++ and resumed once `run_in_context` returns.
    pub fn run_in_context<F>(&self, callback: F)
    where
        F: for<'s> FnOnce(&mut jsg::Lock<'s>, &mut EvalContext<'_>) -> Result<(), jsg::Error>,
    {
        struct Call<F> {
            callback: Option<F>,
            panic: Option<Box<dyn std::any::Any + Send>>,
        }

        #[expect(clippy::needless_pass_by_value)]
        fn trampoline<F>(
            data: usize,
            isolate: *mut v8::ffi::Isolate,
            context: Pin<&mut ffi::EvalContext>,
        ) where
            F: for<'s> FnOnce(&mut jsg::Lock<'s>, &mut EvalContext<'_>) -> Result<(), jsg::Error>,
        {
            // SAFETY: data was cast from &raw mut Call<F> in run_in_context below.
            let call = unsafe { &mut *(data as *mut Call<F>) };
            let Some(callback) = call.callback.take() else {
                return;
            };
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let mut eval_context = EvalContext { inner: &context };
                // SAFETY: the C++ harness calls this with the isolate locked, a context
                // entered and a HandleScope open for the duration of the call.
                jsg::callback_scope!(unsafe let lock, isolate);
                if let Err(e) = callback(lock, &mut eval_context) {
                    panic!("Test failed: {}: {}", e.name, e.message);
                }
            }));
            if let Err(payload) = result {
                call.panic = Some(payload);
            }
        }

        let mut call = Call {
            callback: Some(callback),
            panic: None,
        };
        // SAFETY: `call` is valid for the duration of run_in_context.
        unsafe {
            self.0
                .run_in_context(&raw mut call as usize, trampoline::<F>);
        }
        if let Some(payload) = call.panic {
            std::panic::resume_unwind(payload);
        }
    }

    pub fn request_gc(lock: &mut jsg::Lock<'_>) {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe { ffi::request_gc(lock.isolate().as_ffi(), ffi::GcType::Full) };
    }

    pub fn request_minor_gc(lock: &mut jsg::Lock<'_>) {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe { ffi::request_gc(lock.isolate().as_ffi(), ffi::GcType::Minor) };
    }

    /// Creates a V8 object tagged with the C++ `WORKERD_WRAPPABLE_TAG`.
    /// Used to test that Rust unwrap rejects non-Rust wrappable objects.
    pub fn create_cpp_tagged_object<'s>(lock: &mut jsg::Lock<'s>) -> v8::Local<'s, v8::Value> {
        let isolate = lock.isolate();
        // SAFETY: `lock` proves the isolate is locked and a HandleScope for `'s` is open.
        unsafe {
            let local = ffi::create_cpp_tagged_object(isolate.as_ffi());
            v8::Local::from_ffi(isolate, local)
        }
    }
}

impl Default for Harness {
    fn default() -> Self {
        Self::new()
    }
}
