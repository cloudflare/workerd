// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and its shims take C++'s raw isolate"
)]

//! The bridge through which C++ creates a [`Hyperdrive`](crate::Hyperdrive), and the C++ it
//! calls (`ffi.h`): the per-type shims of `Socket` and the per-method shims, wrapped here in safe
//! functions that take the `Lock`.

use jsg::ToJS;
use jsg::v8;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;

// Every function that takes an isolate takes it locked by this thread with a context entered
// and a `HandleScope` open, and handles in that scope.
#[cxx::bridge(namespace = "workerd::api")]
mod bridge {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = jsg::v8::ffi::Isolate;
        type Local = jsg::v8::ffi::Local;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    #[namespace = "workerd::rust::jsg_cpp"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg-cpp/jsg-cpp.h");

        type CppRef = jsg_cpp::CppRef;
    }

    unsafe extern "C++" {
        include!("workerd/api/hyperdrive/ffi.h");

        /// `api::Socket`, which `connect()` returns.
        type Socket;

        unsafe fn socket_try_unwrap(
            isolate: *mut Isolate,
            value: Local,
        ) -> Result<KjMaybe<KjOwn<CppRef>>>;
        unsafe fn socket_wrap(isolate: *mut Isolate, cpp_ref: &CppRef) -> MaybeLocal;

        fn hyperdrive_fill_entropy(output: &mut [u8]) -> Result<()>;
        unsafe fn hyperdrive_has_connect_override(
            isolate: *mut Isolate,
            address: &str,
            result: &mut bool,
        ) -> bool;
        unsafe fn hyperdrive_register_overrides(
            isolate: *mut Isolate,
            channel: u32,
            host: &str,
            ip: &str,
            port: u16,
        ) -> bool;
        unsafe fn hyperdrive_connect(
            isolate: *mut Isolate,
            channel: u32,
            host: &str,
            port: u16,
            result: &mut KjMaybe<KjOwn<CppRef>>,
        ) -> bool;
    }

    extern "Rust" {
        /// A new `Hyperdrive` for the binding whose subrequest channel is `channel`.
        unsafe fn hyperdrive_new(
            isolate: *mut Isolate,
            channel: u32,
            database: &str,
            user: &str,
            password: &str,
            scheme: &str,
        ) -> MaybeLocal;
    }
}

pub use bridge::Socket;

// SAFETY: the shims instantiate cppRefTryUnwrap and cppRefWrap with api::Socket.
unsafe impl jsg_cpp::CppResource for Socket {
    fn class_name() -> &'static str {
        "Socket"
    }

    unsafe fn try_unwrap(
        isolate: *mut v8::ffi::Isolate,
        value: v8::ffi::Local,
    ) -> Result<KjMaybe<KjOwn<jsg_cpp::CppRef>>, cxx::KjException> {
        // SAFETY: forwarded from the caller.
        unsafe { bridge::socket_try_unwrap(isolate, value) }
    }

    unsafe fn wrap(
        isolate: *mut v8::ffi::Isolate,
        cpp_ref: &jsg_cpp::CppRef,
    ) -> v8::ffi::MaybeLocal {
        // SAFETY: forwarded from the caller.
        unsafe { bridge::socket_wrap(isolate, cpp_ref) }
    }
}

/// Fills `output` with random bytes, from the same source as C++ `getEntropy()`.
pub fn fill_entropy(output: &mut [u8]) -> Result<(), jsg::Error> {
    bridge::hyperdrive_fill_entropy(output)
        .map_err(|err| jsg::Error::new_internal_error(format!("getEntropy() failed: {err}")))
}

/// Whether the global scope of the current request has a connect override for `address`.
pub fn has_connect_override(lock: &mut jsg::Lock<'_>, address: &str) -> Result<bool, jsg::Error> {
    let mut result = false;
    // SAFETY: `call_cpp` calls the shim with the lock's isolate.
    jsg_cpp::call_cpp(lock, |isolate| unsafe {
        bridge::hyperdrive_has_connect_override(isolate, address, &mut result)
    })?;
    Ok(result)
}

/// Routes `host:port` and `ip:port` to the database of the binding whose subrequest channel is
/// `channel`, and resolves `host` to `ip`, for the current request's global scope.
pub fn register_overrides(
    lock: &mut jsg::Lock<'_>,
    channel: u32,
    host: &str,
    ip: &str,
    port: u16,
) -> Result<(), jsg::Error> {
    // SAFETY: as in `has_connect_override`.
    jsg_cpp::call_cpp(lock, |isolate| unsafe {
        bridge::hyperdrive_register_overrides(isolate, channel, host, ip, port)
    })
}

/// A socket connected to `host:port` through the subrequest channel `channel`.
pub fn connect(
    lock: &mut jsg::Lock<'_>,
    channel: u32,
    host: &str,
    port: u16,
) -> Result<jsg_cpp::Ref<Socket>, jsg::Error> {
    // SAFETY: the shim instantiates callCppForRef with api::Socket.
    let socket = unsafe {
        jsg_cpp::call_cpp_for_ref::<Socket>(lock, |isolate, result| {
            bridge::hyperdrive_connect(isolate, channel, host, port, result)
        })
    }?;
    socket.ok_or_else(|| jsg::Error::new_internal_error("connectHyperdrive() returned no socket"))
}

/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open.
unsafe fn hyperdrive_new(
    isolate: *mut bridge::Isolate,
    channel: u32,
    database: &str,
    user: &str,
    password: &str,
    scheme: &str,
) -> bridge::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            let hyperdrive = crate::Hyperdrive::new(channel, database, user, password, scheme)?;
            jsg::Rc::new(hyperdrive).to_js(lock)
        })
    }
}
