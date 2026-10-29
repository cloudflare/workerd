// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Rust and C++ resources that use each other through `jsg_cpp`, for `tests/cpp_interop.rs`.
//!
//! This is what a crate that ports an API with C++ consumers holds: the bridge declares the C++
//! classes it uses, their per-type and per-method shims (`cpp-interop.h`), and the `extern
//! "Rust"` functions through which C++ creates, checks and calls the Rust resource. `Host` plays
//! the part of `ExecutionContext`: it takes a C++ resource as an argument, returns one from a
//! lazy property that depends on the current request, and is held by a C++ resource.

use std::cell::Cell;
use std::cell::RefCell;

use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;

// Every function takes the isolate, locked by this thread with a context entered and a
// `HandleScope` open, and handles in that scope.
#[expect(
    clippy::missing_safety_doc,
    reason = "the contract of every function is the one above"
)]
#[cxx::bridge(namespace = "workerd::rust::jsg_test")]
pub mod ffi {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        type Isolate = jsg::v8::ffi::Isolate;
        type Local = jsg::v8::ffi::Local;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    #[namespace = "workerd::rust::jsg_cpp"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg-cpp/jsg-cpp.h");
        type CppRef = jsg_cpp::CppRef;
    }

    #[namespace = "workerd::cpp_interop_test"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg-test/cpp-interop.h");
        type TestCppGreeter;
        type TestCppBase;
        type TestCppHolder;
    }

    unsafe extern "C++" {
        include!("workerd/rust/jsg-test/cpp-interop.h");

        // Per-type shims.
        unsafe fn test_cpp_greeter_try_unwrap(
            isolate: *mut Isolate,
            value: Local,
        ) -> Result<KjMaybe<KjOwn<CppRef>>>;
        unsafe fn test_cpp_greeter_wrap(isolate: *mut Isolate, cpp_ref: &CppRef) -> MaybeLocal;
        unsafe fn test_cpp_base_try_unwrap(
            isolate: *mut Isolate,
            value: Local,
        ) -> Result<KjMaybe<KjOwn<CppRef>>>;
        unsafe fn test_cpp_base_wrap(isolate: *mut Isolate, cpp_ref: &CppRef) -> MaybeLocal;
        unsafe fn test_cpp_holder_try_unwrap(
            isolate: *mut Isolate,
            value: Local,
        ) -> Result<KjMaybe<KjOwn<CppRef>>>;
        unsafe fn test_cpp_holder_wrap(isolate: *mut Isolate, cpp_ref: &CppRef) -> MaybeLocal;

        // Per-method shims.
        unsafe fn test_cpp_greeter_greet(
            isolate: *mut Isolate,
            greeter: &TestCppGreeter,
            name: &str,
            result: &mut String,
        ) -> bool;
        unsafe fn test_cpp_greeter_fail_with_type_error(
            isolate: *mut Isolate,
            greeter: &TestCppGreeter,
        ) -> bool;
        unsafe fn test_cpp_greeter_fail_with_js_error(
            isolate: *mut Isolate,
            greeter: &TestCppGreeter,
        ) -> bool;
        unsafe fn test_cpp_base_kind(
            isolate: *mut Isolate,
            base: &TestCppBase,
            result: &mut String,
        ) -> bool;
        unsafe fn test_cpp_current_base(
            isolate: *mut Isolate,
            result: &mut KjMaybe<KjOwn<CppRef>>,
        ) -> bool;

        // Test controls and constructors.
        fn test_cpp_set_in_request(in_request: bool);
        fn test_cpp_holders_destroyed() -> u32;
        unsafe fn test_cpp_greeter_new(isolate: *mut Isolate, greeting: &str) -> MaybeLocal;
        unsafe fn test_cpp_derived_new(isolate: *mut Isolate) -> MaybeLocal;
        unsafe fn test_cpp_holder_new(isolate: *mut Isolate) -> MaybeLocal;
        unsafe fn test_cpp_holder_new_holding_host(
            isolate: *mut Isolate,
            label: &str,
        ) -> MaybeLocal;
    }

    extern "Rust" {
        /// Creates a `Host`, as C++ creates an `ExecutionContext` for a request.
        unsafe fn host_new(isolate: *mut Isolate, label: &str) -> MaybeLocal;
        /// `jsg::resource::ffi_is::<Host>`, for `HostTag::isInstance`.
        unsafe fn host_is(isolate: *mut Isolate, value: Local) -> KjMaybe<bool>;
        /// A method of `Host` that C++ calls, given the wrapper.
        unsafe fn host_describe(isolate: *mut Isolate, host: Local) -> MaybeLocal;
    }
}

