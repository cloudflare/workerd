// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to the C++ that `ExecutionContext` uses (`ffi.h`), the C++ resources it takes and
//! returns, and the functions through which C++ creates and checks one.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and the C++ shims take the isolate and V8 \
              handles as raw values"
)]

use jsg::ToJS;
use jsg::v8;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;

use crate::ExecutionContext;

// Every C++ function takes the isolate, locked by this thread with a context entered and a
// `HandleScope` open, and handles in that scope.
#[cxx::bridge(namespace = "workerd::api::execution_context")]
pub mod bridge {
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

    #[namespace = "workerd::api"]
    unsafe extern "C++" {
        include!("workerd/api/execution-context/ffi.h");
        type Fetcher;
        type Tracing;
        type CacheContext;
        type AccessContext;
    }

    unsafe extern "C++" {
        include!("workerd/api/execution-context/ffi.h");

        // Per-type shims.
        unsafe fn fetcher_try_unwrap(
            isolate: *mut Isolate,
            value: Local,
        ) -> Result<KjMaybe<KjOwn<CppRef>>>;
        unsafe fn fetcher_wrap(isolate: *mut Isolate, cpp_ref: &CppRef) -> MaybeLocal;
        unsafe fn tracing_try_unwrap(
            isolate: *mut Isolate,
            value: Local,
        ) -> Result<KjMaybe<KjOwn<CppRef>>>;
        unsafe fn tracing_wrap(isolate: *mut Isolate, cpp_ref: &CppRef) -> MaybeLocal;
        unsafe fn cache_context_try_unwrap(
            isolate: *mut Isolate,
            value: Local,
        ) -> Result<KjMaybe<KjOwn<CppRef>>>;
        unsafe fn cache_context_wrap(isolate: *mut Isolate, cpp_ref: &CppRef) -> MaybeLocal;
        unsafe fn access_context_try_unwrap(
            isolate: *mut Isolate,
            value: Local,
        ) -> Result<KjMaybe<KjOwn<CppRef>>>;
        unsafe fn access_context_wrap(isolate: *mut Isolate, cpp_ref: &CppRef) -> MaybeLocal;

        // Per-method shims. Each returns false, with an exception pending, if the C++ threw.
        unsafe fn fetcher_map_virtual_host(
            isolate: *mut Isolate,
            fetcher: &Fetcher,
            port: u16,
            result: &mut String,
        ) -> bool;
        unsafe fn new_tracing(isolate: *mut Isolate, result: &mut KjMaybe<KjOwn<CppRef>>) -> bool;
        unsafe fn current_cache_context(
            isolate: *mut Isolate,
            result: &mut KjMaybe<KjOwn<CppRef>>,
        ) -> bool;
        unsafe fn current_access_context(
            isolate: *mut Isolate,
            result: &mut KjMaybe<KjOwn<CppRef>>,
        ) -> bool;
        /// The promise of the current entrypoint's `[restore]()`, or empty, with an exception
        /// pending, if it threw.
        unsafe fn restore_current_entrypoint(isolate: *mut Isolate, params: Local) -> MaybeLocal;
    }

    extern "Rust" {
        /// Creates an `ExecutionContext`. `version` is the value of `ctx.version`, if any.
        unsafe fn execution_context_new(
            isolate: *mut Isolate,
            exports: Local,
            props: Local,
            version: MaybeLocal,
        ) -> MaybeLocal;
        /// `jsg::resource::ffi_is::<ExecutionContext>`, for `RustExecutionContextTag::isInstance`.
        unsafe fn execution_context_is(isolate: *mut Isolate, value: Local) -> KjMaybe<bool>;
    }
}

/// Implements `jsg_cpp::CppResource` for a C++ class that the bridge declares, over its per-type
/// shims.
macro_rules! cpp_resource {
    ($type:ident, $class_name:literal, $try_unwrap:ident, $wrap:ident) => {
        // SAFETY: the shims instantiate cppRefTryUnwrap and cppRefWrap with the class.
        unsafe impl jsg_cpp::CppResource for bridge::$type {
            fn class_name() -> &'static str {
                $class_name
            }
            fn fully_qualified_name() -> &'static str {
                concat!("workerd::api::", $class_name)
            }
            unsafe fn try_unwrap(
                isolate: *mut v8::ffi::Isolate,
                value: v8::ffi::Local,
            ) -> Result<KjMaybe<KjOwn<jsg_cpp::CppRef>>, cxx::KjException> {
                // SAFETY: forwarded from the caller.
                unsafe { bridge::$try_unwrap(isolate, value) }
            }
            unsafe fn wrap(
                isolate: *mut v8::ffi::Isolate,
                cpp_ref: &jsg_cpp::CppRef,
            ) -> v8::ffi::MaybeLocal {
                // SAFETY: forwarded from the caller.
                unsafe { bridge::$wrap(isolate, cpp_ref) }
            }
        }
    };
}

