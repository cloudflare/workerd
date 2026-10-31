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
use std::future::Future;
use std::pin::Pin;
use std::sync::Mutex;
use std::sync::PoisonError;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::AtomicU8;
use std::sync::atomic::Ordering;
use std::time::UNIX_EPOCH;

use futures::channel::oneshot;
use io_context::IoContext;
use io_context::IoOwned;
use jsg::FromJS;
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

    /// How the last `await_released()` ends once its future has a value.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum AwaitEnding {
        /// The callback succeeds.
        Succeed,
        /// The callback returns an error.
        Fail,
        /// The callback panics.
        PanicInCallback,
        /// The future panics, so the callback never runs.
        PanicInFuture,
    }

    /// What the callback of the last `await_released()` did.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum AwaitObservation {
        /// It has not run, and still exists.
        Pending,
        /// It ran.
        Ran,
        /// It was dropped without running.
        Dropped,
    }

    /// How the callback of the last `await_promise()` ends once its future has a value.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum PromiseEnding {
        /// The callback returns the value.
        Value,
        /// The callback returns a promise resolved with the value plus one.
        Promise,
        /// The callback returns an error.
        Fail,
        /// The callback calls a function that throws.
        Throw,
        /// The callback panics.
        PanicInCallback,
        /// The future panics, so the callback never runs.
        PanicInFuture,
        /// The future panics when it is dropped, after its output is taken or when it is
        /// cancelled.
        PanicDroppingFuture,
    }

    unsafe extern "C++" {
        include!("workerd/io/io-context/test-bridge.h");

        fn record_async_context(isolate: usize);
        fn record_await_context();
    }

    extern "Rust" {
        unsafe fn await_and_record_async_context(isolate: usize) -> Result<()>;
        fn register_probe_module(registry: Pin<&mut ModuleRegistry>);
        fn take_probe_observation() -> ProbeObservation;
        unsafe fn await_released(isolate: usize, ending: AwaitEnding) -> Result<()>;
        fn release_await(value: i32);
        fn release_await_from_thread(value: i32);
        fn await_observation() -> AwaitObservation;
        fn awaited_value() -> i32;
        fn awaited_future_dropped() -> bool;
        unsafe fn await_promise(isolate: usize, ending: PromiseEnding) -> Result<()>;
        unsafe fn await_value(isolate: usize, fail: bool) -> Result<()>;
        unsafe fn awaited_promise_outcome(isolate: usize) -> Result<String>;
        unsafe fn add_owned_value(isolate: usize) -> Result<()>;
        fn owned_value_dropped() -> bool;
        unsafe fn take_owned_value(isolate: usize) -> Result<bool>;
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

/// What the callback of the last `await_released()` did, as an `AwaitObservation`.
static AWAIT_OBSERVATION: AtomicU8 = AtomicU8::new(ffi::AwaitObservation::Pending.repr);
static AWAITED_VALUE: Mutex<i32> = Mutex::new(0);
/// Whether the future of the last `await_released()` has been dropped.
static AWAITED_FUTURE_DROPPED: AtomicBool = AtomicBool::new(false);
/// What completes the future of the last `await_released()`.
static AWAIT_SENDER: Mutex<Option<oneshot::Sender<i32>>> = Mutex::new(None);

/// Sets the observation to `Dropped` when dropped while it is still `Pending`, that is, if the
/// callback holding it never ran.
struct CallbackDropObserver;

impl Drop for CallbackDropObserver {
    fn drop(&mut self) {
        let _ = AWAIT_OBSERVATION.compare_exchange(
            ffi::AwaitObservation::Pending.repr,
            ffi::AwaitObservation::Dropped.repr,
            Ordering::SeqCst,
            Ordering::SeqCst,
        );
    }
}

/// Sets `AWAITED_FUTURE_DROPPED` when dropped.
struct FutureDropObserver;

impl Drop for FutureDropObserver {
    fn drop(&mut self) {
        AWAITED_FUTURE_DROPPED.store(true, Ordering::SeqCst);
    }
}

/// In a request, awaits a value that `release_await()` or `release_await_from_thread()` sends,
/// with a callback that records the value and has C++ record the `IoContext` it runs in, then
/// ends as `ending` says.
unsafe fn await_released(isolate: usize, ending: ffi::AwaitEnding) -> Result<()> {
    AWAIT_OBSERVATION.store(ffi::AwaitObservation::Pending.repr, Ordering::SeqCst);
    AWAITED_FUTURE_DROPPED.store(false, Ordering::SeqCst);
    let (sender, receiver) = oneshot::channel();
    *AWAIT_SENDER.lock().unwrap_or_else(PoisonError::into_inner) = Some(sender);

    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    let future_observer = FutureDropObserver;
    let future = async move {
        let _observer = future_observer;
        let value = receiver.await.unwrap_or(-1);
        assert!(
            ending != ffi::AwaitEnding::PanicInFuture,
            "the future panicked"
        );
        value
    };
    let callback_observer = CallbackDropObserver;
    IoContext::current(lock)
        .and_then(|context| {
            context.await_io_and_run(future, move |_lock, value| {
                let _observer = callback_observer;
                ffi::record_await_context();
                *AWAITED_VALUE.lock().unwrap_or_else(PoisonError::into_inner) = value;
                AWAIT_OBSERVATION.store(ffi::AwaitObservation::Ran.repr, Ordering::SeqCst);
                match ending {
                    ffi::AwaitEnding::Fail => {
                        Err(jsg::Error::new_type_error("the callback failed"))
                    }
                    ffi::AwaitEnding::PanicInCallback => panic!("the callback panicked"),
                    _ => Ok(()),
                }
            })
        })
        .map_err(|err| to_io(&err))
}

thread_local! {
    /// The promise the last `await_promise()` or `await_value()` returned.
    static AWAITED_PROMISE: RefCell<Option<v8::Global<v8::Promise>>> = const { RefCell::new(None) };
}

/// A future that panics when it is dropped.
struct PanicOnDrop<F>(Pin<Box<F>>);

impl<F: Future> Future for PanicOnDrop<F> {
    type Output = F::Output;

    fn poll(
        mut self: Pin<&mut Self>,
        cx: &mut std::task::Context<'_>,
    ) -> std::task::Poll<F::Output> {
        self.0.as_mut().poll(cx)
    }
}

impl<F> Drop for PanicOnDrop<F> {
    fn drop(&mut self) {
        panic!("the future panicked when dropped");
    }
}

/// Starts waiting, as `await_released()` does, and returns a new oneshot receiver for the value.
fn new_await() -> oneshot::Receiver<i32> {
    AWAIT_OBSERVATION.store(ffi::AwaitObservation::Pending.repr, Ordering::SeqCst);
    AWAITED_FUTURE_DROPPED.store(false, Ordering::SeqCst);
    let (sender, receiver) = oneshot::channel();
    *AWAIT_SENDER.lock().unwrap_or_else(PoisonError::into_inner) = Some(sender);
    receiver
}

fn keep_promise<'s>(lock: &mut jsg::Lock<'s>, promise: v8::Local<'s, v8::Promise>) {
    let promise = promise.to_global(lock);
    AWAITED_PROMISE.with(|cell| *cell.borrow_mut() = Some(promise));
}

