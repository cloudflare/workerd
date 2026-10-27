// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The Rust half of `io-context-test.c++`, which calls these functions inside and outside a
//! request of a real worker.
//!
//! Those that take an `isolate` take the `v8::Isolate*` of the caller: locked by the calling
//! thread, with a context entered and a `v8::HandleScope` open.

use std::cell::Cell;
use std::cell::RefCell;
use std::pin::Pin;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::time::Duration;
use std::time::UNIX_EPOCH;

use io_context::IoContext;
use io_context::IoOwned;
use jsg::ToJS;
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

    /// What the callback of the last `await_io()` started by `await_from_thread()` or
    /// `await_forever()` did.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum AwaitObservation {
        /// It has not run, and still exists.
        Pending,
        /// It ran in a request, with the value the future produced.
        RanInRequest,
        /// It ran outside any request.
        RanOutsideRequest,
        /// It was dropped without running.
        Dropped,
    }

    unsafe extern "C++" {
        include!("workerd/io/io-context/test-bridge.h");

        fn record_async_context(isolate: usize);
    }

    extern "Rust" {
        unsafe fn await_and_record_async_context(isolate: usize) -> Result<()>;
        fn register_probe_module(registry: Pin<&mut ModuleRegistry>);
        fn take_probe_observation() -> ProbeObservation;
        unsafe fn await_from_thread(isolate: usize, value: i32) -> Result<()>;
        unsafe fn await_forever(isolate: usize) -> Result<()>;
        fn await_observation() -> AwaitObservation;
        fn awaited_value() -> i32;
        unsafe fn add_owned_value(isolate: usize) -> Result<()>;
        fn owned_value_dropped() -> bool;
        fn take_owned_value() -> bool;
        fn drop_owned_handle();

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
            Vec::<f64>::new().to_js(lock)
        },
        jsg::modules::ModuleType::Internal,
    );
}

/// What the probe module's closure saw when it last ran, which is then forgotten.
fn take_probe_observation() -> ffi::ProbeObservation {
    PROBE_OBSERVATION.with(|cell| cell.replace(ffi::ProbeObservation::NotRun))
}

thread_local! {
    static AWAIT_OBSERVATION: Cell<ffi::AwaitObservation> =
        const { Cell::new(ffi::AwaitObservation::Pending) };
    static AWAITED_VALUE: Cell<i32> = const { Cell::new(0) };
    static OWNED: RefCell<Option<IoOwned<DropFlag>>> = const { RefCell::new(None) };
}

/// Sets `AWAIT_OBSERVATION` to `Dropped` if dropped while it is still `Pending`, that is, if the
/// callback holding it never ran.
struct DropObserver;

impl Drop for DropObserver {
    fn drop(&mut self) {
        AWAIT_OBSERVATION.with(|cell| {
            if cell.get() == ffi::AwaitObservation::Pending {
                cell.set(ffi::AwaitObservation::Dropped);
            }
        });
    }
}

fn record_await(lock: &mut jsg::Lock<'_>, value: i32) {
    let observation = if IoContext::try_current(lock).is_some() {
        ffi::AwaitObservation::RanInRequest
    } else {
        ffi::AwaitObservation::RanOutsideRequest
    };
    AWAITED_VALUE.with(|cell| cell.set(value));
    AWAIT_OBSERVATION.with(|cell| cell.set(observation));
}

/// In a request, awaits a value that another thread sends after a delay.
unsafe fn await_from_thread(isolate: usize, value: i32) -> Result<()> {
    AWAIT_OBSERVATION.with(|cell| cell.set(ffi::AwaitObservation::Pending));
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    let (sender, receiver) = futures::channel::oneshot::channel();
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(20));
        let _ = sender.send(value);
    });
    let observer = DropObserver;
    IoContext::current(lock)
        .and_then(|context| {
            context.await_io(receiver, move |lock, received| {
                let _observer = observer;
                record_await(lock, received.unwrap_or(-1));
            })
        })
        .map_err(|err| to_io(&err))
}

/// In a request, awaits a future that never completes.
unsafe fn await_forever(isolate: usize) -> Result<()> {
    AWAIT_OBSERVATION.with(|cell| cell.set(ffi::AwaitObservation::Pending));
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    let observer = DropObserver;
    IoContext::current(lock)
        .and_then(|context| {
            context.await_io(std::future::pending::<i32>(), move |lock, value| {
                let _observer = observer;
                record_await(lock, value);
            })
        })
        .map_err(|err| to_io(&err))
}

fn await_observation() -> ffi::AwaitObservation {
    AWAIT_OBSERVATION.with(Cell::get)
}

fn awaited_value() -> i32 {
    AWAITED_VALUE.with(Cell::get)
}

static OWNED_DROPPED: AtomicBool = AtomicBool::new(false);

/// Sets `OWNED_DROPPED` when dropped.
struct DropFlag;

impl Drop for DropFlag {
    fn drop(&mut self) {
        OWNED_DROPPED.store(true, Ordering::SeqCst);
    }
}

/// Gives the current request a value, whose handle this thread keeps.
unsafe fn add_owned_value(isolate: usize) -> Result<()> {
    OWNED_DROPPED.store(false, Ordering::SeqCst);
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    let owned = IoContext::current(lock)
        .and_then(|context| context.add_object(DropFlag))
        .map_err(|err| to_io(&err))?;
    OWNED.with(|cell| *cell.borrow_mut() = Some(owned));
    Ok(())
}

/// Whether the value `add_owned_value()` gave its request has been dropped.
fn owned_value_dropped() -> bool {
    OWNED_DROPPED.load(Ordering::SeqCst)
}

/// Takes the value `add_owned_value()` gave its request and drops it, then drops the handle;
/// false if there was no value.
fn take_owned_value() -> bool {
    let owned = OWNED.with(|cell| cell.borrow_mut().take());
    owned.as_ref().and_then(IoOwned::take).is_some()
}

/// Drops the handle `add_owned_value()` kept, without taking its value.
fn drop_owned_handle() {
    OWNED.with(|cell| cell.borrow_mut().take());
}

/// In a request, awaits a future that is already complete, and from its callback has C++ record
/// the async context frame that is current.
unsafe fn await_and_record_async_context(isolate: usize) -> Result<()> {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    IoContext::current(lock)
        .and_then(|context| {
            context.await_io(std::future::ready(()), move |_lock, ()| {
                ffi::record_async_context(isolate);
            })
        })
        .map_err(|err| to_io(&err))
}
