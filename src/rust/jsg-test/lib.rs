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

    #[derive(Debug)]
    struct EvalResult {
        success: bool,
        value: KjMaybe<Local>,
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
    }
}

pub struct Harness(KjOwn<ffi::TestHarness>);

pub struct EvalContext<'a> {
    inner: &'a ffi::EvalContext,
    isolate: v8::IsolatePtr,
}

impl EvalContext<'_> {
    // TODO(soon): There is no way to distinguish a throw versus an error as a return value.
    pub fn eval<T>(&self, lock: &mut jsg::Lock, code: &str) -> Result<T, jsg::Error>
    where
        T: jsg::FromJS<ResultType = T>,
    {
        let result = unsafe { self.inner.eval(code) };
        let opt_local: Option<v8::ffi::Local> = result.value.into();

        if result.success {
            match opt_local {
                Some(local) => {
                    T::from_js(lock, unsafe { v8::Local::from_ffi(self.isolate, local) })
                }
                None => Err(jsg::Error::new(
                    "Error",
                    "eval returned empty result".to_owned(),
                )),
            }
        } else {
            match opt_local {
                Some(local) => {
                    let value = unsafe { v8::Local::from_ffi(self.isolate, local) };
                    Err(jsg::Error::from_value(lock, value))
                }
                None => Err(jsg::Error::new("Error", "eval failed".to_owned())),
            }
        }
    }

    pub fn set_global(&self, name: &str, value: v8::Local<v8::Value>) {
        unsafe { self.inner.set_global(name, value.into_ffi()) }
    }
}

impl Harness {
    pub fn new() -> Self {
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
            let cb = unsafe { &mut *(data as *mut Option<F>) };
            if let Some(callback) = cb.take() {
                let isolate_ptr = unsafe { v8::IsolatePtr::from_ffi(isolate) };
                let mut eval_context = EvalContext {
                    inner: &context,
                    isolate: isolate_ptr,
                };
                let mut lock = unsafe { jsg::Lock::from_isolate_ptr(isolate) };
                if let Err(e) = callback(&mut lock, &mut eval_context) {
                    panic!("Test failed: {}: {}", e.name, e.message);
                }
            }
        }

        let mut callback = Some(callback);
        unsafe {
            self.0
                .run_in_context(&raw mut callback as usize, trampoline::<F>);
        }
    }
}

impl Default for Harness {
    fn default() -> Self {
        Self::new()
    }
}