/// In a request, `IoContext::await_io()`s a value that `release_await()` or
/// `release_await_from_thread()` sends, then ends as `ending` says. `awaited_promise_outcome()`
/// tells how the promise it returned settled.
unsafe fn await_promise(isolate: usize, ending: ffi::PromiseEnding) -> Result<()> {
    let receiver = new_await();
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    let future_observer = FutureDropObserver;
    let future = async move {
        let _observer = future_observer;
        let value = receiver.await.unwrap_or(-1);
        assert!(
            ending != ffi::PromiseEnding::PanicInFuture,
            "the future panicked"
        );
        value
    };
    let result = IoContext::current(lock).and_then(|context| {
        if ending == ffi::PromiseEnding::PanicDroppingFuture {
            start_await_io(lock, &context, PanicOnDrop(Box::pin(future)), ending)
        } else {
            start_await_io(lock, &context, future, ending)
        }
    });
    let promise = result.map_err(|err| to_io(&err))?;
    keep_promise(lock, promise);
    Ok(())
}

/// `context.await_io(lock, future, ..)` with a callback that ends as `ending` says.
fn start_await_io<'s, F>(
    lock: &mut jsg::Lock<'s>,
    context: &IoContext<'s>,
    future: F,
    ending: ffi::PromiseEnding,
) -> jsg::Result<v8::Local<'s, v8::Promise>>
where
    F: Future<Output = i32> + Send + 'static,
{
    let callback_observer = CallbackDropObserver;
    context.await_io(lock, future, move |lock, value| {
        let _observer = callback_observer;
        AWAIT_OBSERVATION.store(ffi::AwaitObservation::Ran.repr, Ordering::SeqCst);
        finish(lock, ending, value)
    })
}

