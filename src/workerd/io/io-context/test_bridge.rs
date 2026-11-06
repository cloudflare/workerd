// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The Rust half of `io-context-test.c++` and `request-ops-test.c++`, which call these functions
//! inside and outside a request of a real worker.
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

use execution_context::new_gate;
use execution_context::new_rust_execution_context;
use execution_context::open_gate;
use futures::channel::oneshot;
use io_context::IoContext;
use io_context::IoOwned;
use io_context::SpanBuilder;
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
        include!("workerd/rust/jsg/v8.rs.h");

        type ModuleRegistry = jsg::v8::ffi::ModuleRegistry;
        type Isolate = jsg::v8::ffi::Isolate;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
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
        /// The callback returns what calling the function `keep_js_function()` kept returns.
        CallJs,
        /// The callback terminates execution, then calls the function `keep_js_function()` kept:
        /// the termination takes effect when V8 next runs JavaScript, which calling a Rust
        /// function doesn't.
        Terminate,
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
        unsafe fn awaited_promise(isolate: usize) -> Result<usize>;
        unsafe fn keep_js_function(isolate: usize, function: usize);
        unsafe fn add_owned_value(isolate: usize) -> Result<()>;
        fn owned_value_dropped() -> bool;
        unsafe fn take_owned_value(isolate: usize) -> Result<bool>;
        fn drop_owned_handle();
        unsafe fn record_trace_spans(isolate: usize) -> Result<()>;
        unsafe fn hold_trace_span(isolate: usize) -> Result<()>;
        fn drop_held_trace_span();
        unsafe fn await_in_user_trace_span(isolate: usize) -> Result<()>;
        unsafe fn await_after_detached_user_trace_span(isolate: usize) -> Result<()>;

        pub unsafe fn new_rust_execution_context(isolate: *mut Isolate) -> MaybeLocal;
        pub unsafe fn new_gate(isolate: *mut Isolate) -> MaybeLocal;
        fn open_gate() -> bool;
        unsafe fn request_ops_while_terminating(isolate: usize) -> Result<()>;

        unsafe fn has_current(isolate: usize) -> bool;
        unsafe fn now_nanos(isolate: usize) -> Result<i64>;
        unsafe fn write_logfwdr(isolate: usize, channel: u32, message: &[u8]) -> Result<()>;
    }
}