// SAFETY: the shims instantiate cppRefTryUnwrap and cppRefWrap with TestCppGreeter.
unsafe impl jsg_cpp::CppResource for ffi::TestCppGreeter {
    fn class_name() -> &'static str {
        "TestCppGreeter"
    }
    unsafe fn try_unwrap(
        isolate: *mut v8::ffi::Isolate,
        value: v8::ffi::Local,
    ) -> Result<KjMaybe<KjOwn<jsg_cpp::CppRef>>, cxx::KjException> {
        // SAFETY: forwarded from the caller.
        unsafe { ffi::test_cpp_greeter_try_unwrap(isolate, value) }
    }
    unsafe fn wrap(
        isolate: *mut v8::ffi::Isolate,
        cpp_ref: &jsg_cpp::CppRef,
    ) -> v8::ffi::MaybeLocal {
        // SAFETY: forwarded from the caller.
        unsafe { ffi::test_cpp_greeter_wrap(isolate, cpp_ref) }
    }
}

// SAFETY: the shims instantiate cppRefTryUnwrap and cppRefWrap with TestCppBase.
unsafe impl jsg_cpp::CppResource for ffi::TestCppBase {
    fn class_name() -> &'static str {
        "TestCppBase"
    }
    unsafe fn try_unwrap(
        isolate: *mut v8::ffi::Isolate,
        value: v8::ffi::Local,
    ) -> Result<KjMaybe<KjOwn<jsg_cpp::CppRef>>, cxx::KjException> {
        // SAFETY: forwarded from the caller.
        unsafe { ffi::test_cpp_base_try_unwrap(isolate, value) }
    }
    unsafe fn wrap(
        isolate: *mut v8::ffi::Isolate,
        cpp_ref: &jsg_cpp::CppRef,
    ) -> v8::ffi::MaybeLocal {
        // SAFETY: forwarded from the caller.
        unsafe { ffi::test_cpp_base_wrap(isolate, cpp_ref) }
    }
}

// SAFETY: the shims instantiate cppRefTryUnwrap and cppRefWrap with TestCppHolder.
unsafe impl jsg_cpp::CppResource for ffi::TestCppHolder {
    fn class_name() -> &'static str {
        "TestCppHolder"
    }
    unsafe fn try_unwrap(
        isolate: *mut v8::ffi::Isolate,
        value: v8::ffi::Local,
    ) -> Result<KjMaybe<KjOwn<jsg_cpp::CppRef>>, cxx::KjException> {
        // SAFETY: forwarded from the caller.
        unsafe { ffi::test_cpp_holder_try_unwrap(isolate, value) }
    }
    unsafe fn wrap(
        isolate: *mut v8::ffi::Isolate,
        cpp_ref: &jsg_cpp::CppRef,
    ) -> v8::ffi::MaybeLocal {
        // SAFETY: forwarded from the caller.
        unsafe { ffi::test_cpp_holder_wrap(isolate, cpp_ref) }
    }
}

// Typed methods over the per-method shims, as a binding crate writes them.
impl ffi::TestCppGreeter {
    pub fn greet(&self, lock: &mut jsg::Lock<'_>, name: &str) -> Result<String, jsg::Error> {
        let mut result = String::new();
        // SAFETY: `call_cpp` calls the shim with the lock's isolate.
        jsg_cpp::call_cpp(lock, |isolate| unsafe {
            ffi::test_cpp_greeter_greet(isolate, self, name, &mut result)
        })?;
        Ok(result)
    }

    pub fn fail_with_type_error(&self, lock: &mut jsg::Lock<'_>) -> Result<(), jsg::Error> {
        // SAFETY: as in `greet`.
        jsg_cpp::call_cpp(lock, |isolate| unsafe {
            ffi::test_cpp_greeter_fail_with_type_error(isolate, self)
        })
    }

    pub fn fail_with_js_error(&self, lock: &mut jsg::Lock<'_>) -> Result<(), jsg::Error> {
        // SAFETY: as in `greet`.
        jsg_cpp::call_cpp(lock, |isolate| unsafe {
            ffi::test_cpp_greeter_fail_with_js_error(isolate, self)
        })
    }
}

