// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The Rust half of `io-context-test.c++`, which calls these functions inside and outside a
//! request of a real worker.
//!
//! Each takes the `v8::Isolate*` of the caller as `isolate`: locked by the calling thread, with a
//! context entered and a `v8::HandleScope` open.

use std::time::UNIX_EPOCH;

use io_context::IoContext;
use jsg::v8;

/// C++ receives an error as an exception whose description is the `jsg::Error`'s name and
/// message.
type Result<T> = std::io::Result<T>;

fn to_io(err: jsg::Error) -> std::io::Error {
    std::io::Error::other(err.to_string())
}

#[cxx::bridge(namespace = "workerd::io_context::test")]
mod ffi {
    extern "Rust" {
        unsafe fn has_current(isolate: usize) -> bool;
        unsafe fn current_error(isolate: usize) -> String;
        unsafe fn now_nanos(isolate: usize) -> Result<i64>;
        unsafe fn write_logfwdr(isolate: usize, channel: u32, message: &[u8]) -> Result<()>;
    }
}

/// Whether `IoContext::try_current()` finds a request.
unsafe fn has_current(isolate: usize) -> bool {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    IoContext::try_current(lock).is_some()
}

/// The message of the error `IoContext::current()` fails with, or "" if it succeeds.
unsafe fn current_error(isolate: usize) -> String {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    IoContext::current(lock)
        .err()
        .map(|err| err.message)
        .unwrap_or_default()
}

/// `IoContext::current()?.now()`, in nanoseconds since the Unix epoch.
unsafe fn now_nanos(isolate: usize) -> Result<i64> {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    let now = IoContext::current(lock)
        .and_then(|context| context.now())
        .map_err(to_io)?;
    let nanos = now.duration_since(UNIX_EPOCH).unwrap().as_nanos();
    Ok(i64::try_from(nanos).unwrap())
}

/// `IoContext::current()?.write_logfwdr(channel, message)`.
unsafe fn write_logfwdr(isolate: usize, channel: u32, message: &[u8]) -> Result<()> {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    IoContext::current(lock)
        .and_then(|context| context.write_logfwdr(channel, message))
        .map_err(to_io)
}