/// In a request, terminates execution, then calls `IoContext::add_wait_until()` and
/// `IoContext::abort()` with a reason and without one, as Rust can after catching a termination,
/// and fails unless each fails with a pending error. C++ checks that the request was not aborted,
/// and cancels the termination.
unsafe fn request_ops_while_terminating(isolate: usize) -> Result<()> {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    let context = IoContext::current(lock).map_err(|err| to_io(&err))?;
    // Created before terminating, since V8 creates no objects while terminating.
    let promise = v8::PromiseResolver::new(lock)
        .map_err(|err| to_io(&err))?
        .promise();
    let reason = lock.null();
    let termination = lock.terminate_execution_now();
    assert!(termination.is_pending());

    let wait_until = context.add_wait_until(lock, promise);
    let abort_with_reason = context.abort(lock, Some(reason));
    let abort_without_reason = context.abort(lock, None);
    if wait_until.as_ref().is_err_and(jsg::Error::is_pending)
        && abort_with_reason.is_pending()
        && abort_without_reason.is_pending()
    {
        Ok(())
    } else {
        Err(std::io::Error::other(format!(
            "add_wait_until: {wait_until:?}, abort with a reason: {abort_with_reason:?}, abort \
             without one: {abort_without_reason:?}"
        )))
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
            Ok(jsg::Rc::new(lock, probe::Probe))
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
    /// The function the last `keep_js_function()` kept, until a callback calls it.
    static JS_FUNCTION: RefCell<Option<v8::Global<v8::Function>>> = const { RefCell::new(None) };
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
/// Forgets the promise an earlier await kept, which may be of an earlier test's isolate.
fn new_await() -> oneshot::Receiver<i32> {
    AWAITED_PROMISE.with(|cell| cell.borrow_mut().take());
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
        ffi::PromiseEnding::CallJs => take_js_function(lock).call(lock, None, &[]),
        ffi::PromiseEnding::Terminate => {
            let function = take_js_function(lock);
            lock.terminate_execution();
            function.call(lock, None, &[])
        }
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

/// The promise the last `await_promise()` or `await_value()` returned, as a `v8::Local` in the
/// caller's `HandleScope`.
unsafe fn awaited_promise(isolate: usize) -> Result<usize> {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    let promise = AWAITED_PROMISE.with(|cell| {
        cell.borrow()
            .as_ref()
            .map(|promise| promise.as_local(lock))
            .ok_or_else(|| std::io::Error::other("no promise has been awaited"))
    })?;
    // SAFETY: the handle is the caller's to use in its `HandleScope`, which outlives this call.
    Ok(unsafe { promise.into_ffi() }.ptr)
}

/// Keeps `function`, a `v8::Local<v8::Function>` in the caller's `HandleScope`, for the callback
/// of an `await_promise()` that calls it.
unsafe fn keep_js_function(isolate: usize, function: usize) {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    // SAFETY: the caller passes a function handle in its `HandleScope`.
    let function: v8::Local<'_, v8::Function> =
        unsafe { v8::Local::from_ffi(v8::ffi::Local { ptr: function }) };
    let function = function.to_global(lock);
    JS_FUNCTION.with(|cell| *cell.borrow_mut() = Some(function));
}

fn take_js_function<'t>(lock: &mut jsg::Lock<'t>) -> v8::Local<'t, v8::Function> {
    JS_FUNCTION
        .with(|cell| cell.borrow_mut().take())
        .expect("keep_js_function() has not been called")
        .as_local(lock)
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
    static HELD_SPAN: RefCell<Option<SpanBuilder>> = const { RefCell::new(None) };
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

/// In a request, makes the spans `io-context-test.c++` expects, with a tag of each kind, and ends
/// them.
unsafe fn record_trace_spans(isolate: usize) -> Result<()> {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    let context = IoContext::current(lock).map_err(|err| to_io(&err))?;
    let mut span = context
        .make_trace_span(c"rust_span")
        .map_err(|err| to_io(&err))?;
    span.set_tag(c"bool", true);
    span.set_tag(c"int", 42);
    span.set_tag(c"float", 1.5);
    let dynamic = String::from("dynamic");
    span.set_tag(c"str", &dynamic);
    span.set_tag(c"literal", c"literal");
    let mut child = span.new_child(c"rust_child");
    child.set_tag(c"unsigned", 2_u32);
    let mut user = context
        .make_user_trace_span(c"rust_user_span")
        .map_err(|err| to_io(&err))?;
    user.set_tag(c"bool", false);
    user.set_tag(c"int", 7_i64);
    user.set_tag(c"float", 0.5);
    user.set_tag(c"str", "user");
    user.set_tag(c"literal", c"user_literal");
    Ok(())
}

/// In a request, starts a span that this thread keeps until `drop_held_trace_span()`.
unsafe fn hold_trace_span(isolate: usize) -> Result<()> {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    let span = IoContext::current(lock)
        .and_then(|context| context.make_trace_span(c"held_span"))
        .map_err(|err| to_io(&err))?;
    HELD_SPAN.with(|cell| *cell.borrow_mut() = Some(span));
    Ok(())
}

/// Drops the span `hold_trace_span()` started.
fn drop_held_trace_span() {
    HELD_SPAN.with(|cell| cell.borrow_mut().take());
}

/// In a request, starts a user trace span, then awaits a future that is already complete, ending
/// the span in the callback.
unsafe fn await_in_user_trace_span(isolate: usize) -> Result<()> {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    IoContext::current(lock)
        .and_then(|context| {
            let span = context.make_user_trace_span(c"rust_operation")?;
            context.await_io_and_run(std::future::ready(()), move |_lock, ()| {
                drop(span);
                Ok(())
            })
        })
        .map_err(|err| to_io(&err))
}

/// In a request, starts a user trace span and detaches it from async tracing, then awaits a future
/// that is already complete, ending the span in the callback.
unsafe fn await_after_detached_user_trace_span(isolate: usize) -> Result<()> {
    // SAFETY: see the module documentation.
    jsg::callback_scope!(unsafe let lock, isolate as *mut v8::ffi::Isolate);
    IoContext::current(lock)
        .and_then(|context| {
            let mut span = context.make_user_trace_span(c"rust_detached")?;
            span.detach_async();
            context.await_io_and_run(std::future::ready(()), move |_lock, ()| {
                drop(span);
                Ok(())
            })
        })
        .map_err(|err| to_io(&err))
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

/// Resources for the JavaScript of `request-ops-test.c++`, in a module of their own because the
/// resource macros expect the prelude's `Result`.
mod execution_context {
    use std::cell::RefCell;

    use io_context::IoContext;
    use jsg::ToJS;
    use jsg::v8;
    use jsg_macros::jsg_method;
    use jsg_macros::jsg_resource;

    /// `ctx.waitUntil()`, `ctx.passThroughOnException()` and `ctx.abort()`, implemented as a Rust
    /// `ExecutionContext` would implement them, for `request-ops-test.c++` to compare with the C++
    /// `ExecutionContext`.
    #[jsg_resource(name = "ExecutionContext")]
    struct RustExecutionContext;

    #[jsg_resource]
    impl RustExecutionContext {
        #[jsg_method]
        fn wait_until<'s>(
            &self,
            lock: &mut jsg::Lock<'s>,
            promise: v8::VoidPromise<'s>,
        ) -> jsg::Result<()> {
            IoContext::current(lock)?.add_wait_until(lock, promise)
        }

        #[jsg_method]
        fn pass_through_on_exception(&self, lock: &mut jsg::Lock) -> jsg::Result<()> {
            IoContext::current(lock)?.set_fail_open()
        }

        #[jsg_method]
        fn abort<'s>(
            &self,
            lock: &mut jsg::Lock<'s>,
            reason: Option<v8::Local<'s, v8::Value>>,
        ) -> jsg::Result<()> {
            Err(IoContext::current(lock)?.abort(lock, reason))
        }
    }

    /// Creates a `RustExecutionContext` for C++ to hand to JavaScript.
    ///
    /// # Safety
    /// C++ calls this with the isolate locked, a context entered and a `HandleScope` open.
    pub unsafe fn new_rust_execution_context(
        isolate: *mut v8::ffi::Isolate,
    ) -> v8::ffi::MaybeLocal {
        // SAFETY: forwarded from the caller.
        unsafe {
            v8::run_ffi_callback(isolate, |lock| {
                jsg::Rc::new(lock, RustExecutionContext).to_js(lock)
            })
        }
    }

    thread_local! {
        /// What opens the gate the last `Gate.wait()` returned a promise for.
        static GATE: RefCell<Option<futures::channel::oneshot::Sender<()>>> =
            const { RefCell::new(None) };
    }

    /// A promise that only C++ settles, through `open_gate()`, as I/O would.
    #[jsg_resource]
    struct Gate;

    #[jsg_resource]
    impl Gate {
        /// Returns a promise that resolves, in the request, once C++ calls `open_gate()`.
        #[jsg_method]
        fn wait<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Promise>> {
            let resolver = v8::PromiseResolver::new(lock)?;
            let promise = resolver.promise();
            let resolver = v8::Global::new(lock, resolver);
            let (sender, receiver) = futures::channel::oneshot::channel();
            GATE.with(|cell| *cell.borrow_mut() = Some(sender));
            IoContext::current(lock)?.await_io_and_run(receiver, move |lock, _| {
                let resolver = resolver.as_local(lock);
                let undefined = lock.undefined();
                resolver.resolve(lock, undefined)
            })?;
            Ok(promise)
        }
    }

    /// Creates a `Gate` for C++ to hand to JavaScript.
    ///
    /// # Safety
    /// As for `new_rust_execution_context()`.
    pub unsafe fn new_gate(isolate: *mut v8::ffi::Isolate) -> v8::ffi::MaybeLocal {
        // SAFETY: forwarded from the caller.
        unsafe { v8::run_ffi_callback(isolate, |lock| jsg::Rc::new(lock, Gate).to_js(lock)) }
    }

    /// Opens the gate of the last `Gate.wait()`; false if there was none.
    pub fn open_gate() -> bool {
        GATE.with(|cell| cell.borrow_mut().take())
            .is_some_and(|sender| sender.send(()).is_ok())
    }
}