impl ffi::TestCppBase {
    pub fn kind(&self, lock: &mut jsg::Lock<'_>) -> Result<String, jsg::Error> {
        let mut result = String::new();
        // SAFETY: as in `TestCppGreeter::greet`.
        jsg_cpp::call_cpp(lock, |isolate| unsafe {
            ffi::test_cpp_base_kind(isolate, self, &mut result)
        })?;
        Ok(result)
    }

    /// The base of the current simulated request, as `ctx.cache` reads the `CacheContext` of the
    /// current request through `Worker::Api::getCtxCacheProperty()`.
    pub fn current(lock: &mut jsg::Lock<'_>) -> Result<Option<jsg_cpp::Ref<Self>>, jsg::Error> {
        // SAFETY: the shim instantiates callCppForRef with TestCppBase.
        unsafe {
            jsg_cpp::call_cpp_for_ref(lock, |isolate, result| {
                ffi::test_cpp_current_base(isolate, result)
            })
        }
    }
}

thread_local! {
    pub static HOST_DROPS: Cell<u32> = const { Cell::new(0) };
}

/// A Rust resource that uses C++ resources, as a ported `ExecutionContext` would.
#[jsg_resource]
pub struct Host {
    pub label: String,
    /// A C++ resource, traced as a `jsg::Ref` field of a C++ resource is.
    pub held: RefCell<Option<jsg_cpp::Ref<ffi::TestCppHolder>>>,
}

impl Drop for Host {
    fn drop(&mut self) {
        HOST_DROPS.with(|drops| drops.set(drops.get() + 1));
    }
}

#[jsg_resource]
impl Host {
    /// Takes a C++ resource and calls one of its methods, as `mapVirtualHost(fetcher, port)` does.
    #[jsg_method]
    pub fn greet_with<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        greeter: jsg_cpp::Ref<ffi::TestCppGreeter>,
        name: String,
    ) -> Result<String, jsg::Error> {
        greeter.greet(lock, &name)
    }

    /// Takes a C++ resource of a class with a subclass, and calls a virtual method.
    #[jsg_method]
    pub fn kind_of<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        base: jsg_cpp::Ref<ffi::TestCppBase>,
    ) -> Result<String, jsg::Error> {
        base.kind(lock)
    }

    /// Returns the C++ resource of the current request, as `ctx.cache` does.
    #[jsg_property(lazy_instance, readonly)]
    pub fn get_current<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
    ) -> Result<Option<jsg_cpp::Ref<ffi::TestCppBase>>, jsg::Error> {
        ffi::TestCppBase::current(lock)
    }

    #[jsg_method]
    pub fn fail_with_type_error<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        greeter: jsg_cpp::Ref<ffi::TestCppGreeter>,
    ) -> Result<(), jsg::Error> {
        greeter.fail_with_type_error(lock)
    }

    #[jsg_method]
    pub fn fail_with_js_error<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        greeter: jsg_cpp::Ref<ffi::TestCppGreeter>,
    ) -> Result<(), jsg::Error> {
        greeter.fail_with_js_error(lock)
    }

    /// Keeps a C++ resource in a traced field.
    #[jsg_method]
    pub fn hold(&self, holder: jsg_cpp::Ref<ffi::TestCppHolder>) {
        *self.held.borrow_mut() = Some(holder);
    }

    /// The held C++ resource, the same JavaScript object `hold()` was given.
    #[jsg_method]
    pub fn held(&self) -> Option<jsg_cpp::Ref<ffi::TestCppHolder>> {
        self.held.borrow().clone()
    }
}

/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open.
unsafe fn host_new(isolate: *mut ffi::Isolate, label: &str) -> ffi::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            jsg::Rc::new(Host {
                label: label.to_owned(),
                held: RefCell::new(None),
            })
            .to_js(lock)
        })
    }
}

/// # Safety
/// As for `host_new`, and `value` is a handle in the caller's scope.
unsafe fn host_is(isolate: *mut ffi::Isolate, value: ffi::Local) -> KjMaybe<bool> {
    // SAFETY: forwarded from the caller.
    unsafe { jsg::resource::ffi_is::<Host>(isolate, value) }
}

/// # Safety
/// As for `host_is`.
unsafe fn host_describe(isolate: *mut ffi::Isolate, host: ffi::Local) -> ffi::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            // SAFETY: `host` is a handle in the caller's scope, which `lock` stands for.
            let host = v8::Local::<v8::Value>::from_ffi(host);
            let host = <jsg::Rc<Host> as jsg::FromJS>::from_js(lock, host)?;
            format!("Host({})", host.label).to_js(lock)
        })
    }
}
