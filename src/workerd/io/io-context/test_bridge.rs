// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The Rust half of `io-context-test.c++`, which calls these functions inside and outside a
//! request of a real worker.
//!
//! Those that take an `isolate` take the `v8::Isolate*` of the caller: locked by the calling
//! thread, with a context entered and a `v8::HandleScope` open.

use std::cell::Cell;
use std::pin::Pin;
use std::time::UNIX_EPOCH;

use io_context::IoContext;
use jsg::v8;

/// C++ receives an error as an exception whose description is the `jsg::Error`'s name and
/// message.
type Result<T> = std::io::Result<T>;

fn to_io(err: &jsg::Error) -> std::io::Error {
    std::io::Error::other(err.to_string())
}

#[cxx::bridge(namespace = "workerd::io_context::test")]
mod ffi {
    /// What the closure of the module `register_probe_module()` registers saw when it last ran.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum ProbeObservation {
        NotRun,
        NoRequest,
        Request,
    }

    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type ModuleRegistry = jsg::v8::ffi::ModuleRegistry;
    }

    extern "Rust" {
        fn register_probe_module(registry: Pin<&mut ModuleRegistry>);
        fn take_probe_observation() -> ProbeObservation;

        unsafe fn has_current(isolate: usize) -> bool;
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

/// `IoContext::current()?.now()`, in nanoseconds since the Unix epoch.
unsafe fn now_nanos(isolate: usize) -> Result<i64> {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    let now = IoContext::current(lock)
        .and_then(|context| context.now())
        .map_err(|err| to_io(&err))?;
    let nanos = now
        .duration_since(UNIX_EPOCH)
        .map_err(std::io::Error::other)?
        .as_nanos();
    i64::try_from(nanos).map_err(std::io::Error::other)
}

/// `IoContext::current()?.write_logfwdr(channel, message)`.
unsafe fn write_logfwdr(isolate: usize, channel: u32, message: &[u8]) -> Result<()> {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    IoContext::current(lock)
        .and_then(|context| context.write_logfwdr(channel, message))
        .map_err(|err| to_io(&err))
}

thread_local! {
    static PROBE_OBSERVATION: Cell<ffi::ProbeObservation> =
        const { Cell::new(ffi::ProbeObservation::NotRun) };
}

/// The exports of `test:io-context-probe`, which has no members. It is in a module of its own
/// because the code `#[jsg_resource]` generates names `Result`, which this file redefines.
mod probe {
    #[jsg_macros::jsg_resource]
    pub struct Probe;

    #[jsg_macros::jsg_resource]
    impl Probe {}
}

/// Registers the internal module `test:io-context-probe`, whose closure records whether it sees a
/// request.
fn register_probe_module(registry: Pin<&mut ffi::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry,
        "test:io-context-probe",
        |lock| {
            let observation = if IoContext::try_current(lock).is_some() {
                ffi::ProbeObservation::Request
            } else {
                ffi::ProbeObservation::NoRequest
            };
            PROBE_OBSERVATION.with(|cell| cell.set(observation));
            Ok(jsg::Rc::new(probe::Probe))
        },
        jsg::modules::ModuleType::Internal,
    );
}

/// What the probe module's closure saw when it last ran, which is then forgotten.
fn take_probe_observation() -> ffi::ProbeObservation {
    PROBE_OBSERVATION.with(|cell| cell.replace(ffi::ProbeObservation::NotRun))
}