cpp_resource!(Fetcher, "Fetcher", fetcher_try_unwrap, fetcher_wrap);
cpp_resource!(Tracing, "Tracing", tracing_try_unwrap, tracing_wrap);
cpp_resource!(
    CacheContext,
    "CacheContext",
    cache_context_try_unwrap,
    cache_context_wrap
);
cpp_resource!(
    AccessContext,
    "AccessContext",
    access_context_try_unwrap,
    access_context_wrap
);

impl bridge::Fetcher {
    /// `Fetcher::mapVirtualHost()`.
    pub fn map_virtual_host(&self, lock: &mut jsg::Lock<'_>, port: u16) -> jsg::Result<String> {
        let mut result = String::new();
        // SAFETY: `call_cpp` calls the shim with the lock's isolate.
        jsg_cpp::call_cpp(lock, |isolate| unsafe {
            bridge::fetcher_map_virtual_host(isolate, self, port, &mut result)
        })?;
        Ok(result)
    }
}

impl bridge::Tracing {
    /// A new `Tracing`, which is stateless.
    pub fn new(lock: &mut jsg::Lock<'_>) -> jsg::Result<jsg_cpp::Ref<Self>> {
        // SAFETY: the shim instantiates callCppForRef with Tracing.
        let tracing = unsafe {
            jsg_cpp::call_cpp_for_ref(lock, |isolate, result| bridge::new_tracing(isolate, result))
        }?;
        tracing.ok_or_else(|| jsg::Error::new_internal_error("new_tracing() returned nothing"))
    }
}

impl bridge::CacheContext {
    /// The `CacheContext` that the embedder provides for the current request
    /// (`Worker::Api::getCtxCacheProperty()`), or `None`, as outside a request.
    pub fn current(lock: &mut jsg::Lock<'_>) -> jsg::Result<Option<jsg_cpp::Ref<Self>>> {
        // SAFETY: the shim instantiates callCppForRef with CacheContext.
        unsafe {
            jsg_cpp::call_cpp_for_ref(lock, |isolate, result| {
                bridge::current_cache_context(isolate, result)
            })
        }
    }
}

impl bridge::AccessContext {
    /// The Cloudflare Access information of the current request, or `None`, as outside a request.
    pub fn current(lock: &mut jsg::Lock<'_>) -> jsg::Result<Option<jsg_cpp::Ref<Self>>> {
        // SAFETY: the shim instantiates callCppForRef with AccessContext.
        unsafe {
            jsg_cpp::call_cpp_for_ref(lock, |isolate, result| {
                bridge::current_access_context(isolate, result)
            })
        }
    }
}

/// Calls the current entrypoint's `[restore]()` with `params`, as `ctx.restore()` does
/// (`restoreCurrentEntrypoint()`).
pub fn restore_current_entrypoint<'s>(
    lock: &mut jsg::Lock<'s>,
    params: v8::Local<'s, v8::Object>,
) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    if lock.is_execution_terminating() {
        return Err(jsg::Error::pending());
    }
    // SAFETY: `lock` stands for the isolate being locked with a context entered and the
    // `HandleScope` `'s` open, and `params` is a handle in it.
    let promise =
        unsafe { bridge::restore_current_entrypoint(lock.isolate().as_ffi(), params.into_ffi()) };
    // SAFETY: the promise is a handle in the scope `'s`.
    unsafe { v8::Local::from_ffi_maybe(promise) }.ok_or_else(jsg::Error::pending)
}

/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open, and the
/// handles in that scope.
unsafe fn execution_context_new(
    isolate: *mut bridge::Isolate,
    exports: bridge::Local,
    props: bridge::Local,
    version: bridge::MaybeLocal,
) -> bridge::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            // SAFETY: the handles are in the caller's scope, which `lock` stands for.
            let exports = v8::Local::<v8::Value>::from_ffi(exports);
            let props = v8::Local::<v8::Value>::from_ffi(props);
            let version = v8::Local::<v8::Value>::from_ffi_maybe(version);
            let ctx = ExecutionContext::new(lock, exports, props, version);
            jsg::Rc::new(lock, ctx).to_js(lock)
        })
    }
}

/// # Safety
/// As for `execution_context_new`.
unsafe fn execution_context_is(
    isolate: *mut bridge::Isolate,
    value: bridge::Local,
) -> KjMaybe<bool> {
    // SAFETY: forwarded from the caller.
    unsafe { jsg::resource::ffi_is::<ExecutionContext>(isolate, value) }
}
