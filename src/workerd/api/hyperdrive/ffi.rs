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
        unsafe fn hyperdrive_register_overrides(
            isolate: *mut Isolate,
            channel: u32,
            host: &str,
            port: u16,
            ip: &mut String,
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
        /// A new `Hyperdrive` for the binding whose subrequest channel is `channel`. The strings
        /// are the config's bytes, which Cap'n Proto does not check are UTF-8.
        unsafe fn hyperdrive_new(
            isolate: *mut Isolate,
            channel: u32,
            database: &[u8],
            user: &[u8],
            password: &[u8],
            scheme: &[u8],
        ) -> MaybeLocal;
    }
}

pub use bridge::Socket;

// SAFETY: the shims instantiate cppRefTryUnwrap and cppRefWrap with api::Socket.
unsafe impl jsg_cpp::CppResource for Socket {
    fn class_name() -> &'static str {
        "Socket"
    }

    fn fully_qualified_name() -> &'static str {
        "workerd::api::Socket"
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

/// Claims a synthetic IPv4 address for the binding whose subrequest channel is `channel` and whose
/// host is `host`, routes `host:port` and the address's `:port` to its database, resolves `host`
/// to the address, for the current request's global scope, and returns the address. C++
/// `registerHyperdriveOverrides()` says how the address is chosen.
pub fn register_overrides(
    lock: &mut jsg::Lock<'_>,
    channel: u32,
    host: &str,
    port: u16,
) -> Result<String, jsg::Error> {
    let mut ip = String::new();
    // SAFETY: `call_cpp` calls the shim with the lock's isolate.
    jsg_cpp::call_cpp(lock, |isolate| unsafe {
        bridge::hyperdrive_register_overrides(isolate, channel, host, port, &mut ip)
    })?;
    Ok(ip)
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
    database: &[u8],
    user: &[u8],
    password: &[u8],
    scheme: &[u8],
) -> bridge::MaybeLocal {
    // Each invalid sequence becomes U+FFFD, as it does where V8 makes a string of the C++
    // Hyperdrive's bytes, so that both implementations accept the same configs and show them the
    // same way.
    let text = String::from_utf8_lossy;
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            let hyperdrive = crate::Hyperdrive::new(
                channel,
                &text(database),
                &text(user),
                &text(password),
                &text(scheme),
            )?;
            jsg::Rc::new(lock, hyperdrive).to_js(lock)
        })
    }
}