fn finish<'t>(
    lock: &mut jsg::Lock<'t>,
    ending: ffi::PromiseEnding,
    value: i32,
) -> jsg::Result<v8::Local<'t, v8::Value>> {
    match ending {
        ffi::PromiseEnding::Promise => {
            let next = f64::from(value + 1).to_js(lock)?;
            Ok(v8::Promise::resolved(lock, next)?.into())
        }
        ffi::PromiseEnding::Fail => Err(jsg::Error::new_type_error("the callback failed")),
        ffi::PromiseEnding::Throw => {
            let thrower = v8::Function::new(lock, (), |_lock, (), _args| {
                Err(jsg::Error::new_range_error("thrown by JavaScript"))
            })?;
            thrower.call(lock, None, &[])
        }
        ffi::PromiseEnding::PanicInCallback => panic!("the callback panicked"),
        _ => f64::from(value).to_js(lock),
    }
}

/// In a request, `IoContext::await_io_value()`s a value that `release_await()` sends, which the
/// future returns, or fails with a `TypeError` if `fail` is set.
unsafe fn await_value(isolate: usize, fail: bool) -> Result<()> {
    let receiver = new_await();
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    let future = async move {
        let value = receiver.await.unwrap_or(-1);
        if fail {
            Err(jsg::Error::new_type_error("the future failed"))
        } else {
            Ok(f64::from(value))
        }
    };
    let promise = IoContext::current(lock)
        .and_then(|context| context.await_io_value(lock, future))
        .map_err(|err| to_io(&err))?;
    keep_promise(lock, promise);
    Ok(())
}

/// How the promise the last `await_promise()` or `await_value()` returned has settled: "pending",
/// "fulfilled: <number>" or "rejected: <name>: <message>".
unsafe fn awaited_promise_outcome(isolate: usize) -> Result<String> {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    let promise = AWAITED_PROMISE.with(|cell| {
        cell.borrow()
            .as_ref()
            .map(|promise| promise.as_local(lock))
            .ok_or_else(|| std::io::Error::other("no promise has been awaited"))
    })?;
    let Some(result) = promise.result(lock) else {
        return Ok("pending".to_owned());
    };
    if promise.state() == v8::PromiseState::Fulfilled {
        let value = f64::from_js(lock, result).map_err(|err| to_io(&err))?;
        Ok(format!("fulfilled: {value}"))
    } else {
        let error = jsg::Error::from_js(lock, result).map_err(|err| to_io(&err))?;
        Ok(format!("rejected: {error}"))
    }
}

fn take_await_sender() -> oneshot::Sender<i32> {
    AWAIT_SENDER
        .lock()
        .unwrap_or_else(PoisonError::into_inner)
        .take()
        .expect("await_released() has not been called, or was already released")
}

/// Completes the future of the last `await_released()` with `value`, on this thread.
fn release_await(value: i32) {
    let _ = take_await_sender().send(value);
}

/// Completes the future of the last `await_released()` with `value`, on a new thread.
fn release_await_from_thread(value: i32) {
    let sender = take_await_sender();
    std::thread::spawn(move || {
        let _ = sender.send(value);
    });
}

fn await_observation() -> ffi::AwaitObservation {
    ffi::AwaitObservation {
        repr: AWAIT_OBSERVATION.load(Ordering::SeqCst),
    }
}

fn awaited_value() -> i32 {
    *AWAITED_VALUE.lock().unwrap_or_else(PoisonError::into_inner)
}

fn awaited_future_dropped() -> bool {
    AWAITED_FUTURE_DROPPED.load(Ordering::SeqCst)
}

thread_local! {
    static OWNED: RefCell<Option<IoOwned<DropFlag>>> = const { RefCell::new(None) };
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

/// Takes the value `add_owned_value()` gave its request and drops it, keeping the handle; false
/// if it was already taken.
unsafe fn take_owned_value(isolate: usize) -> Result<bool> {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    OWNED.with(|cell| {
        let owned = cell.borrow();
        let owned = owned
            .as_ref()
            .ok_or_else(|| std::io::Error::other("add_owned_value() has not been called"))?;
        Ok(owned.take(lock).map_err(|err| to_io(&err))?.is_some())
    })
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
            context.await_io_and_run(std::future::ready(()), move |_lock, ()| {
                ffi::record_async_context(isolate);
                Ok(())
            })
        })
        .map_err(|err| to_io(&err))
}
